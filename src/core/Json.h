#pragma once
// JSON writer/parser - minimal dependency-free implementation sufficient
// for the project.paper serialization format used by V1.
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>

namespace pf {

class JsonValue;
using JsonObject = std::map<std::string, JsonValue, std::less<>>;
using JsonArray = std::vector<JsonValue>;

class JsonValue {
public:
    enum class Type { Null, Bool, Number, String, Array, Object };

    JsonValue() : type_(Type::Null) {}
    JsonValue(bool b) : type_(Type::Bool), data_(b) {}
    JsonValue(int v) : type_(Type::Number), data_(static_cast<double>(v)) {}
    JsonValue(std::int64_t v) : type_(Type::Number), data_(static_cast<double>(v)) {}
    JsonValue(std::uint64_t v) : type_(Type::Number), data_(static_cast<double>(v)) {}
    JsonValue(double v) : type_(Type::Number), data_(v) {}
    JsonValue(const char* s) : type_(Type::String), data_(std::string(s)) {}
    JsonValue(std::string s) : type_(Type::String), data_(std::move(s)) {}
    JsonValue(JsonArray a) : type_(Type::Array), data_(std::move(a)) {}
    JsonValue(JsonObject o) : type_(Type::Object), data_(std::move(o)) {}

    Type type() const noexcept { return type_; }
    bool is_null() const noexcept { return type_ == Type::Null; }
    bool is_bool() const noexcept { return type_ == Type::Bool; }
    bool is_number() const noexcept { return type_ == Type::Number; }
    bool is_string() const noexcept { return type_ == Type::String; }
    bool is_array() const noexcept { return type_ == Type::Array; }
    bool is_object() const noexcept { return type_ == Type::Object; }

    bool as_bool(bool def = false) const {
        return is_bool() ? std::get<bool>(data_) : def;
    }
    double as_double(double def = 0.0) const {
        return is_number() ? std::get<double>(data_) : def;
    }
    std::int64_t as_int(std::int64_t def = 0) const {
        return is_number() ? static_cast<std::int64_t>(std::get<double>(data_)) : def;
    }
    const std::string& as_string() const {
        static const std::string kEmpty;
        return is_string() ? std::get<std::string>(data_) : kEmpty;
    }
    const JsonArray& as_array() const {
        static const JsonArray kEmpty;
        return is_array() ? std::get<JsonArray>(data_) : kEmpty;
    }
    const JsonObject& as_object() const {
        static const JsonObject kEmpty;
        return is_object() ? std::get<JsonObject>(data_) : kEmpty;
    }

    // Object convenience (returns null JsonValue if missing / not object)
    const JsonValue* find(const std::string& key) const {
        if (!is_object()) return nullptr;
        auto it = std::get<JsonObject>(data_).find(key);
        return it == std::get<JsonObject>(data_).end() ? nullptr : &it->second;
    }

    void set(const std::string& key, JsonValue v) {
        EnsureObject();
        std::get<JsonObject>(data_)[key] = std::move(v);
    }
    void push_back(JsonValue v) {
        EnsureArray();
        std::get<JsonArray>(data_).push_back(std::move(v));
    }

    std::string Dump(int indent = 2) const;

private:
    void EnsureObject() {
        if (type_ != Type::Object) {
            type_ = Type::Object;
            data_ = JsonObject{};
        }
    }
    void EnsureArray() {
        if (type_ != Type::Array) {
            type_ = Type::Array;
            data_ = JsonArray{};
        }
    }

    void DumpTo(std::string& out, int depth, int indent) const;

    Type type_;
    std::variant<std::monostate, bool, double, std::string, JsonArray, JsonObject> data_;
};

// Parse a JSON document. On failure returns nullptr and (when requested)
// writes a human-readable reason into *error.
//
// Resource bounds (P0-02): parsing is bounded by JsonParseLimits - input
// size, nesting depth, total node count and total string bytes. Any input
// that exceeds a bound is rejected with a structured error message; the
// parser never aborts the process and never lets an exception escape
// (stack-overflow and numeric-conversion crashes closed here).
struct JsonParseLimits {
    std::size_t max_input_bytes = 8 * 1024 * 1024;
    std::size_t max_depth = 128;
    std::size_t max_nodes = 200000;
    std::size_t max_string_bytes = 2 * 1024 * 1024;
};

std::unique_ptr<JsonValue> JsonParse(const std::string& text, std::string* error = nullptr);
std::unique_ptr<JsonValue> JsonParse(const std::string& text, std::string* error,
                                     const JsonParseLimits& limits);
std::string JsonEscape(const std::string& s);

}  // namespace pf
