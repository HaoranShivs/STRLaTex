#pragma once
// CLI command implementations.
#include <string>
#include <vector>

namespace pf::cli {

int CmdNew(const std::vector<std::string>& args);
int CmdOpen(const std::vector<std::string>& args);
int CmdBuild(const std::vector<std::string>& args);
int CmdDemo(const std::vector<std::string>& args);
int CmdInfo(const std::vector<std::string>& args);

}  // namespace pf::cli
