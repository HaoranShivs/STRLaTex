#include "core/Json.h"

#include <cctype>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <new>
#include <stdexcept>

namespace pf {

std::string JsonEscape(const std::string& s) {
    std::string out;
    out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        switch (c) {
            case '"': out += "\\\""; break;
            case '\\': out += "\\\\"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            case '\b': out += "\\b"; break;
            case '\f': out += "\\f"; break;
            default:
                if (c < 0x20) {
                    char buf[8];
                    std::snprintf(buf, sizeof(buf), "\\u%04x", c);
                    out += buf;
                } else {
                    out += static_cast<char>(c);
                }
        }
    }
    return out;
}

void JsonValue::DumpTo(std::string& out, int depth, int indent) const {
    auto pad = [&](int d) {
        if (indent <= 0) return;
        out.push_back('\n');
        out.append(static_cast<size_t>(d) * indent, ' ');
    };
    switch (type_) {
        case Type::Null: out += "null"; break;
        case Type::Bool: out += std::get<bool>(data_) ? "true" : "false"; break;
        case Type::Number: {
            double v = std::get<double>(data_);
            if (std::floor(v) == v && std::abs(v) < 1e15) {
                out += std::to_string(static_cast<long long>(v));
            } else {
                char buf[32];
                std::snprintf(buf, sizeof(buf), "%.17g", v);
                out += buf;
            }
            break;
        }
        case Type::String:
            out += '"';
            out += JsonEscape(std::get<std::string>(data_));
            out += '"';
            break;
        case Type::Array: {
            const auto& arr = std::get<JsonArray>(data_);
            out += '[';
            bool first = true;
            for (const auto& item : arr) {
                if (!first) out += ',';
                first = false;
                pad(depth + 1);
                item.DumpTo(out, depth + 1, indent);
            }
            if (!arr.empty()) pad(depth);
            out += ']';
            break;
        }
        case Type::Object: {
            const auto& obj = std::get<JsonObject>(data_);
            out += '{';
            bool first = true;
            for (const auto& [key, value] : obj) {
                if (!first) out += ',';
                first = false;
                pad(depth + 1);
                out += '"';
                out += JsonEscape(key);
                out += "\": ";
                value.DumpTo(out, depth + 1, indent);
            }
            if (!obj.empty()) pad(depth);
            out += '}';
            break;
        }
    }
}

std::string JsonValue::Dump(int indent) const {
    std::string out;
    DumpTo(out, 0, indent);
    out.push_back('\n');
    return out;
}

// ---------------- 解析器 ----------------

namespace {

class Parser {
public:
    struct ParseAbort {};

    explicit Parser(const std::string& text, std::string* error,
                    const JsonParseLimits& limits)
        : text_(text), error_(error), limits_(limits) {}

    std::unique_ptr<JsonValue> Parse() {
        try {
            if (text_.size() > limits_.max_input_bytes) {
                Fail("input exceeds maximum size (" +
                     std::to_string(limits_.max_input_bytes) + " bytes)");
            }
            SkipWs();
            auto value = ParseValue();
            if (!value) return nullptr;
            SkipWs();
            if (pos_ != text_.size()) {
                Fail("trailing characters at offset " + std::to_string(pos_));
            }
            return value;
        } catch (const ParseAbort&) {
            return nullptr;
        } catch (const std::bad_alloc&) {
            // 限额判定出现竞态（例如两次检查之间字符串增长）时必须退化为
            // 一次干净的解析失败，绝不能导致进程终止。
            if (error_) *error_ = "out of memory while parsing";
            return nullptr;
        }
    }

private:
    [[noreturn]] void Fail(const std::string& msg) {
        if (error_) *error_ = msg;
        throw ParseAbort{};
    }

    void SkipWs() {
        while (pos_ < text_.size()) {
            char c = text_[pos_];
            if (c == ' ' || c == '\t' || c == '\n' || c == '\r') {
                ++pos_;
            } else {
                break;
            }
        }
    }

    char Peek() {
        if (pos_ >= text_.size()) {
            Fail("unexpected end of input");
        }
        return text_[pos_];
    }

    char Next() {
        char c = Peek();
        ++pos_;
        return c;
    }

    void Expect(char c) {
        if (Next() != c) {
            --pos_;
            Fail(std::string("expected '") + c + "' at offset " + std::to_string(pos_));
        }
    }

