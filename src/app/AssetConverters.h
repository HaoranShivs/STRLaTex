#pragma once

#include "asset/AssetManager.h"
#include <QString>

namespace pf::gui {

// Optional platform tools. The domain layer only knows IAssetConverter.
class ProcessAssetConverter final : public IAssetConverter {
  public:
    explicit ProcessAssetConverter(
        QString svg_program = QStringLiteral("rsvg-convert"),
        QString eps_program = QStringLiteral("epstopdf"),
        int timeout_ms = 15000);
    std::optional<std::filesystem::path>
    Convert(const std::filesystem::path& source,
            const std::filesystem::path& work_dir, std::string* error) override;

  private:
    QString svg_program_;
    QString eps_program_;
    int timeout_ms_;
};

} // namespace pf::gui
