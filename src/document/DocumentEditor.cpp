#include "document/DocumentEditor.h"

#include <stdexcept>

#include "core/IdGenerator.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"

namespace pf {

const char* ToString(EditError error) {
    switch (error) {
        case EditError::InvalidTarget: return "InvalidTarget";
        case EditError::ConstraintViolation: return "ConstraintViolation";
        case EditError::NotFound: return "NotFound";
    }
    return "Unknown";
}

DocumentEditor::DocumentEditor(Document& document) : document_(document) {}

void DocumentEditor::Throw(EditError e) const {
    throw std::runtime_error(ToString(e));
}

// ---------------- FrontMatter ----------------

Result<void, EditError> DocumentEditor::SetTitle(const InlineContent& title) {
    document_.front_matter().title = title;
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::SetAbstract(
    const std::optional<InlineContent>& abstract_text) {
    document_.front_matter().abstract_text = abstract_text;
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::SetKeywords(
    const std::vector<std::string>& keywords) {
    document_.front_matter().keywords = keywords;
    document_.BumpVersion();
    return {};
}

Result<AffiliationId, EditError> DocumentEditor::AddAffiliation(const std::string& name,
                                                                AffiliationId id) {
    if (id.empty()) id = AffiliationId(IdGenerator::NewAffiliationId());
    Affiliation aff;
    aff.id = id;
    aff.name = name;
    document_.front_matter().affiliations.push_back(std::move(aff));
    document_.BumpVersion();
    return id;
}

Result<void, EditError> DocumentEditor::RemoveAffiliation(const AffiliationId& id) {
    auto& affs = document_.front_matter().affiliations;
    for (auto it = affs.begin(); it != affs.end(); ++it) {
        if (it->id == id) {
            affs.erase(it);
            // Detach from authors.
            for (auto& author : document_.front_matter().authors) {
                std::erase_if(author.affiliations,
                              [&](const AffiliationId& a) { return a == id; });
            }
            document_.BumpVersion();
            return {};
        }
    }
    return Unexpected(ToString(EditError::NotFound));
}

Result<NodeId, EditError> DocumentEditor::AddAuthor(Author author, NodeId) {
    document_.front_matter().authors.push_back(std::move(author));
    document_.BumpVersion();
    return NodeId();  // authors are not tree nodes; id unused
}

Result<void, EditError> DocumentEditor::RemoveAuthor(size_t index) {
    auto& authors = document_.front_matter().authors;
    if (index >= authors.size()) return Unexpected(ToString(EditError::InvalidTarget));
    authors.erase(authors.begin() + index);
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::UpdateAuthor(size_t index, const Author& author) {
    auto& authors = document_.front_matter().authors;
    if (index >= authors.size()) return Unexpected(ToString(EditError::InvalidTarget));
    authors[index] = author;
    document_.BumpVersion();
    return {};
}

// ---------------- Body structure ----------------

Result<NodeId, EditError> DocumentEditor::InsertSection(size_t index, InlineContent title,
                                                        NodeId id) {
    auto& sections = document_.body().sections;
    if (index > sections.size()) return Unexpected(ToString(EditError::InvalidTarget));
    if (id.empty()) id = IdGenerator::NewNode();
    Section section;
    section.id = id;
    section.title = std::move(title);
    sections.insert(sections.begin() + index, std::move(section));
    document_.BumpVersion();
    return id;
}

Result<void, EditError> DocumentEditor::DeleteSection(size_t index) {
    auto& sections = document_.body().sections;
    if (index >= sections.size()) return Unexpected(ToString(EditError::InvalidTarget));
    sections.erase(sections.begin() + index);
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::MoveSection(size_t from, size_t to) {
    auto& sections = document_.body().sections;
    if (from >= sections.size() || to > sections.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    if (from == to || from + 1 == to) {
        document_.BumpVersion();
        return {};
    }
    Section moved = std::move(sections[from]);
    sections.erase(sections.begin() + from);
    // After erase, 'to' may shift if to > from.
    size_t insert_at = to > from ? to - 1 : to;
    if (insert_at > sections.size()) insert_at = sections.size();
    sections.insert(sections.begin() + insert_at, std::move(moved));
    document_.BumpVersion();
    return {};
}

Result<NodeId, EditError> DocumentEditor::InsertSubsection(size_t section_index, size_t index,
                                                           InlineContent title, NodeId id) {
    auto& sections = document_.body().sections;
    if (section_index >= sections.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    auto& subs = sections[section_index].subsections;
    if (index > subs.size()) return Unexpected(ToString(EditError::InvalidTarget));
    if (id.empty()) id = IdGenerator::NewNode();
    Subsection sub;
    sub.id = id;
    sub.title = std::move(title);
    subs.insert(subs.begin() + index, std::move(sub));
    document_.BumpVersion();
    return id;
}

Result<NodeId, EditError> DocumentEditor::InsertSubsectionAfter(
    const NodeId& anchor, InlineContent title, NodeId id) {
    if (id.empty()) id = IdGenerator::NewNode();

    const auto place = [&](Section& section, size_t subsection_index,
                           std::vector<Block> taken) -> Result<NodeId, EditError> {
        Subsection sub;
        sub.id = id;
        sub.title = std::move(title);
        sub.blocks = std::move(taken);
        auto& subs = section.subsections;
        subs.insert(subs.begin() + static_cast<long>(subsection_index),
                    std::move(sub));
        document_.BumpVersion();
        return id;
    };

    // 1. A block directly inside a section or subsection: it takes the blocks
    //    below it, and the heading goes before every existing subsection (or
    //    at the subsection's start), which is the earliest position the model
    //    can render. A block inside a subsubsection cannot host a subsection,
    //    so it is rejected.
    if (auto pos = FindBlockPosition(anchor)) {
        Section& section = document_.body().sections[pos->section_index];
        if (pos->in_subsubsection) {
            return Unexpected(ToString(EditError::InvalidTarget));
        }
        if (pos->in_subsection) {
            std::vector<Block>& blocks =
                section.subsections[pos->subsection_index].blocks;
            std::vector<Block> taken;
            for (size_t i = pos->block_index + 1; i < blocks.size(); ++i) {
                taken.push_back(std::move(blocks[i]));
            }
            blocks.resize(pos->block_index + 1);
            return place(section, pos->subsection_index + 1, std::move(taken));
        }
        std::vector<Block>& blocks = section.blocks;
        std::vector<Block> taken;
        for (size_t i = pos->block_index + 1; i < blocks.size(); ++i) {
            taken.push_back(std::move(blocks[i]));
        }
        blocks.resize(pos->block_index + 1);
        return place(section, 0, std::move(taken));
    }

    // 2. An existing subsection: the new heading follows it, nothing moves.
    for (auto& section : document_.body().sections) {
        for (size_t ui = 0; ui < section.subsections.size(); ++ui) {
            if (section.subsections[ui].id == anchor) {
                return place(section, ui + 1, {});
            }
        }
    }

    // 3. A section: everything it owns becomes the subsection's content, so
    //    the heading lands directly under the section title.
    if (Section* section = FindSection(anchor)) {
        std::vector<Block> taken = std::move(section->blocks);
        section->blocks.clear();
        return place(*section, 0, std::move(taken));
    }
    return Unexpected(ToString(EditError::NotFound));
}

Result<void, EditError> DocumentEditor::DeleteSubsection(size_t section_index,
                                                         size_t subsection_index) {
    auto& sections = document_.body().sections;
    if (section_index >= sections.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    auto& subs = sections[section_index].subsections;
    if (subsection_index >= subs.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    subs.erase(subs.begin() + subsection_index);
    document_.BumpVersion();
    return {};
}

// ---------------- Subsubsections (third heading level) ----------------

Result<NodeId, EditError> DocumentEditor::InsertSubsubsection(
    size_t section_index, size_t subsection_index, size_t index,
    InlineContent title, NodeId id) {
    auto& sections = document_.body().sections;
    if (section_index >= sections.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    auto& subs = sections[section_index].subsections;
    if (subsection_index >= subs.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    auto& subsubs = subs[subsection_index].subsubsections;
    if (index > subsubs.size()) return Unexpected(ToString(EditError::InvalidTarget));
    if (id.empty()) id = IdGenerator::NewNode();
    Subsubsection subsub;
    subsub.id = id;
    subsub.title = std::move(title);
    subsubs.insert(subsubs.begin() + static_cast<long>(index), std::move(subsub));
    document_.BumpVersion();
    return id;
}

Result<NodeId, EditError> DocumentEditor::InsertSubsubsectionAfter(
    const NodeId& anchor, InlineContent title, NodeId id) {
    if (id.empty()) id = IdGenerator::NewNode();

    const auto place = [&](Subsection& sub, size_t subsubsection_index,
                           std::vector<Block> taken) -> Result<NodeId, EditError> {
        Subsubsection subsub;
        subsub.id = id;
        subsub.title = std::move(title);
        subsub.blocks = std::move(taken);
        auto& subsubs = sub.subsubsections;
        subsubs.insert(subsubs.begin() + static_cast<long>(subsubsection_index),
                       std::move(subsub));
        document_.BumpVersion();
        return id;
    };

    // 1. A block inside the subsection (or one of its subsubsections): the
    //    blocks below the anchor become the new subsubsection's content, and
    //    the heading appears where it was asked for.
    if (auto pos = FindBlockPosition(anchor)) {
        Section& section = document_.body().sections[pos->section_index];
        if (!pos->in_subsection) {
            return Unexpected(ToString(EditError::InvalidTarget));
        }
        Subsection& sub = section.subsections[pos->subsection_index];

        if (pos->in_subsubsection) {
            std::vector<Block>& blocks =
                sub.subsubsections[pos->subsubsection_index].blocks;
            std::vector<Block> taken;
            for (size_t i = pos->block_index + 1; i < blocks.size(); ++i) {
                taken.push_back(std::move(blocks[i]));
            }
            blocks.resize(pos->block_index + 1);
            return place(sub, pos->subsubsection_index + 1, std::move(taken));
        }
        std::vector<Block> taken;
        for (size_t i = pos->block_index + 1; i < sub.blocks.size(); ++i) {
            taken.push_back(std::move(sub.blocks[i]));
        }
        sub.blocks.resize(pos->block_index + 1);
        return place(sub, 0, std::move(taken));
    }

    // 2. An existing subsubsection: the new heading follows it, nothing moves.
    if (Subsection* owner = FindParentSubsubsection(anchor)) {
        for (size_t zi = 0; zi < owner->subsubsections.size(); ++zi) {
            if (owner->subsubsections[zi].id == anchor) {
                return place(*owner, zi + 1, {});
            }
        }
    }

    // 3. A subsection: everything it owns becomes the subsubsection's content.
    if (Subsection* sub = FindSubsection(anchor)) {
        std::vector<Block> taken = std::move(sub->blocks);
        sub->blocks.clear();
        return place(*sub, 0, std::move(taken));
    }
    return Unexpected(ToString(EditError::NotFound));
}

Result<void, EditError> DocumentEditor::DeleteSubsubsection(
    size_t section_index, size_t subsection_index, size_t subsubsection_index) {
    auto& sections = document_.body().sections;
    if (section_index >= sections.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    auto& subs = sections[section_index].subsections;
    if (subsection_index >= subs.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    auto& subsubs = subs[subsection_index].subsubsections;
    if (subsubsection_index >= subsubs.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    subsubs.erase(subsubs.begin() + static_cast<long>(subsubsection_index));
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::MoveSubsubsection(size_t section_index,
                                                           size_t subsection_index,
                                                           size_t from, size_t to) {
    auto& sections = document_.body().sections;
    if (section_index >= sections.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    auto& subs = sections[section_index].subsections;
    if (subsection_index >= subs.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    auto& subsubs = subs[subsection_index].subsubsections;
    if (from >= subsubs.size() || to > subsubs.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    if (from == to || from + 1 == to) {
        document_.BumpVersion();
        return {};
    }
    Subsubsection moved = std::move(subsubs[from]);
    subsubs.erase(subsubs.begin() + static_cast<long>(from));
    size_t insert_at = to > from ? to - 1 : to;
    if (insert_at > subsubs.size()) insert_at = subsubs.size();
    subsubs.insert(subsubs.begin() + static_cast<long>(insert_at), std::move(moved));
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::MoveSubsection(size_t section_index, size_t from,
                                                       size_t to) {
    auto& sections = document_.body().sections;
    if (section_index >= sections.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    auto& subs = sections[section_index].subsections;
    if (from >= subs.size() || to > subs.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    if (from == to || from + 1 == to) {
        document_.BumpVersion();
        return {};
    }
    Subsection moved = std::move(subs[from]);
    subs.erase(subs.begin() + from);
    size_t insert_at = to > from ? to - 1 : to;
    if (insert_at > subs.size()) insert_at = subs.size();
    subs.insert(subs.begin() + insert_at, std::move(moved));
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::RenameSection(const NodeId& id,
                                                      const InlineContent& title) {
    if (auto* section = FindSection(id)) {
        section->title = title;
        document_.BumpVersion();
        return {};
    }
    if (auto* sub = FindSubsection(id)) {
        sub->title = title;
        document_.BumpVersion();
        return {};
    }
    if (auto* subsub = FindSubsubsection(id)) {
        subsub->title = title;
        document_.BumpVersion();
        return {};
    }
    return Unexpected(ToString(EditError::NotFound));
}

Result<void, EditError> DocumentEditor::RenameSubsection(
    const NodeId& id, const InlineContent& title) {
    if (auto* sub = FindSubsection(id)) {
        sub->title = title;
        document_.BumpVersion();
        return {};
    }
    return Unexpected(ToString(EditError::NotFound));
}

Result<void, EditError> DocumentEditor::RenameSubsubsection(
    const NodeId& id, const InlineContent& title) {
    if (auto* subsub = FindSubsubsection(id)) {
        subsub->title = title;
        document_.BumpVersion();
        return {};
    }
    return Unexpected(ToString(EditError::NotFound));
}

// ---------------- Blocks ----------------

Section* DocumentEditor::FindSection(const NodeId& id) {
    return pf::FindSection(document_, id);
}

Subsection* DocumentEditor::FindSubsection(const NodeId& id) {
    return pf::FindSubsection(document_, id);
}

Subsubsection* DocumentEditor::FindSubsubsection(const NodeId& id) {
    return pf::FindSubsubsection(document_, id);
}

Subsection* DocumentEditor::FindParentSubsubsection(const NodeId& id) {
    auto address = LocateNode(document_, id);
    if (!address || address->kind != NodeKind::Subsubsection) return nullptr;
    auto& sections = document_.body().sections;
    if (!address->section || !address->subsection) return nullptr;
    if (*address->section >= sections.size()) return nullptr;
    auto& subs = sections[*address->section].subsections;
    if (*address->subsection >= subs.size()) return nullptr;
    return &subs[*address->subsection];
}

Block* DocumentEditor::FindBlock(const NodeId& id) {
    return pf::FindBlock(document_, id);
}

std::optional<DocumentEditor::BlockPosition> DocumentEditor::FindBlockPosition(
    const NodeId& id) {
    auto address = LocateNode(document_, id);
    if (!address || !address->block) return std::nullopt;
    BlockPosition position;
    position.section_index = *address->section;
    if (address->subsection) {
        position.in_subsection = true;
        position.subsection_index = *address->subsection;
    }
    if (address->subsubsection) {
        position.in_subsubsection = true;
        position.subsubsection_index = *address->subsubsection;
    }
    position.block_index = *address->block;
    return position;
}

std::vector<NodeId> DocumentEditor::BlockParents(const NodeId& id) {
    std::vector<NodeId> parents;
    auto address = LocateNode(document_, id);
    if (!address || !address->block) return parents;
    auto& sections = document_.body().sections;
    if (!address->section || *address->section >= sections.size()) return parents;
    const Section& section = sections[*address->section];
    parents.push_back(section.id);
    if (!address->subsection) return parents;
    const Subsection& sub = section.subsections[*address->subsection];
    parents.push_back(sub.id);
    if (!address->subsubsection) return parents;
    parents.push_back(sub.subsubsections[*address->subsubsection].id);
    return parents;
}

Result<NodeId, EditError> DocumentEditor::InsertBlock(const NodeId& parent,
                                                      std::optional<size_t> index,
                                                      Block block, NodeId id) {
    // A block may live in a section, a subsection or a subsubsection; the
    // traversal layer owns that distinction.
    std::vector<Block>* blocks = FindBlockListForParent(parent);
    if (!blocks) return Unexpected(ToString(EditError::InvalidTarget));

    // Extract id from the incoming block or assign a fresh one.
    if (id.empty()) {
        id = std::visit([](const auto& b) { return b.id; }, block);
    }
    if (id.empty()) {
        id = IdGenerator::NewNode();
        std::visit([&](auto& b) { b.id = id; }, block);
    }

    size_t at = index.value_or(blocks->size());
    if (at > blocks->size()) return Unexpected(ToString(EditError::InvalidTarget));
    blocks->insert(blocks->begin() + static_cast<long>(at), std::move(block));
    document_.BumpVersion();
    return id;
}

Result<void, EditError> DocumentEditor::DeleteBlock(const NodeId& id) {
    auto* blocks = FindBlockListForNode(id);
    if (!blocks) return Unexpected(ToString(EditError::NotFound));
    auto address = LocateNode(document_, id);
    blocks->erase(blocks->begin() + static_cast<long>(*address->block));
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::MoveBlock(const NodeId& id, const NodeId& new_parent,
                                                  std::optional<size_t> new_index) {
    auto address = LocateNode(document_, id);
    if (!address || !address->block) return Unexpected(ToString(EditError::NotFound));
    std::vector<Block>* src_blocks = FindBlockList(document_, *address);
    if (!src_blocks) return Unexpected(ToString(EditError::NotFound));

    std::vector<Block>* dst_blocks = FindBlockListForParent(new_parent);
    if (!dst_blocks) return Unexpected(ToString(EditError::InvalidTarget));

    Block moved = std::move((*src_blocks)[*address->block]);
    src_blocks->erase(src_blocks->begin() + static_cast<long>(*address->block));

    size_t at = new_index.value_or(dst_blocks->size());
    if (at > dst_blocks->size()) {
        // Put it back where it was to avoid losing data.
        src_blocks->insert(src_blocks->begin() + static_cast<long>(*address->block),
                           std::move(moved));
        return Unexpected(ToString(EditError::InvalidTarget));
    }
    dst_blocks->insert(dst_blocks->begin() + static_cast<long>(at), std::move(moved));
    document_.BumpVersion();
    return {};
}

// ---------------- Content edits ----------------

Result<void, EditError> DocumentEditor::SetParagraphContent(const NodeId& id,
                                                            const InlineContent& content) {
    Block* block = FindBlock(id);
    if (!block) return Unexpected(ToString(EditError::NotFound));
    auto* para = std::get_if<Paragraph>(block);
    if (!para) return Unexpected(ToString(EditError::InvalidTarget));
    para->content = content;
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::SetCaption(const NodeId& id,
                                                   const InlineContent& caption) {
    Block* block = FindBlock(id);
    if (!block) return Unexpected(ToString(EditError::NotFound));
    if (auto* fig = std::get_if<Figure>(block)) {
        fig->caption = caption;
    } else if (auto* table = std::get_if<Table>(block)) {
        table->caption = caption;
    } else {
        return Unexpected(ToString(EditError::InvalidTarget));
    }
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::SetEquationSource(const NodeId& id,
                                                          const std::string& source,
                                                          std::optional<bool> numbered) {
    Block* block = FindBlock(id);
    if (!block) return Unexpected(ToString(EditError::NotFound));
    if (auto* eq = std::get_if<DisplayEquation>(block)) {
        eq->math_source = source;
        if (numbered) eq->numbered = *numbered;
    } else {
        return Unexpected(ToString(EditError::InvalidTarget));
    }
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::SetFigureAsset(const NodeId& id,
                                                       const AssetId& asset_id,
                                                       std::optional<FigureWidth> width) {
    Block* block = FindBlock(id);
    if (!block) return Unexpected(ToString(EditError::NotFound));
    if (auto* fig = std::get_if<Figure>(block)) {
        fig->asset_id = asset_id;
        if (width) fig->width = *width;
    } else {
        return Unexpected(ToString(EditError::InvalidTarget));
    }
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::SetFigureAltText(const NodeId& id,
                                                         const std::string& alt) {
    Block* block = FindBlock(id);
    if (!block) return Unexpected(ToString(EditError::NotFound));
    if (auto* fig = std::get_if<Figure>(block)) {
        fig->alt_text = alt;
    } else {
        return Unexpected(ToString(EditError::InvalidTarget));
    }
    document_.BumpVersion();
    return {};
}

// ---------------- Tables ----------------

std::vector<std::vector<TableCell>> DocumentEditor::MakeCells(size_t rows, size_t cols) {
    std::vector<std::vector<TableCell>> cells(
        rows, std::vector<TableCell>(cols, TableCell{}));
    return cells;
}

std::vector<Block>* DocumentEditor::FindBlockListForParent(const NodeId& parent) {
    if (auto* section = FindSection(parent)) return &section->blocks;
    if (auto* sub = FindSubsection(parent)) return &sub->blocks;
    if (auto* subsub = FindSubsubsection(parent)) return &subsub->blocks;
    return nullptr;
}

std::vector<Block>* DocumentEditor::FindBlockListForNode(const NodeId& id) {
    auto address = LocateNode(document_, id);
    if (!address || !address->block) return nullptr;
    return FindBlockList(document_, *address);
}

Result<Table, EditError> DocumentEditor::MakeTable(std::vector<TableColumn> columns,
                                                   size_t rows, bool has_header_row,
                                                   NodeId id) {
    if (columns.empty()) return Unexpected(ToString(EditError::ConstraintViolation));
    Table table;
    if (id.empty()) id = IdGenerator::NewNode();
    table.id = id;
    table.has_header_row = has_header_row;
    table.columns = std::move(columns);
    table.cells = MakeCells(rows, table.columns.size());
    return table;
}

Result<void, EditError> DocumentEditor::InsertTableRow(const NodeId& id, size_t index) {
    Table* table = [&]() -> Table* {
        Block* block = FindBlock(id);
        return block ? std::get_if<Table>(block) : nullptr;
    }();
    if (!table) return Unexpected(ToString(EditError::NotFound));
    if (index > table->cells.size()) return Unexpected(ToString(EditError::InvalidTarget));
    std::vector<TableCell> row(table->columns.size(), TableCell{});
    table->cells.insert(table->cells.begin() + index, std::move(row));
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::DeleteTableRow(const NodeId& id, size_t index) {
    Table* table = [&]() -> Table* {
        Block* block = FindBlock(id);
        return block ? std::get_if<Table>(block) : nullptr;
    }();
    if (!table) return Unexpected(ToString(EditError::NotFound));
    if (index >= table->cells.size()) return Unexpected(ToString(EditError::InvalidTarget));
    table->cells.erase(table->cells.begin() + index);
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::InsertTableColumn(const NodeId& id, size_t index,
                                                          ColumnAlignment alignment) {
    Table* table = [&]() -> Table* {
        Block* block = FindBlock(id);
        return block ? std::get_if<Table>(block) : nullptr;
    }();
    if (!table) return Unexpected(ToString(EditError::NotFound));
    if (index > table->columns.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    table->columns.insert(table->columns.begin() + index, TableColumn{alignment});
    for (auto& row : table->cells) {
        row.insert(row.begin() + index, TableCell{});
    }
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::DeleteTableColumn(const NodeId& id, size_t index) {
    Table* table = [&]() -> Table* {
        Block* block = FindBlock(id);
        return block ? std::get_if<Table>(block) : nullptr;
    }();
    if (!table) return Unexpected(ToString(EditError::NotFound));
    if (index >= table->columns.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    table->columns.erase(table->columns.begin() + index);
    for (auto& row : table->cells) {
        if (index < row.size()) row.erase(row.begin() + index);
    }
    document_.BumpVersion();
    return {};
}

Result<void, EditError> DocumentEditor::SetTableCell(const NodeId& id, size_t row,
                                                     size_t column,
                                                     const InlineContent& content) {
    Table* table = [&]() -> Table* {
        Block* block = FindBlock(id);
        return block ? std::get_if<Table>(block) : nullptr;
    }();
    if (!table) return Unexpected(ToString(EditError::NotFound));
    if (row >= table->cells.size() || column >= table->columns.size())
        return Unexpected(ToString(EditError::InvalidTarget));
    table->cells[row][column].content = content;
    document_.BumpVersion();
    return {};
}

}  // namespace pf

namespace pf {
template <>
EditError pf::ToStringError<EditError>(const std::string& value) {
    if (value == "InvalidTarget") return EditError::InvalidTarget;
    if (value == "ConstraintViolation") return EditError::ConstraintViolation;
    if (value == "NotFound") return EditError::NotFound;
    return EditError::InvalidTarget;
}
}  // namespace pf
