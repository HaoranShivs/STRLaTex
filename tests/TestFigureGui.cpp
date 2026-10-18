#include "ScopedTempDir.hpp"
#include "TestMain.hpp"
#include "VectorPdfFixture.hpp"

#include <QApplication>
#include <QCheckBox>
#include <QClipboard>
#include <QComboBox>
#include <QDragEnterEvent>
#include <QDropEvent>
#include <QElapsedTimer>
#include <QFileDialog>
#include <QFileInfo>
#include <QLabel>
#include <QLineEdit>
#include <QMimeData>
#include <QPointer>
#include <QPushButton>
#include <QScrollArea>
#include <QSignalSpy>
#include <QStandardPaths>
#include <QTest>
#include <QTimer>
#include <QUrl>
#include <fstream>

#include "app/AssetConverters.h"
#include "app/BlockEditor.h"
#include "app/FigurePreviewRenderer.h"
#include "app/InlineEditor.h"
#include "app/MainWindow.h"
#include "core/IdGenerator.h"
#include "document/DocumentTraversal.h"
#include "document/InlineText.h"

using namespace pf;
using namespace pf::gui;

namespace {
void Spin(int ms = 60) {
    QTest::qWait(ms);
    QCoreApplication::sendPostedEvents(nullptr, QEvent::DeferredDelete);
}

struct Fixture {
    test::ScopedTempDir directory{"pf-wp05-gui"};
    MainWindow window;
    NodeId section;
    NodeId paragraph;
    QString png;
    QString pdf;
    Fixture() {
        SaveRequest request;
        request.destination = directory / "project.paper";
        request.snapshot.project_id = IdGenerator::NewProjectId();
        request.snapshot.template_id = "generic-article";
        DocumentEditor editor(request.snapshot.document);
        editor.SetTitle(InlineFromText("Figure workflow"));
        section = editor.InsertSection(0, InlineFromText("Results")).value();
        Paragraph text;
        text.content = InlineFromText("Before image");
        paragraph = editor.InsertBlock(section, std::nullopt, text).value();
        PF_CHECK(ProjectPersistence::Save(request).status ==
                 SaveResult::Status::Ok);
        png = QString::fromStdString((directory / "raster plot.png").string());
        QImage image(180, 90, QImage::Format_ARGB32);
        image.fill(Qt::blue);
        PF_CHECK(image.save(png));
        pdf = QString::fromStdString((directory / "vector plot.pdf").string());
        test::WriteVectorPdf(directory / "vector plot.pdf");
        window.resize(1400, 900);
        PF_CHECK(
            window.OpenProjectDir(QString::fromStdString(directory.string())));
        window.show();
        QApplication::setActiveWindow(&window);
        Spin();
    }
    ~Fixture() {
        QApplication::clipboard()->clear();
    }
    BlockEditor* Editor() {
        return window.findChild<BlockEditor*>();
    }
    QWidget* Card(const NodeId& id) {
        for (QWidget* card : window.findChildren<QWidget*>("blockCard"))
            if (card->isVisible() && card->property("row_node").toString() ==
                                         QString::fromStdString(id.value()))
                return card;
        return nullptr;
    }
    InlineEditor* Text() {
        return Card(paragraph)->findChild<InlineEditor*>();
    }
    std::vector<Figure> Figures() {
        std::vector<Figure> figures;
        VisitBlocks(window.controller()->session().state().document(),
                    [&](const Block& block, const NodeAddress&) {
                        if (const auto* figure = std::get_if<Figure>(&block))
                            figures.push_back(*figure);
                    });
        return figures;
    }
    QPoint ScrollTo(QWidget* target) {
        auto* scroll = Editor()->findChild<QScrollArea*>();
        const QPoint point =
            target->mapTo(scroll->widget(), target->rect().center());
        scroll->ensureVisible(point.x(), point.y(), 2, 2);
        Spin(20);
        return target->rect().center();
    }
    void Drop(QWidget* target, const QMimeData& mime) {
        const QPoint point = ScrollTo(target);
        QDragEnterEvent enter(point, Qt::CopyAction, &mime, Qt::LeftButton,
                              Qt::NoModifier);
        QApplication::sendEvent(target, &enter);
        PF_CHECK(enter.isAccepted());
        QDragMoveEvent move(point, Qt::CopyAction, &mime, Qt::LeftButton,
                            Qt::NoModifier);
        QApplication::sendEvent(target, &move);
        PF_CHECK(move.isAccepted());
        QDropEvent drop(point, Qt::CopyAction, &mime, Qt::LeftButton,
                        Qt::NoModifier);
        QApplication::sendEvent(target, &drop);
        PF_CHECK(drop.isAccepted() && drop.dropAction() == Qt::CopyAction);
        Spin(100);
    }
};

QString Executable(const std::filesystem::path& path,
                   const std::string& script) {
    std::ofstream(path) << "#!/bin/sh\n" << script;
    std::filesystem::permissions(path, std::filesystem::perms::owner_all);
    return QString::fromStdString(path.string());
}
} // namespace

