#include "app/AssetConverters.h"

#include <QDir>
#include <QFileInfo>
#include <QProcess>
#include <QStandardPaths>

namespace pf::gui {

ProcessAssetConverter::ProcessAssetConverter(QString svg_program,
                                             QString eps_program,
                                             int timeout_ms)
    : svg_program_(std::move(svg_program)),
      eps_program_(std::move(eps_program)), timeout_ms_(timeout_ms) {}

std::optional<std::filesystem::path>
ProcessAssetConverter::Convert(const std::filesystem::path& source,
                               const std::filesystem::path& work_dir,
                               std::string* error) {
    const bool svg = QString::fromStdString(source.extension().string())
                         .compare(".svg", Qt::CaseInsensitive) == 0;
    const QString program = svg ? svg_program_ : eps_program_;
    const auto fail =
        [error](const QString& detail) -> std::optional<std::filesystem::path> {
        if (error)
            *error = detail.toStdString();
        return std::nullopt;
    };
    if (QStandardPaths::findExecutable(program).isEmpty()) {
        return fail(
            svg ? QStringLiteral("SVG import requires rsvg-convert (librsvg). "
                                 "Install it on PATH, or export as PDF.")
                : QStringLiteral(
                      "EPS import requires epstopdf and Ghostscript. Install "
                      "them on PATH, or export as PDF."));
    }
    const QString input =
        QFileInfo(QString::fromStdString(source.string())).absoluteFilePath();
    const QString output = QDir(QString::fromStdString(work_dir.string()))
                               .absoluteFilePath("converted.pdf");
    QProcess process;
    process.setWorkingDirectory(QString::fromStdString(work_dir.string()));
    process.start(program, svg ? QStringList{"-f", "pdf", "-o", output, input}
                               : QStringList{"--outfile=" + output, input});
    if (!process.waitForFinished(timeout_ms_)) {
        const bool failed_to_start = process.error() == QProcess::FailedToStart;
        process.kill();
        process.waitForFinished(1000);
        return fail(failed_to_start
                        ? program + ": " + process.errorString()
                        : program + ": image conversion timed out. Export the "
                                    "figure as PDF and try again.");
    }
    if (process.exitStatus() != QProcess::NormalExit ||
        process.exitCode() != 0 || !QFileInfo(output).isFile() ||
        QFileInfo(output).size() == 0) {
        return fail(
            program + ": image conversion failed. " +
            QString::fromUtf8(process.readAllStandardError()).left(2000));
    }
    return std::filesystem::path(output.toStdString());
}

} // namespace pf::gui