    void NoteNode() {
        ++node_count_;
        if (node_count_ > limits_.max_nodes) {
            Fail("node count exceeds limit (" +
                 std::to_string(limits_.max_nodes) + ")");
        }
    }

    void ExpectLiteral(const char* lit) {
        for (const char* p = lit; *p; ++p) {
            if (Next() != *p) Fail(std::string("bad literal, expected ") + lit);
        }
    }

    std::unique_ptr<JsonValue> ParseValue() {
        char c = Peek();
        switch (c) {
            case '{': return ParseObject();
            case '[': return ParseArray();
            case '"': return std::make_unique<JsonValue>(ParseString());
            case 't': ExpectLiteral("true"); NoteNode(); return std::make_unique<JsonValue>(true);
            case 'f': ExpectLiteral("false"); NoteNode(); return std::make_unique<JsonValue>(false);
            case 'n': ExpectLiteral("null"); NoteNode(); return std::make_unique<JsonValue>();
            default: return ParseNumber();
        }
    }

    // 作用域深度守卫：嵌套超过限额属于解析错误，而不是
    // 栈溢出。
    class DepthGuard {
    public:
        DepthGuard(Parser& parser, std::size_t& depth) : parser_(parser), depth_(depth) {
            ++depth_;
            if (depth_ > parser_.limits_.max_depth) {
                parser_.Fail("nesting depth exceeds limit (" +
                             std::to_string(parser_.limits_.max_depth) + ")");
            }
        }
        ~DepthGuard() { --depth_; }
        DepthGuard(const DepthGuard&) = delete;
        DepthGuard& operator=(const DepthGuard&) = delete;

    private:
        Parser& parser_;
        std::size_t& depth_;
    };

    std::unique_ptr<JsonValue> ParseObject() {
        DepthGuard guard(*this, depth_);
        Expect('{');
        auto obj = std::make_unique<JsonValue>(JsonObject{});
        NoteNode();
        SkipWs();
        if (Peek() == '}') { Next(); return obj; }
        while (true) {
            SkipWs();
            std::string key = ParseString();
            SkipWs();
            Expect(':');
            SkipWs();
            auto value = ParseValue();
            obj->set(key, std::move(*value));
            SkipWs();
            char c = Next();
            if (c == '}') break;
            if (c != ',') Fail("expected ',' or '}' in object");
        }
        return obj;
    }

    std::unique_ptr<JsonValue> ParseArray() {
        DepthGuard guard(*this, depth_);
        Expect('[');
        auto arr = std::make_unique<JsonValue>(JsonArray{});
        NoteNode();
        SkipWs();
        if (Peek() == ']') { Next(); return arr; }
        while (true) {
            SkipWs();
            auto value = ParseValue();
            arr->push_back(std::move(*value));
            SkipWs();
            char c = Next();
            if (c == ']') break;
            if (c != ',') Fail("expected ',' or ']' in array");
        }
        return arr;
    }

    std::string ParseString() {
        Expect('"');
        std::string out;
        while (true) {
            char c = Next();
            if (c == '"') break;
            if (c == '\\') {
                char esc = Next();
                switch (esc) {
                    case '"': out += '"'; break;
                    case '\\': out += '\\'; break;
                    case '/': out += '/'; break;
                    case 'n': out += '\n'; break;
                    case 't': out += '\t'; break;
                    case 'r': out += '\r'; break;
                    case 'b': out += '\b'; break;
                    case 'f': out += '\f'; break;
                    case 'u': out += ParseUnicodeEscape(); break;
                    default: Fail("bad escape character");
                }
            } else {
                out += c;
            }
            // 限制累积后字符串的大小，而不只是原始输入，这样一份由 \u 转义
            // 组成的小文档也无法无限膨胀。
            if (out.size() > limits_.max_string_bytes) {
                Fail("string exceeds maximum length (" +
                     std::to_string(limits_.max_string_bytes) + " bytes)");
            }
        }
        return out;
    }