PF_TEST(WP05DroppingPngPdfAndMultipleFilesUsesThePointedBlockOrGap) {
    Fixture fixture;
    QWidget* gap = nullptr;
    for (QWidget* widget : fixture.Editor()->findChildren<QWidget*>())
        if (widget->property("gap_anchor").toString() ==
            QString::fromStdString(fixture.paragraph.value()))
            gap = widget;
    PF_CHECK(gap);
    if (!gap)
        return;
    QMimeData mime;
    mime.setUrls(
        {QUrl::fromLocalFile(fixture.png), QUrl::fromLocalFile(fixture.pdf)});
    fixture.Drop(gap, mime);
    auto figures = fixture.Figures();
    PF_CHECK(figures.size() == 2);
    if (figures.size() != 2)
        return;
    auto& assets = fixture.window.controller()->session().assets();
    PF_CHECK(assets.registry().Find(figures[0].asset_id)->kind ==
             AssetKind::Raster);
    PF_CHECK(assets.registry().Find(figures[1].asset_id)->kind ==
             AssetKind::Pdf);
    // A child viewport receives the event, and its parent card is the anchor.
    QMimeData another;
    another.setUrls({QUrl::fromLocalFile(fixture.pdf)});
    fixture.Drop(fixture.Text()->viewport(), another);
    figures = fixture.Figures();
    PF_CHECK(figures.size() == 3);
    PF_CHECK(assets.registry().Find(figures[0].asset_id)->kind ==
             AssetKind::Pdf);
    PF_CHECK(assets.registry().Find(figures[1].asset_id)->kind ==
             AssetKind::Raster);
    fixture.window.controller()->Undo();
    PF_CHECK(fixture.Figures().size() == 2);
    fixture.window.controller()->Redo();
    PF_CHECK(fixture.Figures().size() == 3);
}

PF_TEST(WP05ImagePasteCommitsPendingTextAndCleansTemporaryPng) {
    Fixture fixture;
    auto* text = fixture.Text();
    fixture.ScrollTo(text);
    text->setFocus();
    text->moveCursor(QTextCursor::End);
    QTest::keyClicks(text, " retained");
    QImage image(80, 40, QImage::Format_ARGB32);
    image.fill(Qt::green);
    QApplication::clipboard()->setImage(image);
    QSignalSpy pasted(fixture.Editor(), &BlockEditor::AssetPasted);
    QTest::keyClick(text, Qt::Key_V, Qt::ControlModifier);
    Spin(100);
    PF_CHECK(fixture.Figures().size() == 1 && pasted.size() == 1);
    if (pasted.size() != 1)
        return;
    PF_CHECK(!QFileInfo::exists(pasted[0][0].toString()));
    const auto* stored =
        FindBlock(fixture.window.controller()->session().mutable_document(),
                  fixture.paragraph);
    PF_CHECK(stored &&
             InlineToPlainText(std::get<Paragraph>(*stored).content) ==
                 "Before image retained");
    auto& assets = fixture.window.controller()->session().assets();
    const auto* metadata =
        assets.registry().Find(fixture.Figures().front().asset_id);
    PF_CHECK(metadata && metadata->width == 80 && metadata->height == 40);
    PF_CHECK(metadata && std::filesystem::exists(assets.assets_dir() /
                                                 metadata->relative_path));
    // QTextEdit's context-menu Paste calls paste() without a key event.
    fixture.Text()->paste();
    Spin(100);
    PF_CHECK(fixture.Figures().size() == 2);
    PF_CHECK(pasted.size() == 2 && !QFileInfo::exists(pasted[1][0].toString()));
    QApplication::clipboard()->setText(" ordinary text");
    text = fixture.Text();
    text->moveCursor(QTextCursor::End);
    text->paste();
    fixture.Editor()->CommitFocused();
    PF_CHECK(fixture.Figures().size() == 2);
    PF_CHECK(InlineToPlainText(text->Content()).find("ordinary text") !=
             std::string::npos);
}

