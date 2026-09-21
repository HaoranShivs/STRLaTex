// 引用功能方案的回归矩阵（domain + numbering + validation +
// persistence 层；GUI 级覆盖见 paperforge-inline-editor-test）：
//
//   * CitationNumberResolver：首次引用顺序、重复引用时编号稳定、
//     删除首次引用后重新编号、未知 key
//     -> [?]、多引用排序/压缩。
//   * Validator：E-CITATION-UNKNOWN-KEY、E-CITATION-NO-BIBLIOGRAPHY。
//   * 模板/渲染：GUI 编号策略与 LaTeX 样式一致
//     （generic-article -> unsrtnat，ieee -> IEEEtran），LaTeX 保持语义化
//     （\citep{key}，绝不硬编码 [1]）。
//   * 参考文献：导入时暴露重复 key；references.bib 是
//     项目资源，随引用 key 一起在保存/重载后保留。
#include "TestMain.hpp"
#include "ScopedTempDir.hpp"

#include <chrono>
#include <filesystem>

#include "bibliography/BibliographyService.h"
#include "core/IdGenerator.h"
#include "document/DocumentEditor.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"
#include "editing/EditCommand.h"
#include "numbering/CitationNumberResolver.h"
#include "project/ProjectSession.h"
#include "render/LatexRenderer.h"
#include "template/TemplateRegistry.h"
#include "validation/Validator.h"

using namespace pf;

namespace {

ProjectSession::Config TestConfig() {
    ProjectSession::Config config;
    config.tectonic_path = PF_TECTONIC_BIN;
    // E-08：每个进程唯一（共享路径的 bug 见 TestEndToEnd.cpp）。
    static pf::test::ScopedTempDir workspace("pf-cite-workspaces");
    config.workspace_root = workspace.path();
    config.debounce = std::chrono::milliseconds{0};
    return config;
}

// 构造一个文档，其各段落按顺序引用 `chains`
// （每条 chain 一个段落，每条 chain 是一个多 key 引用）。
Document MakeCitingDoc(const std::vector<std::vector<std::string>>& chains,
                       std::vector<NodeId>* paragraphs = nullptr) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    for (const auto& chain : chains) {
        Paragraph p;
        p.content.push_back(TextRun{"Claim from ", 0});
        Citation cit;
        cit.keys = chain;
        p.content.push_back(std::move(cit));
        editor.InsertBlock(s.value(), std::nullopt, p);
        if (paragraphs) paragraphs->push_back(p.id);
    }
    return doc;
}

BibliographyDatabase MakeDb(const std::vector<std::string>& keys) {
    std::string bib;
    for (const auto& key : keys) {
        bib += "@article{" + key + ", author={A. Author}, title={T}, year={2020}}\n";
    }
    BibliographyDatabase db;
    BibliographyService service(db);
    service.ImportText(bib);
    return db;
}

int CountDiagnostics(const ValidationResult& r, const char* code) {
    int n = 0;
    for (const auto& d : r.diagnostics) {
        if (d.code == code) ++n;
    }
    return n;
}

}  // namespace

// ---------------------------------------------------------------------------
// 编号策略
// ---------------------------------------------------------------------------

PF_TEST(CitationNumberingFollowsFirstCitationOrder) {
    auto db = MakeDb({"a", "b", "c"});
    // 引用顺序：A，然后 B，再 A，最后 C。
    auto doc = MakeCitingDoc({{"a"}, {"b"}, {"a"}, {"c"}});
    auto resolver = CitationNumberResolver::Build(doc, db);

    PF_CHECK(resolver.Find("a")->number == 1);
    PF_CHECK(resolver.Find("b")->number == 2);
    PF_CHECK(resolver.Find("c")->number == 3);
    // 再次引用 A 仍保持其编号：两处均为 "[1]"。
    PF_CHECK(resolver.Find("a")->display == "[1]");
    PF_CHECK(resolver.Find("a")->resolved);
}

PF_TEST(CitationNumberingRenumbersAfterCitationRemoved) {
    auto db = MakeDb({"a", "b"});
    {
        auto doc = MakeCitingDoc({{"a"}, {"b"}});
        auto resolver = CitationNumberResolver::Build(doc, db);
        PF_CHECK(resolver.Find("a")->number == 1);
        PF_CHECK(resolver.Find("b")->number == 2);
    }
    // 删除 A 的*全部*引用：B 变为 [1]（按引用顺序重新流动）。
    auto doc = MakeCitingDoc({{"b"}});
    auto resolver = CitationNumberResolver::Build(doc, db);
    PF_CHECK(resolver.Find("b")->number == 1);
    PF_CHECK(!resolver.Find("a"));
}

