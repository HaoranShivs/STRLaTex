#include "persistence/ProjectPersistence.h"

#include <cmath>
#include <cstdio>
#include <fstream>
#include <set>
#include <sstream>
#include <system_error>

#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "persistence/ProjectMigrator.h"

namespace pf {

namespace {

// P0-02：对每个反序列化后的 project 强制施加的硬性不变量。.paper 文件属于
// 不可信输入；超出这些界限的内容一律以结构化错误拒绝，而不是将其载入
// （或留到之后崩溃）。
constexpr std::size_t kMaxStringFieldBytes = 64 * 1024; // 单个文本字段
constexpr std::size_t kMaxTableRows = 100;
constexpr std::size_t kMaxTableColumns = 50;
constexpr std::size_t kMaxNodes = 200000;

bool CheckStringField(const std::string& value) {
    return value.size() <= kMaxStringFieldBytes;
}

std::string DescribeTableProblem(const Table& table) {
    if (table.columns.empty())
        return "table has no columns";
    if (table.columns.size() > kMaxTableColumns)
        return "table exceeds column limit (max 50)";
    if (table.RowCount() > kMaxTableRows)
        return "table exceeds row limit (max 100)";
    if (!table.IsRectangular())
        return "table cells do not match declared column count";
    return {};
}

// 对反序列化后的整个 project 做 schema 校验。project 合法时返回空字符串，
// 否则返回人类可读的原因。校验范围包括：id（非空且唯一）、节层级结构、
// 表格不变量、字符串长度、引用目标以及 revision 的合理性。
std::string ValidateSerializedProject(const SerializedProject& project) {
    // revision 溢出防护：revision 按顺序生成。
    if (project.revision.value > std::uint64_t{1} << 48)
        return "revision out of range";

    std::set<std::string> seen_ids;
    std::set<std::string> reference_targets; // 可被交叉引用的 id
    std::size_t node_count = 0;
    auto count_id = [&](const NodeId& id, const char* what) -> std::string {
        if (++node_count > kMaxNodes)
            return "document exceeds node limit";
        if (!id.empty() && !seen_ids.insert(id.value()).second)
            return "duplicate " + std::string(what) + " id: " + id.value();
        return {};
    };

    const FrontMatter& fm = project.document.front_matter();
    if (!CheckStringField(project.project_id))
        return "projectId too long";
    if (!CheckStringField(project.template_id))
        return "template id too long";
    if (!CheckStringField(InlineToPlainText(fm.title)))
        return "title too long";
    if (fm.keywords.size() > 100)
        return "too many keywords";
    for (const auto& kw : fm.keywords) {
        if (!CheckStringField(kw))
            return "keyword too long";
    }

    for (const auto& section : project.document.body().sections) {
        if (auto err = count_id(section.id, "section"); !err.empty())
            return err;
        if (!CheckStringField(InlineToPlainText(section.title)))
            return "section title too long";
        for (const auto& block : section.blocks) {
            if (const auto* table = std::get_if<Table>(&block)) {
                if (auto err = count_id(table->id, "table"); !err.empty())
                    return err;
                if (auto problem = DescribeTableProblem(*table); !problem.empty())
                    return problem;
                reference_targets.insert(table->id.value());
            } else if (const auto* figure = std::get_if<Figure>(&block)) {
                if (auto err = count_id(figure->id, "figure"); !err.empty())
                    return err;
                if (!CheckStringField(figure->alt_text))
                    return "figure alt text too long";
                reference_targets.insert(figure->id.value());
            } else if (const auto* equation = std::get_if<EquationBlock>(&block)) {
                if (auto err = count_id(equation->id, "equation"); !err.empty())
                    return err;
                if (!CheckStringField(equation->expression.latex))
                    return "equation latex too long";
                reference_targets.insert(equation->id.value());
            } else if (const auto* paragraph = std::get_if<Paragraph>(&block)) {
                if (auto err = count_id(paragraph->id, "paragraph"); !err.empty())
                    return err;
            }
        }
        for (const auto& sub : section.subsections) {
            if (auto err = count_id(sub.id, "subsection"); !err.empty())
                return err;
            if (!CheckStringField(InlineToPlainText(sub.title)))
                return "subsection title too long";
            for (const auto& block : sub.blocks) {
                if (const auto* table = std::get_if<Table>(&block)) {
                    if (auto err = count_id(table->id, "table"); !err.empty())
                        return err;
                    if (auto problem = DescribeTableProblem(*table); !problem.empty())
                        return problem;
                    reference_targets.insert(table->id.value());
                } else if (const auto* figure = std::get_if<Figure>(&block)) {
                    if (auto err = count_id(figure->id, "figure"); !err.empty())
                        return err;
                    reference_targets.insert(figure->id.value());
                } else if (const auto* equation = std::get_if<EquationBlock>(&block)) {
                    if (auto err = count_id(equation->id, "equation"); !err.empty())
                        return err;
                    reference_targets.insert(equation->id.value());
                } else if (const auto* paragraph = std::get_if<Paragraph>(&block)) {
                    if (auto err = count_id(paragraph->id, "paragraph"); !err.empty())
                        return err;
                }
            }
            for (const auto& subsub : sub.subsubsections) {
                if (auto err = count_id(subsub.id, "subsubsection"); !err.empty())
                    return err;
                for (const auto& block : subsub.blocks) {
                    if (const auto* table = std::get_if<Table>(&block)) {
                        if (auto err = count_id(table->id, "table"); !err.empty())
                            return err;
                        if (auto problem = DescribeTableProblem(*table); !problem.empty())
                            return problem;
                        reference_targets.insert(table->id.value());
                    } else if (const auto* figure = std::get_if<Figure>(&block)) {
                        if (auto err = count_id(figure->id, "figure"); !err.empty())
                            return err;
                        reference_targets.insert(figure->id.value());
                    } else if (const auto* equation = std::get_if<EquationBlock>(&block)) {
                        if (auto err = count_id(equation->id, "equation"); !err.empty())
                            return err;
                        reference_targets.insert(equation->id.value());
                    } else if (const auto* paragraph = std::get_if<Paragraph>(&block)) {
                        if (auto err = count_id(paragraph->id, "paragraph"); !err.empty())
                            return err;
                    }
                }
            }
        }
    }

    // 交叉引用必须指向一个存在且可被引用的节点。
    for (const auto& section : project.document.body().sections) {
        auto check_ref = [&](const CrossReference& ref) -> std::string {
            if (ref.target.empty())
                return {};
            if (reference_targets.count(ref.target.value()) == 0)
                return "cross-reference target does not exist: " + ref.target.value();
            return {};
        };
        auto walk_inline = [&](const InlineContent& content) -> std::string {
            for (const auto& node : content) {
                if (const auto* ref = std::get_if<CrossReference>(&node)) {
                    if (auto err = check_ref(*ref); !err.empty())
                        return err;
                }
            }
            return {};
        };
        if (auto err = walk_inline(section.title); !err.empty())
            return err;
        for (const auto& block : section.blocks) {
            if (const auto* para = std::get_if<Paragraph>(&block)) {
                if (auto err = walk_inline(para->content); !err.empty())
                    return err;
            } else if (const auto* table = std::get_if<Table>(&block)) {
                if (auto err = walk_inline(table->caption); !err.empty())
                    return err;
                for (const auto& row : table->cells)
                    for (const auto& cell : row) {
                        if (auto err = walk_inline(cell.content); !err.empty())
                            return err;
                    }
            } else if (const auto* figure = std::get_if<Figure>(&block)) {
                if (auto err = walk_inline(figure->caption); !err.empty())
                    return err;
            }
        }
    }

    // asset 表：id 唯一，声明的路径为项目内相对路径。
    std::set<std::string> asset_ids;
    for (const auto& asset : project.assets) {
        if (!asset_ids.insert(asset.id.value()).second)
            return "duplicate asset id: " + asset.id.value();
        if (!CheckStringField(asset.relative_path))
            return "asset path too long";
        auto parsed = ProjectRelativePath::Parse(asset.relative_path);
        if (!parsed.ok())
            return "asset path is not a safe project-relative path: " + PathErrorMessage(parsed.error());
    }

    auto bib = ProjectRelativePath::Parse(project.bibliography_path);
    if (!bib.ok())
        return "bibliography path is not a safe project-relative path: " + PathErrorMessage(bib.error());

    return {};
}

} // namespace

// ---------------- Serialize ----------------

namespace {

JsonValue InlineToJson(const InlineContent& content) {
    JsonArray arr;
    for (const auto& node : content) {
        JsonObject obj;
        if (const auto* run = std::get_if<TextRun>(&node)) {
            obj["type"] = "text";
            obj["text"] = run->text;
            if (run->marks)
                obj["marks"] = static_cast<std::int64_t>(run->marks);
        } else if (const auto* eq = std::get_if<InlineMath>(&node)) {
            // 设计 §10：document 只存储数学公式主体。
            obj["type"] = "inline_math";
            obj["latex"] = eq->expression.latex;
        } else if (const auto* cit = std::get_if<Citation>(&node)) {
            obj["type"] = "citation";
            JsonArray keys;
            for (const auto& k : cit->keys)
                keys.push_back(k);
            obj["keys"] = std::move(keys);
            obj["mode"] = cit->mode == CitationMode::Narrative ? "narrative" : "parenthetical";
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            obj["type"] = "crossReference";
            obj["target"] = ref->target.value();
        }
        arr.push_back(JsonValue(std::move(obj)));
    }
    return JsonValue(std::move(arr));
}

std::optional<InlineContent> InlineFromJson(const JsonValue* value) {
    if (!value || !value->is_array())
        return std::nullopt;
    InlineContent content;
    for (const auto& item : value->as_array()) {
        const auto* type = item.find("type");
        if (!type || !type->is_string())
            return std::nullopt;
        const std::string& t = type->as_string();
        if (t == "text") {
            TextRun run;
            if (const auto* text = item.find("text"))
                run.text = text->as_string();
            if (const auto* marks = item.find("marks"))
                run.marks = static_cast<std::uint8_t>(marks->as_int());
            content.push_back(std::move(run));
        } else if (t == "inline_math" || t == "inlineEquation") {
            // "inlineEquation"/"math" 是重构前的旧拼写；继续读取它，
            // 以便既有项目保持原样打开。
            InlineMath eq;
            if (const auto* m = item.find("latex")) {
                eq.expression.latex = m->as_string();
            } else if (const auto* m = item.find("math")) {
                eq.expression.latex = m->as_string();
            }
            content.push_back(std::move(eq));
        } else if (t == "citation") {
            Citation cit;
            if (const auto* keys = item.find("keys")) {
                for (const auto& k : keys->as_array())
                    cit.keys.push_back(k.as_string());
            }
            if (const auto* mode = item.find("mode")) {
                cit.mode = mode->as_string() == "narrative" ? CitationMode::Narrative : CitationMode::Parenthetical;
            }
            content.push_back(std::move(cit));
        } else if (t == "crossReference") {
            CrossReference ref;
            if (const auto* target = item.find("target")) {
                ref.target = NodeId(target->as_string());
            }
            content.push_back(std::move(ref));
        } else {
            return std::nullopt;
        }
    }
    return content;
}

JsonValue TableToJson(const Table& table) {
    JsonObject obj;
    obj["id"] = table.id.value();
    obj["caption"] = InlineToJson(table.caption);
    obj["hasHeaderRow"] = table.has_header_row;
    JsonArray cols;
    for (const auto& col : table.columns) {
        cols.push_back(std::string(col.alignment == ColumnAlignment::Left     ? "left"
                                   : col.alignment == ColumnAlignment::Center ? "center"
                                                                              : "right"));
    }
    obj["columns"] = std::move(cols);
    JsonArray rows;
    for (const auto& row : table.cells) {
        JsonArray row_arr;
        for (const auto& cell : row) {
            row_arr.push_back(InlineToJson(cell.content));
        }
        rows.push_back(std::move(row_arr));
    }
    obj["cells"] = std::move(rows);
    return JsonValue(std::move(obj));
}

std::optional<Table> TableFromJson(const JsonValue* value) {
    if (!value || !value->is_object())
        return std::nullopt;
    Table table;
    if (const auto* id = value->find("id"))
        table.id = NodeId(id->as_string());
    if (const auto* cap = value->find("caption")) {
        table.caption = InlineFromJson(cap).value_or(InlineContent{});
    }
    if (const auto* hdr = value->find("hasHeaderRow"))
        table.has_header_row = hdr->as_bool();
    if (const auto* cols = value->find("columns")) {
        for (const auto& c : cols->as_array()) {
            TableColumn col;
            const std::string& a = c.as_string();
            col.alignment = a == "center"  ? ColumnAlignment::Center
                            : a == "right" ? ColumnAlignment::Right
                                           : ColumnAlignment::Left;
            table.columns.push_back(col);
        }
    }
    if (const auto* cells = value->find("cells")) {
        for (const auto& row : cells->as_array()) {
            std::vector<TableCell> row_cells;
            for (const auto& cell : row.as_array()) {
                TableCell tc;
                tc.content = InlineFromJson(&cell).value_or(InlineContent{});
                row_cells.push_back(std::move(tc));
            }
            table.cells.push_back(std::move(row_cells));
        }
    }
    if (table.columns.empty() || !table.IsRectangular())
        return std::nullopt;
    return table;
}

JsonValue BlockToJson(const Block& block) {
    JsonObject obj;
    if (const auto* para = std::get_if<Paragraph>(&block)) {
        obj["type"] = "paragraph";
        obj["id"] = para->id.value();
        obj["content"] = InlineToJson(para->content);
    } else if (const auto* fig = std::get_if<Figure>(&block)) {
        obj["type"] = "figure";
        obj["id"] = fig->id.value();
        obj["assetId"] = fig->asset_id.value();
        obj["caption"] = InlineToJson(fig->caption);
        obj["alt"] = fig->alt_text;
        const char* w = "100";
        switch (fig->width) {
        case FigureWidth::Percent25:
            w = "25";
            break;
        case FigureWidth::Percent50:
            w = "50";
            break;
        case FigureWidth::Percent75:
            w = "75";
            break;
        case FigureWidth::Percent100:
            w = "100";
            break;
        }
        obj["width"] = w;
        // 单栏与双栏图。为每个 figure 都写入，使该选择在文件中显式可见；
        // 不识别该键的旧读取方会忽略它。
        obj["span"] = fig->span == FigureSpan::DoubleColumn ? "double" : "single";
    } else if (const auto* table = std::get_if<Table>(&block)) {
        JsonObject t = TableToJson(*table).as_object();
        t["type"] = "table";
        return JsonValue(std::move(t));
    } else if (const auto* eq = std::get_if<EquationBlock>(&block)) {
        // 设计 §10："equation" + latex/numbered/label；
        // 生成的环境文本从不存储。
        obj["type"] = "equation";
        obj["id"] = eq->id.value();
        obj["latex"] = eq->expression.latex;
        obj["numbered"] = eq->numbered;
        obj["label"] = eq->label;
    }
    return JsonValue(std::move(obj));
}

std::optional<Block> BlockFromJson(const JsonValue* value) {
    if (!value || !value->is_object())
        return std::nullopt;
    const auto* type = value->find("type");
    if (!type)
        return std::nullopt;
    const std::string& t = type->as_string();
    if (t == "paragraph") {
        Paragraph para;
        if (const auto* id = value->find("id"))
            para.id = NodeId(id->as_string());
        if (const auto* content = value->find("content")) {
            para.content = InlineFromJson(content).value_or(InlineContent{});
        }
        return para;
    }
    if (t == "figure") {
        Figure fig;
        if (const auto* id = value->find("id"))
            fig.id = NodeId(id->as_string());
        if (const auto* asset = value->find("assetId"))
            fig.asset_id = AssetId(asset->as_string());
        if (const auto* cap = value->find("caption")) {
            fig.caption = InlineFromJson(cap).value_or(InlineContent{});
        }
        if (const auto* alt = value->find("alt"))
            fig.alt_text = alt->as_string();
        if (const auto* w = value->find("width")) {
            const std::string& ws = w->as_string();
            fig.width = ws == "25"   ? FigureWidth::Percent25
                        : ws == "50" ? FigureWidth::Percent50
                        : ws == "75" ? FigureWidth::Percent75
                                     : FigureWidth::Percent100;
        }
        // 缺少该键（在属性存在之前写入的文件）表示普通的单栏图。
        if (const auto* span = value->find("span")) {
            fig.span = span->as_string() == "double" ? FigureSpan::DoubleColumn : FigureSpan::SingleColumn;
        }
        return fig;
    }
    if (t == "table") {
        return TableFromJson(value);
    }
    if (t == "equation" || t == "displayEquation") {
        EquationBlock eq;
        if (const auto* id = value->find("id"))
            eq.id = NodeId(id->as_string());
        if (const auto* m = value->find("latex")) {
            eq.expression.latex = m->as_string();
        } else if (const auto* m = value->find("math")) {
            eq.expression.latex = m->as_string(); // 重构前的旧拼写
        }
        if (const auto* n = value->find("numbered"))
            eq.numbered = n->as_bool();
        if (const auto* l = value->find("label"))
            eq.label = l->as_string();
        return eq;
    }
    return std::nullopt;
}

JsonValue BlocksToJson(const std::vector<Block>& blocks) {
    JsonArray arr;
    for (const auto& b : blocks)
        arr.push_back(BlockToJson(b));
    return JsonValue(std::move(arr));
}

bool BlocksFromJson(const JsonValue* value, std::vector<Block>* out) {
    if (!value || !value->is_array())
        return false;
    for (const auto& item : value->as_array()) {
        auto block = BlockFromJson(&item);
        if (!block)
            return false;
        out->push_back(std::move(*block));
    }
    return true;
}

// 带描述的包装：指出出错的 block 类型，使加载错误能说明问题所在，
// 而不是只给出干巴巴的 "bad block"。
std::optional<std::string> BlocksFromJsonDetailed(const JsonValue* value, std::vector<Block>* out) {
    if (!value || !value->is_array())
        return std::string("blocks field is not an array");
    for (const auto& item : value->as_array()) {
        auto block = BlockFromJson(&item);
        if (!block) {
            std::string kind = "unknown";
            if (const auto* type = item.find("type"))
                kind = type->as_string();
            if (kind.empty())
                kind = "unknown";
            return std::string("invalid ") + kind + " block";
        }
        out->push_back(std::move(*block));
    }
    return std::nullopt;
}

} // namespace

std::string ProjectSerializer::Serialize(const SerializedProject& project) {
    JsonObject root;
    root["schemaVersion"] = project.schema_version;
    root["projectId"] = project.project_id;
    root["revision"] = project.revision.value;
    root["template"] = project.template_id;
    root["bibliographyPath"] = project.bibliography_path;

    // FrontMatter
    JsonObject front;
    const auto& fm = project.document.front_matter();
    front["title"] = InlineToJson(fm.title);
    JsonArray authors;
    for (const auto& author : fm.authors) {
        JsonObject a;
        a["name"] = author.name;
        if (author.email)
            a["email"] = *author.email;
        JsonArray affs;
        for (const auto& aff : author.affiliations)
            affs.push_back(aff.value());
        a["affiliations"] = std::move(affs);
        authors.push_back(JsonValue(std::move(a)));
    }
    front["authors"] = std::move(authors);
    JsonArray affs;
    for (const auto& aff : fm.affiliations) {
        JsonObject a;
        a["id"] = aff.id.value();
        a["name"] = aff.name;
        affs.push_back(JsonValue(std::move(a)));
    }
    front["affiliations"] = std::move(affs);
    if (fm.abstract_text) {
        front["abstract"] = InlineToJson(*fm.abstract_text);
    }
    JsonArray keywords;
    for (const auto& kw : fm.keywords)
        keywords.push_back(kw);
    front["keywords"] = std::move(keywords);
    root["frontMatter"] = std::move(front);

    // Body
    JsonArray sections;
    for (const auto& section : project.document.body().sections) {
        JsonObject s;
        s["id"] = section.id.value();
        s["title"] = InlineToJson(section.title);
        s["blocks"] = BlocksToJson(section.blocks);
        JsonArray subs;
        for (const auto& sub : section.subsections) {
            JsonObject sub_obj;
            sub_obj["id"] = sub.id.value();
            sub_obj["title"] = InlineToJson(sub.title);
            sub_obj["blocks"] = BlocksToJson(sub.blocks);
            JsonArray subsubs;
            for (const auto& subsub : sub.subsubsections) {
                JsonObject subsub_obj;
                subsub_obj["id"] = subsub.id.value();
                subsub_obj["title"] = InlineToJson(subsub.title);
                subsub_obj["blocks"] = BlocksToJson(subsub.blocks);
                subsubs.push_back(JsonValue(std::move(subsub_obj)));
            }
            sub_obj["subsubsections"] = std::move(subsubs);
            subs.push_back(JsonValue(std::move(sub_obj)));
        }
        s["subsections"] = std::move(subs);
        sections.push_back(JsonValue(std::move(s)));
    }
    JsonObject body;
    body["sections"] = std::move(sections);
    root["body"] = std::move(body);

    // Assets
    JsonArray assets;
    for (const auto& asset : project.assets) {
        JsonObject a;
        a["id"] = asset.id.value();
        a["path"] = asset.relative_path;
        a["mediaType"] = asset.media_type;
        a["originalName"] = asset.original_name;
        a["fileSize"] = asset.file_size;
        a["hash"] = asset.content_hash;
        a["width"] = asset.width;
        a["height"] = asset.height;
        assets.push_back(JsonValue(std::move(a)));
    }
    root["assets"] = std::move(assets);

    JsonValue value(std::move(root));
    return value.Dump(2);
}

// ---------------- Deserialize ----------------

namespace {

// 修复在 id 生成器尚未感知已加载状态时保存的项目：当时计数器在每次打开时
// 都从零重新开始，因此插入已加载项目的第一个 block 会复用磁盘上已存在的
// id（例如两个节点都叫 "n1"）。id 重复会导致一次编辑解析到错误的 block，
// 并打乱插入顺序。首次出现的节点保留其 id；之后每个重复的 id 都会分配
// 一个新的。仅可在 ObserveIdsFromProject() 已将生成器推进到文件中最高 id
// 之后调用。
void HealDuplicateNodeIds(Document& document) {
    std::set<std::string> seen;
    auto heal = [&seen](NodeId& id) {
        if (id.empty())
            return;
        if (seen.insert(id.value()).second)
            return;
        id = IdGenerator::NewNode();
        seen.insert(id.value());
    };
    auto heal_blocks = [&heal](std::vector<Block>& blocks) {
        for (auto& block : blocks) {
            std::visit([&heal](auto& b) { heal(b.id); }, block);
        }
    };
    Body& body = DocumentMutableAccess::body(document);
    for (auto& section : body.sections) {
        heal(section.id);
        heal_blocks(section.blocks);
        for (auto& sub : section.subsections) {
            heal(sub.id);
            heal_blocks(sub.blocks);
            for (auto& subsub : sub.subsubsections) {
                heal(subsub.id);
                heal_blocks(subsub.blocks);
            }
        }
    }
}

// 将 id 生成器推进到该项目已包含的所有 id 之后，使打开项目后新生成的 id
// 不会与已存储的 id 冲突。
void ObserveIdsFromProject(const SerializedProject& project) {
    for (const auto& id : project.document.CollectNodeIds()) {
        IdGenerator::ObserveNodeId(id.value());
    }
    for (const auto& asset : project.assets) {
        IdGenerator::ObserveAssetId(asset.id.value());
    }
    for (const auto& affiliation : project.document.front_matter().affiliations) {
        IdGenerator::ObserveAffiliationId(affiliation.id.value());
    }
}

} // namespace

Result<SerializedProject, std::string> ProjectSerializer::Deserialize(const std::string& json_text) {
    // 异常屏障（P0-02）：损坏或恶意的负载必须以结构化错误字符串的形式
    // 呈现——绝不能表现为未捕获的异常。
    std::string error;
    std::unique_ptr<JsonValue> root;
    try {
        root = JsonParse(json_text, &error);
    } catch (const std::exception& e) {
        return Unexpected(std::string("JSON parse failure: ") + e.what());
    } catch (...) {
        return Unexpected(std::string("JSON parse failure: unknown error"));
    }
    if (!root)
        return Unexpected("JSON parse error: " + error);
    if (!root->is_object())
        return Unexpected("root is not an object");

    // schema 关卡（P0-02）：由更新的 schema 版本写入的文件必须硬失败——
    // 加载它会静默丢弃应用不认识的字段，而下次保存将销毁数据。较旧的版本
    // 可以加载并迁移。
    {
        std::string file_version;
        if (const auto* v = root->find("schemaVersion"))
            file_version = v->as_string();
        if (!file_version.empty() && !ProjectMigrator::IsKnownVersion(file_version)) {
            return Unexpected("unsupported schemaVersion " + file_version + " (this app supports up to " +
                              kSchemaVersion + ")");
        }
    }

    SerializedProject project;
    if (const auto* v = root->find("schemaVersion")) {
        project.schema_version = v->as_string();
    }
    if (const auto* v = root->find("projectId")) {
        project.project_id = v->as_string();
    }
    if (const auto* v = root->find("revision")) {
        // 大于 2^53 的 double 会丢失整数精度；此处改为截断，避免让
        // static_cast 成为未定义行为（UBSan 发现的问题）。
        const double raw = v->as_double();
        if (raw < 0.0 || !std::isfinite(raw) || raw >= 9007199254740992.0) {
            return Unexpected("revision out of representable range");
        }
        project.revision = ProjectRevision{static_cast<std::uint64_t>(raw)};
    }
    if (const auto* v = root->find("template")) {
        project.template_id = v->as_string();
    }
    if (const auto* v = root->find("bibliographyPath")) {
        project.bibliography_path = v->as_string();
    }

    DocumentEditor editor(project.document);

    // FrontMatter
    if (const auto* front = root->find("frontMatter")) {
        if (const auto* title = front->find("title")) {
            auto t = InlineFromJson(title);
            if (!t)
                return Unexpected("bad title");
            editor.SetTitle(*t);
        }
        if (const auto* abstract = front->find("abstract")) {
            editor.SetAbstract(InlineFromJson(abstract));
        }
        if (const auto* keywords = front->find("keywords")) {
            std::vector<std::string> kws;
            for (const auto& kw : keywords->as_array()) {
                kws.push_back(kw.as_string());
            }
            editor.SetKeywords(kws);
        }
        if (const auto* affs = front->find("affiliations")) {
            for (const auto& item : affs->as_array()) {
                Affiliation aff;
                if (const auto* id = item.find("id"))
                    aff.id = AffiliationId(id->as_string());
                if (const auto* n = item.find("name"))
                    aff.name = n->as_string();
                project.document.front_matter().affiliations.push_back(std::move(aff));
            }
        }
        if (const auto* authors = front->find("authors")) {
            for (const auto& item : authors->as_array()) {
                Author author;
                if (const auto* n = item.find("name"))
                    author.name = n->as_string();
                if (const auto* e = item.find("email"))
                    author.email = e->as_string();
                if (const auto* affs = item.find("affiliations")) {
                    for (const auto& a : affs->as_array()) {
                        author.affiliations.push_back(AffiliationId(a.as_string()));
                    }
                }
                project.document.front_matter().authors.push_back(std::move(author));
            }
        }
    }

    // Body
    if (const auto* body = root->find("body")) {
        if (const auto* sections = body->find("sections")) {
            for (const auto& s_item : sections->as_array()) {
                Section section;
                if (const auto* id = s_item.find("id"))
                    section.id = NodeId(id->as_string());
                if (const auto* t = s_item.find("title")) {
                    section.title = InlineFromJson(t).value_or(InlineContent{});
                }
                if (const auto* blocks = s_item.find("blocks")) {
                    if (auto problem = BlocksFromJsonDetailed(blocks, &section.blocks)) {
                        return Unexpected("section " + section.id.value() + ": " + *problem);
                    }
                }
                if (const auto* subs = s_item.find("subsections")) {
                    for (const auto& sub_item : subs->as_array()) {
                        Subsection sub;
                        if (const auto* id = sub_item.find("id"))
                            sub.id = NodeId(id->as_string());
                        if (const auto* t = sub_item.find("title")) {
                            sub.title = InlineFromJson(t).value_or(InlineContent{});
                        }
                        if (const auto* blocks = sub_item.find("blocks")) {
                            if (auto problem = BlocksFromJsonDetailed(blocks, &sub.blocks)) {
                                return Unexpected("subsection " + sub.id.value() + ": " + *problem);
                            }
                        }
                        if (const auto* subsubs = sub_item.find("subsubsections")) {
                            for (const auto& subsub_item : subsubs->as_array()) {
                                Subsubsection subsub;
                                if (const auto* id = subsub_item.find("id"))
                                    subsub.id = NodeId(id->as_string());
                                if (const auto* t = subsub_item.find("title")) {
                                    subsub.title = InlineFromJson(t).value_or(InlineContent{});
                                }
                                if (const auto* blocks = subsub_item.find("blocks")) {
                                    if (auto problem = BlocksFromJsonDetailed(blocks, &subsub.blocks)) {
                                        return Unexpected("subsubsection " + subsub.id.value() + ": " + *problem);
                                    }
                                }
                                sub.subsubsections.push_back(std::move(subsub));
                            }
                        }
                        section.subsections.push_back(std::move(sub));
                    }
                }
                project.document.body().sections.push_back(std::move(section));
            }
        }
    }

    // Assets
    if (const auto* assets = root->find("assets")) {
        for (const auto& item : assets->as_array()) {
            AssetMetadata meta;
            if (const auto* id = item.find("id"))
                meta.id = AssetId(id->as_string());
            if (const auto* p = item.find("path"))
                meta.relative_path = p->as_string();
            if (const auto* m = item.find("mediaType"))
                meta.media_type = m->as_string();
            if (const auto* o = item.find("originalName"))
                meta.original_name = o->as_string();
            if (const auto* s = item.find("fileSize")) {
                const double raw = s->as_double();
                // 超范围 double 改为截断，避免未定义行为（UBSan 发现的问题）。
                meta.file_size = (raw >= 0.0 && std::isfinite(raw) && raw < 9007199254740992.0)
                                     ? static_cast<std::uint64_t>(raw)
                                     : 0;
            }
            if (const auto* h = item.find("hash"))
                meta.content_hash = h->as_string();
            if (const auto* w = item.find("width")) {
                const double raw = w->as_double();
                meta.width = (raw >= 0.0 && std::isfinite(raw) && raw < 9007199254740992.0)
                                 ? static_cast<std::uint64_t>(raw)
                                 : 0;
            }
            if (const auto* hh = item.find("height")) {
                const double raw = hh->as_double();
                meta.height = (raw >= 0.0 && std::isfinite(raw) && raw < 9007199254740992.0)
                                  ? static_cast<std::uint64_t>(raw)
                                  : 0;
            }
            project.assets.push_back(std::move(meta));
        }
    }

    // id 安全（见上文的辅助函数）：先把生成器推进到所有已存储 id 之后，
    // 再修复旧生成器遗留的重复节点 id。两者都必须在 document 交给 session
    // 之前完成。
    ObserveIdsFromProject(project);
    HealDuplicateNodeIds(project.document);

    // schema 校验（P0-02）：结构、界限、id 唯一性、表格几何、
    // 路径安全与引用完整性。上文已修复的重复 id 允许保留；其余任何问题
    // 都是加载错误。
    if (std::string problem = ValidateSerializedProject(project); !problem.empty()) {
        return Unexpected("project validation failed: " + problem);
    }

    project.document.set_version(DocumentVersion{project.revision.value});
    return project;
}

// ---------------- Persistence I/O ----------------

SaveResult ProjectPersistence::Save(const SaveRequest& request) {
    SaveResult result;
    result.save_id = request.save_id;
    result.saved_revision = request.revision;

    std::string json = ProjectSerializer::Serialize(request.snapshot);
    if (json.empty()) {
        result.status = SaveResult::Status::SerializeError;
        result.detail = "serialization produced empty output";
        return result;
    }

    std::error_code ec;
    auto dest = request.destination;
    // 目标路径若指向一个已存在的目录，它绝不可能成为原子重命名的目标；
    // 此处必须显式失败，而不是产生令人困惑的重命名错误。
    if (std::filesystem::is_directory(dest, ec)) {
        result.status = SaveResult::Status::IoError;
        result.detail = "save destination is a directory: " + dest.string();
        return result;
    }
    std::filesystem::create_directories(dest.parent_path(), ec);

    // 原子写入：先写临时文件再重命名。
    auto temp = dest.string() + ".tmp-" + request.save_id;
    {
        std::ofstream out(temp, std::ios::binary | std::ios::trunc);
        if (!out) {
            result.status = SaveResult::Status::IoError;
            result.detail = "cannot open temp file: " + temp;
            return result;
        }
        out << json;
        out.flush();
        if (!out.good()) {
            result.status = SaveResult::Status::IoError;
            result.detail = "write failed";
            std::remove(temp.c_str());
            return result;
        }
    }
    // 在替换之前校验往返读写。
    {
        std::ifstream in(temp, std::ios::binary);
        std::ostringstream ss;
        ss << in.rdbuf();
        auto round_trip = ProjectSerializer::Deserialize(ss.str());
        if (!round_trip) {
            result.status = SaveResult::Status::SerializeError;
            result.detail = "round-trip validation failed: " + round_trip.error();
            std::remove(temp.c_str());
            return result;
        }
    }
    if (std::rename(temp.c_str(), dest.string().c_str()) != 0) {
        result.status = SaveResult::Status::IoError;
        result.detail = "atomic replace failed";
        std::remove(temp.c_str());
        return result;
    }
    result.status = SaveResult::Status::Ok;
    return result;
}

LoadResult ProjectPersistence::Load(const LoadRequest& request) {
    LoadResult result;
    // P0-02：读取之前先做大小关卡。损坏或恶意的文件无法让加载器
    // 分配不受限制的内存。
    std::error_code ec;
    const auto file_size = std::filesystem::file_size(request.project_file, ec);
    if (ec) {
        // 区分文件缺失与文件不可读。
        result.status = std::filesystem::exists(request.project_file, ec) ? LoadResult::Status::IoError
                                                                          : LoadResult::Status::FileMissing;
        result.detail = "cannot stat " + request.project_file.string();
        return result;
    }
    if (file_size > kMaxProjectFileBytes) {
        result.status = LoadResult::Status::TooLarge;
        result.detail = "project file too large (" + std::to_string(file_size) + " bytes, limit " +
                        std::to_string(kMaxProjectFileBytes) + ")";
        return result;
    }

    std::ifstream in(request.project_file, std::ios::binary);
    if (!in) {
        result.status = LoadResult::Status::FileMissing;
        result.detail = "cannot open " + request.project_file.string();
        return result;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    if (!in.good() && !in.eof()) {
        result.status = LoadResult::Status::IoError;
        result.detail = "read failed: " + request.project_file.string();
        return result;
    }
    auto project = ProjectSerializer::Deserialize(ss.str());
    if (!project) {
        result.status = LoadResult::Status::ParseError;
        result.detail = project.error();
        return result;
    }
    // 旧 schema 文件在内存中迁移；磁盘上的文件在用户保存之前保持不变。
    auto migration = ProjectMigrator::MigrateToCurrent(&project.value());
    result.migration = std::move(migration);
    result.project = std::move(project.value());
    result.status = LoadResult::Status::Ok;
    return result;
}

} // namespace pf
