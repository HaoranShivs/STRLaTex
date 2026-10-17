#include "ScopedTempDir.hpp"
#include "TestMain.hpp"

#include <QCoreApplication>
#include <QLabel>
#include <QPainter>
#include <QPdfWriter>
#include <QScrollArea>
#include <QScrollBar>
#include <QStandardPaths>
#include <QTest>

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

void CheckPreviewFitsWidth(PdfPreview* preview) {
    auto* scroll = preview->findChild<QScrollArea*>();
    auto* page = preview->findChild<QLabel*>("pdfPageSheet");
    PF_CHECK(scroll != nullptr && page != nullptr);
    if (!scroll || !page)
        return;
    PF_CHECK(QTest::qWaitFor([&]() {
        return preview->isVisible() && page->width() == scroll->viewport()->width() &&
               scroll->horizontalScrollBar()->maximum() == 0;
    }));
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
        window.resize(1400 + launch * 160, 900);
        // Cover both opening from the welcome page and loading before show().
        if (launch == 0) {
            window.show();
            QTest::qWait(30);
        }
        PF_CHECK(window.OpenProjectDir(QString::fromStdString(project.string())));
        window.show();
        auto* preview = window.findChild<PdfPreview*>();
        PF_CHECK(preview != nullptr);
        if (preview) {
            PF_CHECK(preview->pageCount() == 2);
            CheckPreviewFitsWidth(preview);
        }
        PF_CHECK(window.controller()->session().latest_build_id().empty());
        PF_CHECK(window.controller()->session().preview_state() == PreviewState::Stale);
    }
}

PF_TEST(PdfFitWidthTracksTheViewportAndPreservesManualZoom) {
    if (!CanRenderPreview())
        return;
    test::ScopedTempDir project("pf-gui-preview-fit");
    WritePreviewPdf(project / "project.pdf", 2);
    PdfPreview preview;
    preview.resize(720, 600);
    preview.SetDocument(QString::fromStdString((project / "project.pdf").string()));
    preview.show();
    CheckPreviewFitsWidth(&preview);

    preview.resize(610, 550);
    CheckPreviewFitsWidth(&preview);
    auto* scroll = preview.findChild<QScrollArea*>();
    // Scrollbar changes resize the viewport without resizing PdfPreview itself.
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOff);
    CheckPreviewFitsWidth(&preview);
    scroll->setVerticalScrollBarPolicy(Qt::ScrollBarAlwaysOn);
    CheckPreviewFitsWidth(&preview);

    preview.SetZoom(1.25);
    preview.resize(680, 600);
    QTest::qWait(100);
    PF_CHECK(preview.zoom() == 1.25);
    PF_CHECK(scroll->horizontalScrollBar()->maximum() > 0);
    preview.FitWidth();
    CheckPreviewFitsWidth(&preview);
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