PF_TEST(WP05ReplaceButtonKeepsCaptionLayoutLabelAndRoundTrips) {
    Fixture fixture;
    auto* controller = fixture.window.controller();
    const NodeId id =
        controller->InsertFigureAfter(fixture.paragraph, fixture.png)
            .created_node;
    const InlineContent caption{
        TextRun{"Rich caption", static_cast<std::uint8_t>(TextMark::Strong)}};
    controller->EditCaptionRich(id, caption);
    controller->EditFigureLabel(id, "fig:kept");
    controller->EditFigureWidth(id, FigureWidth::Percent50);
    controller->EditFigureSpan(id, true);
    Spin();
    const AssetId old_asset = fixture.Figures().front().asset_id;
    auto* button =
        fixture.Card(id)->findChild<QPushButton*>("replaceFigureAsset");
    PF_CHECK(button);
    if (!button)
        return;
    QSignalSpy requested(fixture.Editor(),
                         &BlockEditor::FigureAssetReplaceRequested);
    QTimer::singleShot(0, &fixture.window, [&]() {
        auto* dialog =
            qobject_cast<QFileDialog*>(QApplication::activeModalWidget());
        PF_CHECK(dialog);
        if (dialog) {
            dialog->selectFile(fixture.pdf);
            QMetaObject::invokeMethod(dialog, "accept", Qt::QueuedConnection);
        }
    });
    button->click();
    Spin();
    PF_CHECK(requested.size() == 1);
    const auto replaced = fixture.Figures().front();
    PF_CHECK(replaced.id == id && replaced.asset_id != old_asset);
    PF_CHECK(replaced.caption == caption && replaced.label == "fig:kept");
    PF_CHECK(replaced.width == FigureWidth::Percent50 &&
             replaced.span == FigureSpan::DoubleColumn);
    controller->Undo();
    PF_CHECK(fixture.Figures().front().asset_id == old_asset);
    controller->Redo();
    controller->Save();
    controller->FlushSaves();
    PF_CHECK(controller->persistence_state() == PersistenceState::Clean);
    PF_CHECK(fixture.window.OpenProjectDir(
        QString::fromStdString(fixture.directory.string())));
    Spin();
    PF_CHECK(fixture.Figures().front().asset_id == replaced.asset_id);
    auto* card = fixture.Card(id);
    PF_CHECK(card->findChild<QComboBox*>("figureWidth")->currentIndex() == 1);
    PF_CHECK(card->findChild<InlineEditor*>("captionEditor")->Content() ==
             caption);
    PF_CHECK(fixture.Figures().front().label == "fig:kept");
    if (!QStandardPaths::findExecutable("pdftoppm").isEmpty()) {
        PF_CHECK(QTest::qWaitFor(
            [&]() {
                auto* current_card = fixture.Card(id);
                auto* image =
                    current_card
                        ? current_card->findChild<QLabel*>("figurePreview")
                        : nullptr;
                return image && !image->pixmap().isNull();
            },
            10000));
        auto* image = fixture.Card(id)->findChild<QLabel*>("figurePreview");
        PF_CHECK(qAbs(image->pixmap().width() - qRound(image->width() * 0.5)) <=
                 1);
    }
}