    std::string ParseUnicodeEscape() {
        unsigned cp = ParseHex4();
        // 代理对处理
        if (cp >= 0xD800 && cp <= 0xDBFF) {
            if (Next() == '\\' && Next() == 'u') {
                unsigned low = ParseHex4();
                if (low >= 0xDC00 && low <= 0xDFFF) {
                    cp = 0x10000 + ((cp - 0xD800) << 10) + (low - 0xDC00);
                } else {
                    Fail("invalid surrogate pair");
                }
            } else {
                Fail("invalid surrogate pair");
            }
        }
        // UTF-8 编码
        std::string out;
        if (cp < 0x80) {
            out += static_cast<char>(cp);
        } else if (cp < 0x800) {
            out += static_cast<char>(0xC0 | (cp >> 6));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else if (cp < 0x10000) {
            out += static_cast<char>(0xE0 | (cp >> 12));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        } else {
            out += static_cast<char>(0xF0 | (cp >> 18));
            out += static_cast<char>(0x80 | ((cp >> 12) & 0x3F));
            out += static_cast<char>(0x80 | ((cp >> 6) & 0x3F));
            out += static_cast<char>(0x80 | (cp & 0x3F));
        }
        return out;
    }

    unsigned ParseHex4() {
        unsigned v = 0;
        for (int i = 0; i < 4; ++i) {
            char c = Next();
            v <<= 4;
            if (c >= '0' && c <= '9') v |= c - '0';
            else if (c >= 'a' && c <= 'f') v |= c - 'a' + 10;
            else if (c >= 'A' && c <= 'F') v |= c - 'A' + 10;
            else Fail("bad hex digit in \\u escape");
        }
        return v;
    }

    std::unique_ptr<JsonValue> ParseNumber() {
        NoteNode();
        size_t start = pos_;
        if (Peek() == '-') Next();
        if (Peek() == '0') {
            Next();
        } else {
            char c = Peek();
            if (c < '1' || c > '9') Fail("invalid number");
            while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) Next();
        }
        bool is_float = false;
        if (pos_ < text_.size() && text_[pos_] == '.') {
            is_float = true;
            Next();
            while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) Next();
        }
        if (pos_ < text_.size() && (text_[pos_] == 'e' || text_[pos_] == 'E')) {
            is_float = true;
            Next();
            if (pos_ < text_.size() && (text_[pos_] == '+' || text_[pos_] == '-')) Next();
            while (pos_ < text_.size() && isdigit(static_cast<unsigned char>(text_[pos_]))) Next();
        }
        std::string num = text_.substr(start, pos_ - start);

        // 结构化的数字转换：任何失败都是解析错误。这里不使用裸的
        // std::stod / std::stoll —— 它们的异常曾会逃出解析器，
        // 并在 "1e999" 这类输入上使进程崩溃。
        if (is_float) {
            try {
                double v = std::stod(num);
                if (!std::isfinite(v)) {
                    // "1e999" 能解析但会溢出为 +inf：直接拒绝。JSON 中
                    // 无法表示 NaN，因此这里只可能是溢出。
                    Fail("number overflow out of representable range: " + num);
                }
                return std::make_unique<JsonValue>(v);
            } catch (const std::invalid_argument&) {
                Fail("invalid number: " + num);
            } catch (const std::out_of_range&) {
                Fail("number overflow out of representable range: " + num);
            }
        }
        // 整数快速路径，带完整消费与溢出检查；无法装入 std::int64_t 的
        // 值会回退到下面（有防护的）double 转换。
        std::int64_t parsed = 0;
        bool fits = false;
        try {
            size_t consumed = 0;
            parsed = std::stoll(num, &consumed);
            fits = consumed == num.size();
        } catch (const std::out_of_range&) {
            fits = false;
        } catch (const std::invalid_argument&) {
            fits = false;
        }
        if (fits) {
            return std::make_unique<JsonValue>(parsed);
        }
        try {
            double v = std::stod(num);
            if (!std::isfinite(v)) {
                Fail("number overflow out of representable range: " + num);
            }
            return std::make_unique<JsonValue>(v);
        } catch (const std::invalid_argument&) {
            Fail("invalid number: " + num);
        } catch (const std::out_of_range&) {
            Fail("number overflow out of representable range: " + num);
        }
    }

    const std::string& text_;
    std::string* error_;
    JsonParseLimits limits_;
    size_t pos_ = 0;
    std::size_t depth_ = 0;
    std::size_t node_count_ = 0;
};

}  // namespace

std::unique_ptr<JsonValue> JsonParse(const std::string& text, std::string* error) {
    JsonParseLimits defaults;
    return JsonParse(text, error, defaults);
}

std::unique_ptr<JsonValue> JsonParse(const std::string& text, std::string* error,
                                     const JsonParseLimits& limits) {
    Parser p(text, error, limits);
    return p.Parse();
}

}  // namespace pf