PF_TEST(CitationPillSortsDedupesAndCompresses) {
    auto db = MakeDb({"a", "b", "c", "d"});
    auto doc = MakeCitingDoc({{"a"}, {"b"}, {"c"}, {"d"}, {"d", "a", "b"}});
    auto resolver = CitationNumberResolver::Build(doc, db);

    // 多引用无论 key 顺序如何都按编号排序，且
    // [1,2,3] 连续段像 natbib 的 sort&compress 一样压缩。
    PF_CHECK(resolver.FormatPill({"b", "a"}) == "[1, 2]");
    PF_CHECK(resolver.FormatPill({"c", "a", "b"}) == "[1-3]");
    PF_CHECK(resolver.FormatPill({"d", "a", "b"}) == "[1, 2, 4]");
    // 单个引用内的重复 key 合并为一个编号。
    PF_CHECK(resolver.FormatPill({"a", "a"}) == "[1]");
}

PF_TEST(CitationUnknownKeyShowsQuestionMarkWithoutConsumingNumber) {
    auto db = MakeDb({"a", "b"});
    auto doc = MakeCitingDoc({{"ghost"}, {"a"}, {"a", "ghost"}});
    auto resolver = CitationNumberResolver::Build(doc, db);

    // 未知 key：[?]，且不得影响真实编号。
    PF_CHECK(resolver.Find("ghost")->display == "[?]");
    PF_CHECK(!resolver.Find("ghost")->resolved);
    PF_CHECK(resolver.Find("a")->number == 1);
    PF_CHECK(resolver.Find("b") == std::nullopt);  // 从未被引用，但已知
    PF_CHECK(resolver.FormatPill({"a", "ghost"}) == "[1, ?]");
}

// ---------------------------------------------------------------------------
// 校验（每次 build 之前；引用方案 §9）
// ---------------------------------------------------------------------------

PF_TEST(ValidatorReportsUnknownCitationKey) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    Paragraph p;
    Citation cit;
    cit.keys = {"ghost2024"};
    p.content.push_back(std::move(cit));
    editor.InsertBlock(s.value(), std::nullopt, p);

    Validator validator;
    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    input.has_bibliography = true;
    input.bibliography_keys = {"real2024"};
    auto result = validator.Validate(input);
    PF_CHECK(CountDiagnostics(result, "E-CITATION-UNKNOWN-KEY") == 1);
}

PF_TEST(ValidatorReportsCitationsWithoutBibliography) {
    Document doc;
    DocumentEditor editor(doc);
    auto s = editor.InsertSection(0, InlineFromText("S"));
    Paragraph p;
    Citation cit;
    cit.keys = {"anything"};
    p.content.push_back(std::move(cit));
    Citation cit2;
    cit2.keys = {"other"};
    p.content.push_back(std::move(cit2));
    editor.InsertBlock(s.value(), std::nullopt, p);

    Validator validator;
    ValidationInput input;
    input.document = &doc;
    input.template_id = "generic-article";
    input.has_bibliography = false;  // 空参考文献
    auto result = validator.Validate(input);
    // 只报一条可读的项目级错误，绝不按 key 逐个报噪声错误。
    PF_CHECK(CountDiagnostics(result, "E-CITATION-NO-BIBLIOGRAPHY") == 1);
    PF_CHECK(CountDiagnostics(result, "E-CITATION-UNKNOWN-KEY") == 0);
}

// ---------------------------------------------------------------------------
// LaTeX 层：仅语义化，编号策略保持一致
// ---------------------------------------------------------------------------

PF_TEST(GenericArticleUsesCitationOrderBibliographyStyle) {
    const auto* def = TemplateRegistry::Instance().Find("generic-article");
    PF_CHECK(def != nullptr);
    if (def) PF_CHECK(def->bibliography_style == "unsrtnat");
    // IEEE 保留 IEEEtran —— 同样按引用顺序编号。
    const auto* ieee = TemplateRegistry::Instance().Find("ieee-conference");
    if (ieee) PF_CHECK(ieee->bibliography_style == "IEEEtran");
}

PF_TEST(RendererEmitsSemanticCitesNeverNumbers) {
    auto doc = MakeCitingDoc({{"a"}, {"a", "b"}});
    LatexRenderer renderer;
    RenderRequest req;
    req.document = &doc;
    req.template_id = "generic-article";
    req.bibliography_bibtex =
        "@article{a, title={A}}\n@article{b, title={B}}";
    auto result = renderer.Render(req);
    std::string tex;
    for (const auto& f : result.package.files) {
        if (f.path == "main.tex") tex = f.content;
    }
    PF_CHECK(tex.find("\\citep{a}") != std::string::npos);
    PF_CHECK(tex.find("\\citep{a,b}") != std::string::npos);
    // 渲染器不得硬编码显示编号……
    PF_CHECK(tex.find("[1]") == std::string::npos);
    // ……并且样式必须与 GUI 的按引用顺序编号一致。
    PF_CHECK(tex.find("\\bibliographystyle{unsrtnat}") != std::string::npos);
    PF_CHECK(tex.find("natbib") != std::string::npos);
}

// ---------------------------------------------------------------------------
// 参考文献生命周期
// ---------------------------------------------------------------------------

