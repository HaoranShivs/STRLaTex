#include <iostream>
#include <string>
#include <vector>

#include "cli/Commands.h"

namespace pf::cli {
extern std::string PF_TECTONIC_PATH;
}

int main(int argc, char** argv) {
    std::vector<std::string> args(argv + 1, argv + argc);
    if (args.empty()) {
        std::cerr
            << "PaperForge V1 (core + CLI)\n\n"
            << "usage: paperforge <command> [args]\n\n"
            << "commands:\n"
            << "  new <dir>      create a new project with starter content\n"
            << "  info <dir>     show project info\n"
            << "  build <dir>    build PDF via tectonic\n"
            << "  demo [dir]     full end-to-end demo (create+edit+build)\n";
        return 2;
    }
    std::string cmd = args[0];
    std::vector<std::string> rest(args.begin() + 1, args.end());
    if (cmd == "new") return pf::cli::CmdNew(rest);
    if (cmd == "build") return pf::cli::CmdBuild(rest);
    if (cmd == "demo") return pf::cli::CmdDemo(rest);
    if (cmd == "info") return pf::cli::CmdInfo(rest);
    std::cerr << "unknown command: " << cmd << "\n";
    return 2;
}