PF_TEST(WP05PdfThumbnailIsAsynchronousCachedAndContainsVectorDrawing) {
    if (QStandardPaths::findExecutable("pdftoppm").isEmpty()) {
        std::cout << "  (pdftoppm unavailable; real thumbnail check skipped)\n";
        return;
    }
    test::ScopedTempDir directory("pf-wp05-preview");
    test::WriteVectorPdf(directory / "figure.pdf");
    const auto path =
        QString::fromStdString((directory / "figure.pdf").string());
    FigurePreviewRenderer renderer;
    QSignalSpy ready(&renderer, &FigurePreviewRenderer::PreviewReady);
    QElapsedTimer timer;
    timer.start();
    PF_CHECK(renderer.Request(path, QSize(400, 400)).isNull());
    PF_CHECK(timer.elapsed() < 100);
    PF_CHECK(renderer.Request(path, QSize(400, 400)).isNull());
    PF_CHECK(QTest::qWaitFor([&]() { return ready.size() == 1; }, 10000));
    const QImage thumbnail = renderer.Request(path, QSize(400, 400)).toImage();
    PF_CHECK(!thumbnail.isNull() && thumbnail.width() <= 400 &&
             thumbnail.height() <= 400);
    int red_pixels = 0;
    for (int y = 0; y < thumbnail.height(); y += 5)
        for (int x = 0; x < thumbnail.width(); x += 5) {
            const auto color = thumbnail.pixelColor(x, y);
            red_pixels +=
                color.red() > 200 && color.green() < 40 && color.blue() < 40;
        }
    PF_CHECK(red_pixels > 20);
    Spin();
    PF_CHECK(ready.size() == 1);
}

PF_TEST(WP05MissingPdfPreviewToolReturnsPlaceholderWithoutRetryLoop) {
    test::ScopedTempDir directory("pf-wp05-no-preview");
    test::WriteVectorPdf(directory / "figure.pdf");
    FigurePreviewRenderer renderer(
        nullptr,
        QString::fromStdString((directory / "missing-pdftoppm").string()));
    QSignalSpy ready(&renderer, &FigurePreviewRenderer::PreviewReady);
    const auto path =
        QString::fromStdString((directory / "figure.pdf").string());
    PF_CHECK(renderer.Request(path, QSize(400, 400)).isNull());
    PF_CHECK(QTest::qWaitFor([&]() { return ready.size() == 1; }));
    PF_CHECK(renderer.Request(path, QSize(400, 400)).isNull());
    Spin();
    PF_CHECK(ready.size() == 1);
}

PF_TEST(WP05PlatformConvertersHandleArgumentsMissingToolsErrorsAndTimeouts) {
    test::ScopedTempDir directory("pf-wp05-process-converter");
    const auto source = directory / "input with spaces.svg";
    test::WriteVectorPdf(source);
    std::string error;
    ProcessAssetConverter missing(
        QString::fromStdString((directory / "missing-rsvg").string()));
    PF_CHECK(!missing.Convert(source, directory, &error));
    PF_CHECK(error.find("rsvg-convert") != std::string::npos);
#if !defined(_WIN32)
    const QString svg = Executable(
        directory / "fake-rsvg", "[ \"$1\" = -f ] && [ \"$2\" = pdf ] && [ "
                                 "\"$3\" = -o ] || exit 8\ncp \"$5\" \"$4\"\n");
    const QString eps = Executable(directory / "fake-epstopdf",
                                   "cp \"$2\" \"${1#--outfile=}\"\n");
    ProcessAssetConverter converter(svg, eps);
    PF_CHECK(converter.Convert(source, directory, &error).has_value());
    const auto eps_source = directory / "input.eps";
    test::WriteVectorPdf(eps_source);
    PF_CHECK(converter.Convert(eps_source, directory, &error).has_value());
    const QString failing =
        Executable(directory / "fail-rsvg", "printf 'bad svg' >&2\nexit 7\n");
    ProcessAssetConverter failed(failing);
    PF_CHECK(!failed.Convert(source, directory, &error));
    PF_CHECK(error.find("bad svg") != std::string::npos);
    const QString slow =
        Executable(directory / "slow-rsvg", "exec /bin/sleep 5\n");
    ProcessAssetConverter timeout(slow, eps, 30);
    QElapsedTimer timer;
    timer.start();
    PF_CHECK(!timeout.Convert(source, directory, &error));
    PF_CHECK(error.find("timed out") != std::string::npos &&
             timer.elapsed() < 2000);
#endif
}
