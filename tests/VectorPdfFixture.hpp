#pragma once

#include <filesystem>
#include <fstream>
#include <iomanip>
#include <sstream>
#include <string>
#include <vector>

namespace pf::test {

// A real one-page vector PDF, shared by domain, GUI and TeX integration tests.
inline void WriteVectorPdf(const std::filesystem::path& path) {
    const std::string drawing = "1 0 0 rg 10 10 120 35 re f\nBT /F1 12 Tf 10 "
                                "65 Td (WP05 vector graphic) Tj ET\n";
    const std::vector<std::string> objects = {
        "<< /Type /Catalog /Pages 2 0 R >>",
        "<< /Type /Pages /Kids [3 0 R] /Count 1 >>",
        "<< /Type /Page /Parent 2 0 R /MediaBox [0 0 180 90] /Resources << "
        "/Font << /F1 5 0 R >> >> /Contents 4 0 R >>",
        "<< /Length " + std::to_string(drawing.size()) + " >>\nstream\n" +
            drawing + "endstream",
        "<< /Type /Font /Subtype /Type1 /BaseFont /Helvetica >>"};
    std::ostringstream pdf;
    pdf << "%PDF-1.4\n";
    std::vector<std::streamoff> offsets;
    for (size_t i = 0; i < objects.size(); ++i) {
        offsets.push_back(pdf.tellp());
        pdf << i + 1 << " 0 obj\n" << objects[i] << "\nendobj\n";
    }
    const auto xref = pdf.tellp();
    pdf << "xref\n0 " << objects.size() + 1 << "\n0000000000 65535 f \n";
    for (const auto offset : offsets)
        pdf << std::setfill('0') << std::setw(10) << offset << " 00000 n \n";
    pdf << "trailer\n<< /Size " << objects.size() + 1
        << " /Root 1 0 R >>\nstartxref\n"
        << xref << "\n%%EOF\n";
    std::ofstream(path, std::ios::binary) << pdf.str();
}

} // namespace pf::test
