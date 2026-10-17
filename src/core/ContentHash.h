#pragma once
#include <string>
#include <string_view>

namespace pf {
std::string Sha256(std::string_view bytes);
}
