#include "persistence/ProjectPersistence.h"

#include <cstdio>
#include <fstream>
#include <sstream>

#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "document/InlineText.h"

namespace pf {

// ---------------- Serialize ----------------

namespace {

JsonValue InlineToJson(const InlineContent& content) {
    JsonArray arr;
    for (const auto& node : content) {
        JsonObject obj;
        if (const auto* run = std::get_if<TextRun>(&node)) {
            obj["type"] = "text";
            obj["text"] = run->text;
            if (run->marks) obj["marks"] = static_cast<std::int64_t>(run->marks);
        } else if (const auto* eq = std::get_if<InlineEquation>(&node)) {
            obj["type"] = "inlineEquation";
            obj["math"] = eq->math_source;
        } else if (const auto* cit = std::get_if<Citation>(&node)) {
            obj["type"] = "citation";
            JsonArray keys;
            for (const auto& k : cit->keys) keys.push_back(k);
            obj["keys"] = std::move(keys);
            obj["mode"] = cit->mode == CitationMode::Narrative ? "narrative"
                                                               : "parenthetical";
        } else if (const auto* ref = std::get_if<CrossReference>(&node)) {
            obj["type"] = "crossReference";
            obj["target"] = ref->target.value();
        }
        arr.push_back(JsonValue(std::move(obj)));
    }
    return JsonValue(std::move(arr));
}

std::optional<InlineContent> InlineFromJson(const JsonValue* value) {
    if (!value || !value->is_array()) return std::nullopt;
    InlineContent content;
    for (const auto& item : value->as_array()) {
        const auto* type = item.find("type");
        if (!type || !type->is_string()) return std::nullopt;
        const std::string& t = type->as_string();
        if (t == "text") {
            TextRun run;
            if (const auto* text = item.find("text")) run.text = text->as_string();
            if (const auto* marks = item.find("marks"))
                run.marks = static_cast<std::uint8_t>(marks->as_int());
            content.push_back(std::move(run));
        } else if (t == "inlineEquation") {
            InlineEquation eq;
            if (const auto* m = item.find("math")) eq.math_source = m->as_string();
            content.push_back(std::move(eq));
        } else if (t == "citation") {
            Citation cit;
            if (const auto* keys = item.find("keys")) {
                for (const auto& k : keys->as_array()) cit.keys.push_back(k.as_string());
            }
            if (const auto* mode = item.find("mode")) {
                cit.mode = mode->as_string() == "narrative" ? CitationMode::Narrative
                                                            : CitationMode::Parenthetical;
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
        cols.push_back(std::string(col.alignment == ColumnAlignment::Left
                                       ? "left"
                                       : col.alignment == ColumnAlignment::Center
                                             ? "center"
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
    if (!value || !value->is_object()) return std::nullopt;
    Table table;
    if (const auto* id = value->find("id")) table.id = NodeId(id->as_string());
    if (const auto* cap = value->find("caption")) {
        table.caption = InlineFromJson(cap).value_or(InlineContent{});
    }
    if (const auto* hdr = value->find("hasHeaderRow"))
        table.has_header_row = hdr->as_bool();
    if (const auto* cols = value->find("columns")) {
        for (const auto& c : cols->as_array()) {
            TableColumn col;
            const std::string& a = c.as_string();
            col.alignment = a == "center"   ? ColumnAlignment::Center
                            : a == "right"  ? ColumnAlignment::Right
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
    if (table.columns.empty() || !table.IsRectangular()) return std::nullopt;
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
            case FigureWidth::Percent25: w = "25"; break;
            case FigureWidth::Percent50: w = "50"; break;
            case FigureWidth::Percent75: w = "75"; break;
            case FigureWidth::Percent100: w = "100"; break;
        }
        obj["width"] = w;
    } else if (const auto* table = std::get_if<Table>(&block)) {
        JsonObject t = TableToJson(*table).as_object();
        t["type"] = "table";
        return JsonValue(std::move(t));
    } else if (const auto* eq = std::get_if<DisplayEquation>(&block)) {
        obj["type"] = "displayEquation";
        obj["id"] = eq->id.value();
        obj["math"] = eq->math_source;
        obj["numbered"] = eq->numbered;
    }
    return JsonValue(std::move(obj));
}

std::optional<Block> BlockFromJson(const JsonValue* value) {
    if (!value || !value->is_object()) return std::nullopt;
    const auto* type = value->find("type");
    if (!type) return std::nullopt;
    const std::string& t = type->as_string();
    if (t == "paragraph") {
        Paragraph para;
        if (const auto* id = value->find("id")) para.id = NodeId(id->as_string());
        if (const auto* content = value->find("content")) {
            para.content = InlineFromJson(content).value_or(InlineContent{});
        }
        return para;
    }
    if (t == "figure") {
        Figure fig;
        if (const auto* id = value->find("id")) fig.id = NodeId(id->as_string());
        if (const auto* asset = value->find("assetId"))
            fig.asset_id = AssetId(asset->as_string());
        if (const auto* cap = value->find("caption")) {
            fig.caption = InlineFromJson(cap).value_or(InlineContent{});
        }
        if (const auto* alt = value->find("alt")) fig.alt_text = alt->as_string();
        if (const auto* w = value->find("width")) {
            const std::string& ws = w->as_string();
            fig.width = ws == "25"   ? FigureWidth::Percent25
                        : ws == "50" ? FigureWidth::Percent50
                        : ws == "75" ? FigureWidth::Percent75
                                     : FigureWidth::Percent100;
        }
        return fig;
    }
    if (t == "table") {
        return TableFromJson(value);
    }
    if (t == "displayEquation") {
        DisplayEquation eq;
        if (const auto* id = value->find("id")) eq.id = NodeId(id->as_string());
        if (const auto* m = value->find("math")) eq.math_source = m->as_string();
        if (const auto* n = value->find("numbered")) eq.numbered = n->as_bool();
        return eq;
    }
    return std::nullopt;
}

JsonValue BlocksToJson(const std::vector<Block>& blocks) {
    JsonArray arr;
    for (const auto& b : blocks) arr.push_back(BlockToJson(b));
    return JsonValue(std::move(arr));
}

bool BlocksFromJson(const JsonValue* value, std::vector<Block>* out) {
    if (!value || !value->is_array()) return false;
    for (const auto& item : value->as_array()) {
        auto block = BlockFromJson(&item);
        if (!block) return false;
        out->push_back(std::move(*block));
    }
    return true;
}

}  // namespace

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
        if (author.email) a["email"] = *author.email;
        JsonArray affs;
        for (const auto& aff : author.affiliations) affs.push_back(aff.value());
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
    for (const auto& kw : fm.keywords) keywords.push_back(kw);
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

Result<SerializedProject, std::string> ProjectSerializer::Deserialize(
    const std::string& json_text) {
    std::string error;
    auto root = JsonParse(json_text, &error);
    if (!root) return Unexpected("JSON parse error: " + error);
    if (!root->is_object()) return Unexpected("root is not an object");

    SerializedProject project;
    if (const auto* v = root->find("schemaVersion")) {
        project.schema_version = v->as_string();
    }
    if (const auto* v = root->find("projectId")) {
        project.project_id = v->as_string();
    }
    if (const auto* v = root->find("revision")) {
        project.revision = ProjectRevision{static_cast<std::uint64_t>(v->as_int())};
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
            if (!t) return Unexpected("bad title");
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
                if (const auto* id = item.find("id")) aff.id = AffiliationId(id->as_string());
                if (const auto* n = item.find("name")) aff.name = n->as_string();
                project.document.front_matter().affiliations.push_back(std::move(aff));
            }
        }
        if (const auto* authors = front->find("authors")) {
            for (const auto& item : authors->as_array()) {
                Author author;
                if (const auto* n = item.find("name")) author.name = n->as_string();
                if (const auto* e = item.find("email")) author.email = e->as_string();
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
                    if (!BlocksFromJson(blocks, &section.blocks)) {
                        return Unexpected("bad block in section " + section.id.value());
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
                            if (!BlocksFromJson(blocks, &sub.blocks)) {
                                return Unexpected("bad block in subsection " +
                                                  sub.id.value());
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
            if (const auto* id = item.find("id")) meta.id = AssetId(id->as_string());
            if (const auto* p = item.find("path")) meta.relative_path = p->as_string();
            if (const auto* m = item.find("mediaType")) meta.media_type = m->as_string();
            if (const auto* o = item.find("originalName"))
                meta.original_name = o->as_string();
            if (const auto* s = item.find("fileSize"))
                meta.file_size = static_cast<std::uint64_t>(s->as_int());
            if (const auto* h = item.find("hash")) meta.content_hash = h->as_string();
            if (const auto* w = item.find("width"))
                meta.width = static_cast<std::uint64_t>(w->as_int());
            if (const auto* hh = item.find("height"))
                meta.height = static_cast<std::uint64_t>(hh->as_int());
            project.assets.push_back(std::move(meta));
        }
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
    std::filesystem::create_directories(dest.parent_path(), ec);

    // Atomic write: temp file then rename.
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
    // Validate round-trip before replace.
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
    std::ifstream in(request.project_file, std::ios::binary);
    if (!in) {
        result.status = LoadResult::Status::FileMissing;
        result.detail = "cannot open " + request.project_file.string();
        return result;
    }
    std::ostringstream ss;
    ss << in.rdbuf();
    auto project = ProjectSerializer::Deserialize(ss.str());
    if (!project) {
        result.status = LoadResult::Status::ParseError;
        result.detail = project.error();
        return result;
    }
    result.project = std::move(project.value());
    result.status = LoadResult::Status::Ok;
    return result;
}

}  // namespace pf
