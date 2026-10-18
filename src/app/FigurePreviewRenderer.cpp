#include "app/FigurePreviewRenderer.h"

#include <QDateTime>
#include <QFileInfo>
#include <QImageReader>
#include <QProcess>

namespace pf::gui {

FigurePreviewRenderer::FigurePreviewRenderer(QObject* parent,
                                             QString pdf_program)
    : QObject(parent), pdf_program_(std::move(pdf_program)) {
    timeout_.setSingleShot(true);
    timeout_.setInterval(15000);
    connect(&timeout_, &QTimer::timeout, this, [this]() { Finish(false); });
}

FigurePreviewRenderer::~FigurePreviewRenderer() {
    if (process_) {
        process_->disconnect(this);
        process_->kill();
        process_->waitForFinished(1000);
    }
}

QPixmap FigurePreviewRenderer::Request(const QString& path,
                                       const QSize& target) {
    const QFileInfo file(path);
    if (!file.isFile())
        return {};
    const int pixels = qBound(64, qMax(target.width(), target.height()), 1600);
    const QString key =
        file.absoluteFilePath() + ':' + QString::number(file.size()) + ':' +
        QString::number(file.lastModified().toMSecsSinceEpoch()) + ':' +
        QString::number(pixels);
    if (const QPixmap* cached = cache_.object(key))
        return *cached;
    if (file.suffix().compare("pdf", Qt::CaseInsensitive) != 0) {
        QImageReader reader(path);
        reader.setAutoTransform(true);
        if (reader.size().isValid())
            reader.setScaledSize(
                reader.size().scaled(pixels, pixels, Qt::KeepAspectRatio));
        QPixmap result = QPixmap::fromImage(reader.read());
        cache_.insert(key, new QPixmap(result),
                      qMax(1, result.width() * result.height() * 4 / 1024));
        return result;
    }
    if (!pending_.contains(key)) {
        pending_.insert(key);
        jobs_.enqueue({file.absoluteFilePath(), key, pixels});
        QTimer::singleShot(0, this, [this]() { StartNext(); });
    }
    return {};
}

void FigurePreviewRenderer::StartNext() {
    if (process_ || jobs_.isEmpty())
        return;
    current_ = jobs_.dequeue();
    scratch_ = std::make_unique<QTemporaryDir>();
    process_ = new QProcess(this);
    if (!scratch_->isValid()) {
        Finish(false);
        return;
    }
    connect(process_,
            QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), this,
            [this](int code, QProcess::ExitStatus status) {
                Finish(code == 0 && status == QProcess::NormalExit);
            });
    connect(process_, &QProcess::errorOccurred, this,
            [this](QProcess::ProcessError error) {
                if (error == QProcess::FailedToStart)
                    Finish(false);
            });
    timeout_.start();
    process_->start(pdf_program_,
                    {"-f", "1", "-l", "1", "-singlefile", "-scale-to",
                     QString::number(current_.pixels), "-png", current_.path,
                     scratch_->path() + "/preview"});
}

void FigurePreviewRenderer::Finish(bool success) {
    if (!process_)
        return;
    timeout_.stop();
    process_->disconnect(this);
    if (process_->state() != QProcess::NotRunning) {
        process_->kill();
        process_->waitForFinished(1000);
    }
    process_->deleteLater();
    process_.clear();
    const QPixmap result = success && scratch_
                               ? QPixmap(scratch_->path() + "/preview.png")
                               : QPixmap();
    scratch_.reset();
    // Cache failure too; refreshing another figure must not repeatedly retry a
    // missing tool or an unreadable PDF. A changed file gets a different key.
    cache_.insert(current_.key, new QPixmap(result),
                  qMax(1, result.width() * result.height() * 4 / 1024));
    pending_.remove(current_.key);
    emit PreviewReady(current_.path);
    QTimer::singleShot(0, this, [this]() { StartNext(); });
}

} // namespace pf::gui