PF_TEST(BibliographyDuplicateKeysSurfacedOnImport) {
    BibliographyDatabase db;
    BibliographyService service(db);
    auto r = service.ImportText(
        "@article{x, title={First}}\n"
        "@article{y, title={Unique}}\n"
        "@article{x, title={Second}}\n");
    PF_CHECK(r.status == BibliographyImportResult::Status::Ok);
    PF_CHECK(r.duplicate_keys.size() == 1);
    if (!r.duplicate_keys.empty()) {
        PF_CHECK(r.duplicate_keys[0] == "x");
    }
    // 后一个定义生效（BibTeX 行为），不会静默重复。
    PF_CHECK(db.Entries().size() == 2);
    PF_CHECK(db.Find("x")->title == "Second");
}

PF_TEST(BibliographyImportedAtomicallyAndReloaded) {
    auto dir = std::filesystem::temp_directory_path() / "pf-citation-bib";
    std::filesystem::remove_all(dir);

    {
        ProjectSession session(TestConfig());
        PF_CHECK(session.NewProject(dir));

        // 引用方案 §7：*成功*导入会立即持久化 references.bib
        // —— 而非仅在保存时。
        auto ok = session.ImportBibliography(
            "@article{smith2024, author={J. Smith}, title={Seminal}, year={2024}}\n");
        PF_CHECK(ok.status == BibliographyImportResult::Status::Ok);
        PF_CHECK(std::filesystem::exists(dir / "references.bib"));
        PF_CHECK(session.state().settings().bibliography_path ==
                 "references.bib");
        // 原子重命名不留下 .tmp 残留。
        PF_CHECK(!std::filesystem::exists(dir / "references.bib.tmp"));

        // 导入失败不得覆盖已存储的参考文献。
        auto bad = session.ImportBibliography("not bibtex at all");
        PF_CHECK(bad.status == BibliographyImportResult::Status::ParseError);
        PF_CHECK(std::filesystem::exists(dir / "references.bib"));
        PF_CHECK(session.bibliography().Find("smith2024") != nullptr);

        session.Save();
        PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
    }

    // 重新打开：参考文献从项目资源中重载（引用 §7）。
    ProjectSession reopened(TestConfig());
    std::string error;
    PF_CHECK(reopened.OpenProject(dir, &error));
    PF_CHECK(reopened.bibliography().Find("smith2024") != nullptr);
    std::filesystem::remove_all(dir);
}

PF_TEST(CitationKeysSurviveSaveAndReload) {
    auto dir = std::filesystem::temp_directory_path() / "pf-citation-roundtrip";
    std::filesystem::remove_all(dir);

    NodeId paragraph_id;
    {
        ProjectSession session(TestConfig());
        PF_CHECK(session.NewProject(dir));
        session.ImportBibliography(
            "@article{smith2024, title={S}}\n@article{jones2020, title={J}}\n");

        EditCommand cmd;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.project_id = session.state().id();
        cmd.base_revision = session.current_revision();
        InsertSectionPayload sec;
        sec.index = 0;
        sec.title = InlineFromText("Intro");
        cmd.payload = sec;
        auto sec_r = session.Execute(cmd);
        PF_CHECK(sec_r.status == EditStatus::Applied);

        InsertParagraphPayload para;
        para.parent = sec_r.created_node;
        para.content = InlineFromText("This method was introduced by ");
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.base_revision = session.current_revision();
        cmd.payload = para;
        auto para_r = session.Execute(cmd);
        PF_CHECK(para_r.status == EditStatus::Applied);
        paragraph_id = para_r.created_node;

        // picker 提交后 InlineEditor 产出的确切内容。
        Citation cit;
        cit.keys = {"smith2024"};
        InlineContent content;
        content.push_back(TextRun{"This method was introduced by ", 0});
        content.push_back(std::move(cit));
        content.push_back(TextRun{" previously.", 0});
        EditParagraphPayload edit;
        edit.paragraph = paragraph_id;
        edit.content = content;
        cmd.operation_id = OperationId(IdGenerator::NewOperationId());
        cmd.base_revision = session.current_revision();
        cmd.payload = edit;
        PF_CHECK(session.Execute(cmd).status == EditStatus::Applied);

        session.Save();
        PF_CHECK(session.FlushSaves().status == SaveResult::Status::Ok);
    }

    ProjectSession reopened(TestConfig());
    std::string error;
    PF_CHECK(reopened.OpenProject(dir, &error));

    const Document& doc = reopened.state().document();
    bool found_citation = false;
    VisitNodes(doc, [&](const NodeAddress& address) {
        if (const auto* block = FindBlock(
                const_cast<Document&>(doc), address.node)) {
            if (const auto* para = std::get_if<Paragraph>(block)) {
                for (const auto& node : para->content) {
                    if (const auto* cit = std::get_if<Citation>(&node)) {
                        found_citation =
                            cit->keys.size() == 1 &&
                            cit->keys[0] == "smith2024";
                    }
                }
            }
        }
    });
    PF_CHECK(found_citation);
    // 重新打开后编号以相同方式重算：key 被存储，
    // 编号是派生出来的（引用方案 §10）。
    auto resolver =
        CitationNumberResolver::Build(doc, reopened.bibliography());
    PF_CHECK(resolver.Find("smith2024")->number == 1);
    std::filesystem::remove_all(dir);
}
