#include "editing/EditingSystem.h"

#include <algorithm>
#include <functional>

#include "core/IdGenerator.h"
#include "document/InlineText.h"

namespace pf {

EditResult EditingSystem::Apply(const EditCommand& command) {
    if (command.project_id != host_.project_id()) {
        return EditResult::Fail(FailureReason::WrongProject, "project id mismatch");
    }
    if (command.base_revision != host_.revision()) {
        return EditResult::Fail(FailureReason::StaleOperation,
                                "base revision " + std::to_string(command.base_revision.value) + " != current " +
                                    std::to_string(host_.revision().value));
    }
    return ApplyFullPayload(command);
}

EditResult EditingSystem::ApplyFullPayload(const EditCommand& cmd) {
    if (const auto* tpl = std::get_if<ChangeTemplatePayload>(&cmd.payload)) {
        const auto old_id = host_.template_selection
                                ? host_.template_selection()
                                : std::string{};
        const auto old_binding = host_.template_binding
                                     ? host_.template_binding()
                                     : std::optional<TemplateBinding>{};
        if (host_.change_template && !host_.change_template(*tpl))
            return EditResult::Fail(FailureReason::InvalidTarget,
                                    "Template binding is unavailable");
        // 模板变更是项目级变更（架构补充 8）：
        // DocumentVersion 不变，ProjectRevision +1。
        ProjectRevision old_rev = host_.revision();
        ProjectRevision new_rev = host_.bump_revision();
        if (host_.template_binding && cmd.origin != EditOrigin::Undo &&
            cmd.origin != EditOrigin::Redo) {
            HistoryEntry entry;
            entry.action =
                TemplateHistoryAction{old_id, tpl->template_id, old_binding,
                                      host_.template_binding()};
            entry.operation_id = cmd.operation_id;
            entry.resulting_revision = new_rev;
            history_.Push(std::move(entry));
        }
        Notify(cmd, old_rev, new_rev, {}, {ChangeKind::TemplateChanged});
        return EditResult::Ok(new_rev);
    }

    // Document 类 payload：抓取完整的 before/after snapshot，
    // 使每个操作（包括删除）都可逆。
    Document& doc = host_.document();
    auto before = std::make_shared<const Document>(doc);

    std::optional<EditPayload> doc_payload = std::visit(
        [&](const auto& p) -> std::optional<EditPayload> {
            using T = std::decay_t<decltype(p)>;
            if constexpr (std::is_same_v<T, ChangeTemplatePayload>) {
                return std::nullopt;
            } else {
                return p;
            }
        },
        cmd.payload);
    if (!doc_payload) {
        return EditResult::Fail(FailureReason::UnknownPayload, "unreachable payload branch");
    }

    EditResult result = ApplyDocumentPayload(cmd, *doc_payload);
    if (result.status == EditStatus::Applied) {
        auto after = std::make_shared<const Document>(doc);
        HistoryEntry entry;
        entry.action = SnapshotHistoryAction{std::move(before), std::move(after)};
        entry.operation_id = cmd.operation_id;
        entry.resulting_revision = result.new_revision;
        history_.Push(std::move(entry));
    }
    return result;
}

EditResult EditingSystem::ApplyDocumentPayload(const EditCommand& cmd, const EditPayload& payload) {
    DocumentEditor editor(host_.document());

    // 所有结果路径在成功时都经由 host 递增项目 revision。
    auto finish = [&](Result<NodeId, EditError> r) -> EditResult {
        if (!r) {
            return EditResult::Fail(FailureReason::InvalidTarget, std::string(ToString(r.error())));
        }
        ProjectRevision rev = host_.bump_revision();
        Notify(cmd, cmd.base_revision, rev, {r.value()}, {ChangeKind::StructureChanged});
        return EditResult::Ok(rev, r.value());
    };
    auto finishVoid = [&](Result<void, EditError> r, std::vector<NodeId> affected = {},
                          ChangeKind kind = ChangeKind::TextChanged) -> EditResult {
        if (!r) {
            return EditResult::Fail(FailureReason::InvalidTarget, std::string(ToString(r.error())));
        }
        ProjectRevision rev = host_.bump_revision();
        Notify(cmd, cmd.base_revision, rev, std::move(affected), {kind});
        return EditResult::Ok(rev);
    };
    auto finishMeta = [&](Result<void, EditError> r) -> EditResult {
        if (!r) {
            return EditResult::Fail(FailureReason::InvalidTarget, std::string(ToString(r.error())));
        }
        ProjectRevision rev = host_.bump_revision();
        Notify(cmd, cmd.base_revision, rev, {}, {ChangeKind::MetadataChanged});
        return EditResult::Ok(rev);
    };

    if (const auto* p = std::get_if<SetTitlePayload>(&payload)) {
        return finishMeta(editor.SetTitle(p->title));
    }
    if (const auto* p = std::get_if<SetAbstractPayload>(&payload)) {
        return finishMeta(editor.SetAbstract(p->abstract_text));
    }
    if (const auto* p = std::get_if<SetKeywordsPayload>(&payload)) {
        return finishMeta(editor.SetKeywords(p->keywords));
    }
    if (const auto* p = std::get_if<AddAuthorPayload>(&payload)) {
        auto r = editor.AddAuthor(p->author);
        if (!r)
            return EditResult::Fail(FailureReason::InvalidTarget, ToString(r.error()));
        ProjectRevision rev = host_.bump_revision();
        Notify(cmd, cmd.base_revision, rev, {}, {ChangeKind::MetadataChanged});
        return EditResult::Ok(rev);
    }
    if (const auto* p = std::get_if<RemoveAuthorPayload>(&payload)) {
        return finishMeta(editor.RemoveAuthor(p->index));
    }
    if (const auto* p = std::get_if<UpdateAuthorPayload>(&payload)) {
        return finishMeta(editor.UpdateAuthor(p->index, p->author));
    }
    if (const auto* p = std::get_if<SetAffiliationsPayload>(&payload)) {
        // 整体替换 affiliation 列表。
        auto& front = editor.doc().front_matter();
        front.affiliations = p->affiliations;
        // 作者只可引用仍然存在的机构；否则缩短列表会留下悬空的 affiliation id。
        for (auto& author : front.authors) {
            std::vector<AffiliationId> kept;
            for (const auto& id : author.affiliations) {
                for (const auto& candidate : front.affiliations) {
                    if (candidate.id == id) {
                        kept.push_back(id);
                        break;
                    }
                }
            }
            author.affiliations = std::move(kept);
        }
        editor.doc().BumpVersion();
        ProjectRevision rev = host_.bump_revision();
        Notify(cmd, cmd.base_revision, rev, {}, {ChangeKind::MetadataChanged});
        return EditResult::Ok(rev);
    }
    if (const auto* p = std::get_if<InsertSectionPayload>(&payload)) {
        return finish(editor.InsertSection(p->index, p->title));
    }
    if (const auto* p = std::get_if<DeleteSectionPayload>(&payload)) {
        return finishVoid(editor.DeleteSection(p->index), {}, ChangeKind::StructureChanged);
    }
    if (const auto* p = std::get_if<MoveSectionPayload>(&payload)) {
        return finishVoid(editor.MoveSection(p->from, p->to), {}, ChangeKind::StructureChanged);
    }
    if (const auto* p = std::get_if<RenameSectionPayload>(&payload)) {
        return finishVoid(editor.RenameSection(p->section, p->title), {p->section}, ChangeKind::MetadataChanged);
    }
    if (const auto* p = std::get_if<RenameSubsectionPayload>(&payload)) {
        return finishVoid(editor.RenameSubsection(p->subsection, p->title), {p->subsection},
                          ChangeKind::MetadataChanged);
    }
    if (const auto* p = std::get_if<MoveSubsectionPayload>(&payload)) {
        return finishVoid(editor.MoveSubsection(p->section_index, p->from, p->to), {}, ChangeKind::StructureChanged);
    }
    if (const auto* p = std::get_if<InsertSubsectionPayload>(&payload)) {
        return finish(editor.InsertSubsection(p->section_index, p->index, p->title));
    }
    if (const auto* p = std::get_if<InsertSubsectionAfterPayload>(&payload)) {
        return finish(editor.InsertSubsectionAfter(p->after, p->title));
    }
    if (const auto* p = std::get_if<DeleteSubsectionPayload>(&payload)) {
        return finishVoid(editor.DeleteSubsection(p->section_index, p->subsection_index), {},
                          ChangeKind::StructureChanged);
    }
    if (const auto* p = std::get_if<InsertSubsubsectionPayload>(&payload)) {
        return finish(editor.InsertSubsubsection(p->section_index, p->subsection_index, p->index, p->title));
    }
    if (const auto* p = std::get_if<InsertSubsubsectionAfterPayload>(&payload)) {
        return finish(editor.InsertSubsubsectionAfter(p->after, p->title));
    }
    if (const auto* p = std::get_if<RenameSubsubsectionPayload>(&payload)) {
        return finishVoid(editor.RenameSubsubsection(p->subsubsection, p->title), {p->subsubsection},
                          ChangeKind::MetadataChanged);
    }
    if (const auto* p = std::get_if<MoveSubsubsectionPayload>(&payload)) {
        return finishVoid(editor.MoveSubsubsection(p->section_index, p->subsection_index, p->from, p->to), {},
                          ChangeKind::StructureChanged);
    }
    if (const auto* p = std::get_if<DeleteSubsubsectionPayload>(&payload)) {
        return finishVoid(editor.DeleteSubsubsection(p->section_index, p->subsection_index, p->subsubsection_index), {},
                          ChangeKind::StructureChanged);
    }
    if (const auto* p = std::get_if<InsertParagraphPayload>(&payload)) {
        Paragraph para;
        para.content = p->content;
        return finish(editor.InsertBlock(p->parent, p->index, para));
    }
    if (const auto* p = std::get_if<InsertFigurePayload>(&payload)) {
        Figure fig;
        fig.asset_id = p->asset_id;
        fig.caption = p->caption;
        fig.width = p->width;
        fig.span = p->span;
        return finish(editor.InsertBlock(p->parent, p->index, fig));
    }
    if (const auto* p = std::get_if<InsertTablePayload>(&payload)) {
        auto table = DocumentEditor::MakeTable(p->columns, p->rows, p->has_header_row);
        if (!table) {
            return EditResult::Fail(FailureReason::ConstraintViolation, ToString(table.error()));
        }
        table.value().caption = p->caption;
        return finish(editor.InsertBlock(p->parent, p->index, table.value()));
    }
    if (const auto* p = std::get_if<InsertEquationPayload>(&payload)) {
        EquationBlock eq;
        eq.expression.latex = p->latex;
        eq.numbered = p->numbered;
        eq.label = p->label;
        return finish(editor.InsertBlock(p->parent, p->index, eq));
    }
    if (const auto* p = std::get_if<DeleteBlockPayload>(&payload)) {
        return finishVoid(editor.DeleteBlock(p->node), {p->node}, ChangeKind::StructureChanged);
    }
    if (const auto* p = std::get_if<MoveBlockPayload>(&payload)) {
        return finishVoid(editor.MoveBlock(p->node, p->new_parent, p->new_index), {p->node},
                          ChangeKind::StructureChanged);
    }
    if (const auto* p = std::get_if<EditParagraphPayload>(&payload)) {
        return finishVoid(editor.SetParagraphContent(p->paragraph, p->content), {p->paragraph},
                          ChangeKind::TextChanged);
    }
    if (const auto* p = std::get_if<EditCaptionPayload>(&payload)) {
        return finishVoid(editor.SetCaption(p->block, p->caption), {p->block}, ChangeKind::TextChanged);
    }
    if (const auto* p = std::get_if<EditEquationPayload>(&payload)) {
        return finishVoid(editor.SetEquationSource(p->equation, p->latex, p->numbered, p->label), {p->equation},
                          ChangeKind::TextChanged);
    }
    if (const auto* p = std::get_if<EditFigureSpanPayload>(&payload)) {
        // 仅布局变更：文本未改动，因此标记为 MetadataChanged，
        // 且永不触发相邻内容的 reflow。
        return finishVoid(editor.SetFigureSpan(p->figure, p->span), {p->figure}, ChangeKind::MetadataChanged);
    }
    if (const auto* p = std::get_if<InsertCitationPayload>(&payload)) {
        // 将 citation 以内联方式插入段落内容。
        Block* block = editor.FindBlock(p->paragraph);
        if (!block)
            return EditResult::Fail(FailureReason::InvalidTarget, "paragraph missing");
        auto* para = std::get_if<Paragraph>(block);
        if (!para)
            return EditResult::Fail(FailureReason::InvalidTarget, "not a paragraph");
        Citation citation;
        citation.keys = p->keys;
        citation.mode = p->mode;
        InlineContent content = para->content;
        Citation new_cit = citation;
        if (p->at_index && *p->at_index <= content.size()) {
            content.insert(content.begin() + *p->at_index, InlineNode(std::move(new_cit)));
        } else {
            content.push_back(InlineNode(std::move(new_cit)));
        }
        return finishVoid(editor.SetParagraphContent(p->paragraph, content), {p->paragraph},
                          ChangeKind::ReferenceChanged);
    }
    if (const auto* p = std::get_if<InsertCrossReferencePayload>(&payload)) {
        Block* block = editor.FindBlock(p->paragraph);
        if (!block)
            return EditResult::Fail(FailureReason::InvalidTarget, "paragraph missing");
        auto* para = std::get_if<Paragraph>(block);
        if (!para)
            return EditResult::Fail(FailureReason::InvalidTarget, "not a paragraph");
        CrossReference ref;
        ref.target = p->target;
        InlineContent content = para->content;
        if (p->at_index && *p->at_index <= content.size()) {
            content.insert(content.begin() + *p->at_index, InlineNode(std::move(ref)));
        } else {
            content.push_back(InlineNode(std::move(ref)));
        }
        return finishVoid(editor.SetParagraphContent(p->paragraph, content), {p->paragraph},
                          ChangeKind::ReferenceChanged);
    }
    return EditResult::Fail(FailureReason::UnknownPayload, "unhandled payload");
}

EditResult EditingSystem::Undo() {
    auto entry = history_.PopUndo();
    if (!entry)
        return EditResult::Fail(FailureReason::InvalidTarget, "nothing to undo");
    return ApplyHistoryEntry(*entry, EditOrigin::Undo);
}

EditResult EditingSystem::Redo() {
    auto entry = history_.PopRedo();
    if (!entry)
        return EditResult::Fail(FailureReason::InvalidTarget, "nothing to redo");
    return ApplyHistoryEntry(*entry, EditOrigin::Redo);
}

EditResult EditingSystem::ApplyHistoryEntry(const HistoryEntry& entry, EditOrigin origin) {
    const bool undo = origin == EditOrigin::Undo;
    if (const auto* snap = std::get_if<SnapshotHistoryAction>(&entry.action)) {
        // 恢复整篇文档 snapshot：host 层级的变更会递增 revision 并发出通知。
        Document& doc = host_.document();
        // 先复制再移动，避免原地赋值读到被覆盖的 buffer。
        Document restored = undo ? *snap->before : *snap->after;
        doc = std::move(restored);
        EditCommand cmd;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.project_id = host_.project_id();
        cmd.base_revision = host_.revision();
        cmd.origin = origin;
        const ProjectRevision old_rev = host_.revision();
        const ProjectRevision new_rev = host_.bump_revision();
        Notify(cmd, old_rev, new_rev, {}, {ChangeKind::StructureChanged, ChangeKind::TextChanged});
        return EditResult::Ok(new_rev);
    }
    if (const auto* tpl = std::get_if<TemplateHistoryAction>(&entry.action)) {
        ChangeTemplatePayload p;
        p.template_id = undo ? tpl->old_template : tpl->new_template;
        p.binding = undo ? tpl->old_binding : tpl->new_binding;
        EditCommand cmd;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.project_id = host_.project_id();
        cmd.base_revision = host_.revision();
        cmd.origin = origin;
        cmd.payload = std::move(p);
        return ApplyFullPayload(cmd);
    }
    return EditResult::Fail(FailureReason::UnknownPayload, "unknown history entry");
}

void EditingSystem::Notify(const EditCommand& cmd, ProjectRevision old_rev, ProjectRevision new_rev,
                           std::vector<NodeId> affected, std::vector<ChangeKind> kinds) {
    if (!host_.on_document_changed)
        return;
    DocumentChangedEvent event;
    event.project_id = host_.project_id();
    event.old_revision = old_rev;
    event.new_revision = new_rev;
    event.origin = cmd.origin;
    event.affected_nodes = std::move(affected);
    event.change_kinds = std::move(kinds);
    host_.on_document_changed(event);
}

} // namespace pf
