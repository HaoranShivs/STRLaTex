#pragma once

#include <QCache>
#include <QObject>
#include <QPixmap>
#include <QPointer>
#include <QQueue>
#include <QSet>
#include <QTemporaryDir>
#include <QTimer>
#include <memory>

class QProcess;

namespace pf::gui {

// One bounded PDF rasterization at a time; no external process blocks typing.
class FigurePreviewRenderer final : public QObject {
    Q_OBJECT
  public:
    explicit FigurePreviewRenderer(
        QObject* parent = nullptr,
        QString pdf_program = QStringLiteral("pdftoppm"));
    ~FigurePreviewRenderer() override;
    QPixmap Request(const QString& path, const QSize& target);

  signals:
    void PreviewReady(const QString& path);

  private:
    struct Job {
        QString path;
        QString key;
        int pixels = 1200;
    };
    void StartNext();
    void Finish(bool success);
    QString pdf_program_;
    QCache<QString, QPixmap> cache_{32768}; // KiB
    QQueue<Job> jobs_;
    QSet<QString> pending_;
    Job current_;
    QPointer<QProcess> process_;
    QTimer timeout_;
    std::unique_ptr<QTemporaryDir> scratch_;
};

} // namespace pf::gui
