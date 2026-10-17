#include "ScopedTempDir.hpp"
#include "TestMain.hpp"

#include <QCoreApplication>
#include <QPainter>
#include <QPdfWriter>
#include <QStandardPaths>

#include "app/MainWindow.h"
#include "app/PdfPreview.h"
#include "core/IdGenerator.h"
#include "persistence/ProjectPersistence.h"

using namespace pf;
using namespace pf::gui;

namespace {

void SavePreviewProject(const std::filesystem::path& directory) {
    SaveRequest request;
    request.destination = directory / "project.paper";
    request.snapshot.project_id = IdGenerator::NewProjectId();
    request.snapshot.template_id = "generic-article";
    PF_CHECK(ProjectPersistence::Save(request).status == SaveResult::Status::Ok);
}

void WritePreviewPdf(const std::filesystem::path& path, int pages) {
    QPdfWriter writer(QString::fromStdString(path.string()));
    QPainter painter(&writer);
    PF_CHECK(painter.isActive());
    for (int page = 0; page < pages; ++page) {
        painter.drawText(100, 100, QStringLiteral("Saved preview page %1").arg(page + 1));
        if (page + 1 < pages)
            PF_CHECK(writer.newPage());
    }
}

bool CanRenderPreview() {
    if (!QStandardPaths::findExecutable("pdftoppm").isEmpty())
        return true;
    std::cout << "  (pdftoppm unavailable; PDF page rendering skipped)\n";
    return false;
}

} // namespace

PF_TEST(ReopeningTheEditorRestoresSavedPdfPagesWithoutBuilding) {
    if (!CanRenderPreview())
        return;
    test::ScopedTempDir project("pf-gui-saved-preview");
    SavePreviewProject(project);
    WritePreviewPdf(project / "project.pdf", 2);
    // Distinct window instances exercise startup, rather than retained widgets.
    for (int launch = 0; launch < 2; ++launch) {
        MainWindow window;
        PF_CHECK(window.controller()->OpenProject(QString::fromStdString(project.string())));
        auto* preview = window.findChild<PdfPreview*>();
        PF_CHECK(preview != nullptr);
        if (preview)
            PF_CHECK(preview->pageCount() == 2);
        PF_CHECK(window.controller()->session().latest_build_id().empty());
        PF_CHECK(window.controller()->session().preview_state() == PreviewState::Stale);
    }
}

PF_TEST(SwitchingProjectsClearsOrRestoresOnlyTheirOwnPreview) {
    if (!CanRenderPreview())
        return;
    test::ScopedTempDir first("pf-gui-preview-first");
    test::ScopedTempDir empty("pf-gui-preview-empty");
    test::ScopedTempDir legacy("pf-gui-preview-legacy");
    SavePreviewProject(first);
    SavePreviewProject(empty);
    SavePreviewProject(legacy);
    WritePreviewPdf(first / "project.pdf", 2);
    const auto legacy_pdf = legacy / ".paperforge/build/main.pdf";
    std::filesystem::create_directories(legacy_pdf.parent_path());
    WritePreviewPdf(legacy_pdf, 3);

    MainWindow window;
    auto* controller = window.controller();
    auto* preview = window.findChild<PdfPreview*>();
    PF_CHECK(!controller->has_project());
    PF_CHECK(preview != nullptr);
    if (!preview)
        return;
    PF_CHECK(controller->OpenProject(QString::fromStdString(first.string())));
    PF_CHECK(controller->has_project());
    PF_CHECK(preview->pageCount() == 2);
    PF_CHECK(!controller->OpenProject(QString::fromStdString((empty / "missing-project").string())));
    PF_CHECK(!controller->has_project());
    PF_CHECK(preview->pageCount() == 0);
    PF_CHECK(controller->OpenProject(QString::fromStdString(empty.string())));
    PF_CHECK(preview->pageCount() == 0);
    PF_CHECK(!controller->session().preview_pdf().valid());
    PF_CHECK(controller->OpenProject(QString::fromStdString(legacy.string())));
    PF_CHECK(preview->pageCount() == 3);
    controller->CloseProject();
    PF_CHECK(!controller->has_project());
    PF_CHECK(preview->pageCount() == 0);
}
