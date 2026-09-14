#include "build/Compiler.h"

#include <cstdlib>
#include <chrono>
#include <fstream>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pf {
namespace {

std::string QuoteShell(const std::string& value) {
    std::string quoted = "'";
    for (char c : value) {
        if (c == '\'') {
            quoted += "'\\''";
        } else {
            quoted += c;
        }
    }
    quoted += '\'';
    return quoted;
}

bool StagePackage(const CompileRequest& request, std::string* error) {
    std::error_code ec;
    std::filesystem::create_directories(request.workspace, ec);
    if (ec) {
        *error = "cannot create build workspace: " + ec.message();
        return false;
    }
    for (const auto& file : request.package.files) {
        const auto destination = request.workspace / file.path;
        std::filesystem::create_directories(destination.parent_path(), ec);
        std::ofstream output(destination, std::ios::binary | std::ios::trunc);
        if (!output) {
            *error = "cannot write generated file: " + destination.string();
            return false;
        }
        output << file.content;
    }
    for (const auto& [destination_name, source] : request.asset_sources) {
        const auto destination = request.workspace / destination_name;
        std::filesystem::copy_file(
            source, destination,
            std::filesystem::copy_options::overwrite_existing, ec);
        if (ec) {
            *error = "cannot stage asset " + source.string() + ": " +
                     ec.message();
            return false;
        }
    }
    return true;
}

std::vector<CompilerMessage> ParseMessages(const std::string& log) {
    std::vector<CompilerMessage> messages;
    std::istringstream lines(log);
    std::string line;
    while (std::getline(lines, line)) {
        CompilerMessage message;
        const size_t first = line.find(':');
        const size_t second =
            first == std::string::npos ? std::string::npos
                                       : line.find(':', first + 1);
        if (first != std::string::npos && second != std::string::npos) {
            try {
                message.file = line.substr(0, first);
                message.line = static_cast<std::uint32_t>(
                    std::stoul(line.substr(first + 1, second - first - 1)));
                message.text = line.substr(second + 1);
                message.is_error =
                    message.text.find("warning") == std::string::npos &&
                    message.text.find("Warning") == std::string::npos;
                messages.push_back(std::move(message));
                continue;
            } catch (...) {
            }
        }
        if (line.find("error") != std::string::npos ||
            line.find("Error") != std::string::npos) {
            message.file = "main.tex";
            message.line = 1;
            message.text = line;
            messages.push_back(std::move(message));
        }
    }
    return messages;
}

int RunCommand(const std::string& command,
               const std::atomic<bool>* cancel_requested,
               bool* cancelled) {
#ifdef _WIN32
    (void)cancel_requested;
    return std::system(command.c_str());
#else
    const pid_t child = fork();
    if (child == 0) {
        setpgid(0, 0);
        execl("/bin/sh", "sh", "-c", command.c_str(),
              static_cast<char*>(nullptr));
        _exit(127);
    }
    if (child < 0) return -1;

    const auto deadline =
        std::chrono::steady_clock::now() + std::chrono::minutes(2);
    int status = 0;
    while (waitpid(child, &status, WNOHANG) == 0) {
        if ((cancel_requested && cancel_requested->load()) ||
            std::chrono::steady_clock::now() >= deadline) {
            *cancelled = true;
            kill(-child, SIGTERM);
            if (waitpid(child, &status, 0) < 0) return -1;
            break;
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
    }
    return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

}  // namespace

TectonicCompiler::TectonicCompiler(std::string executable)
    : executable_(std::move(executable)) {
    std::error_code ec;
    if (!executable_.empty() &&
        std::filesystem::exists(executable_, ec)) {
        executable_ = std::filesystem::absolute(executable_, ec).string();
    }
}

CompileResult TectonicCompiler::Compile(
    const CompileRequest& request,
    const std::atomic<bool>* cancel_requested) {
    CompileResult result;
    if (cancel_requested && cancel_requested->load()) {
        result.status = CompileStatus::Cancelled;
        return result;
    }
    if (executable_.empty()) {
        result.log = "tectonic executable was not configured";
        result.messages.push_back(
            {"main.tex", 1, true, result.log});
        return result;
    }
    std::string stage_error;
    if (!StagePackage(request, &stage_error)) {
        result.log = stage_error;
        result.messages.push_back({"main.tex", 1, true, stage_error});
        return result;
    }

    const auto log_path = request.workspace / "build.log";
    const std::string command =
        "cd " + QuoteShell(request.workspace.string()) + " && " +
        QuoteShell(executable_) + " -X compile --keep-logs --outdir " +
        QuoteShell(request.workspace.string()) + " " +
        QuoteShell(request.package.entry_file) + " >" +
        QuoteShell(log_path.string()) + " 2>&1";
    bool cancelled = false;
    const int exit_code =
        RunCommand(command, cancel_requested, &cancelled);

    std::ifstream log_file(log_path, std::ios::binary);
    std::ostringstream log;
    log << log_file.rdbuf();
    result.log = log.str();
    result.messages = ParseMessages(result.log);
    result.pdf_path =
        request.workspace /
        (std::filesystem::path(request.package.entry_file).stem().string() +
         ".pdf");
    if (cancelled || (cancel_requested && cancel_requested->load())) {
        result.status = CompileStatus::Cancelled;
    } else if (exit_code == 0 && std::filesystem::exists(result.pdf_path)) {
        result.status = CompileStatus::Success;
    } else {
        result.status = CompileStatus::Failure;
        if (result.messages.empty()) {
            result.messages.push_back(
                {"main.tex", 1, true, "Tectonic compilation failed"});
        }
    }
    return result;
}

CompileResult MockCompiler::Compile(
    const CompileRequest& request,
    const std::atomic<bool>* cancel_requested) {
    CompileResult result;
    if (cancel_requested && cancel_requested->load()) {
        result.status = CompileStatus::Cancelled;
        return result;
    }
    std::error_code ec;
    std::filesystem::create_directories(request.workspace, ec);
    result.pdf_path = request.workspace / "main.pdf";
    if (succeeds_) {
        std::ofstream pdf(result.pdf_path, std::ios::binary | std::ios::trunc);
        pdf << "%PDF-1.4\n% PaperForge mock compiler\n";
        result.status = CompileStatus::Success;
        result.log = "mock compilation succeeded";
    } else {
        result.status = CompileStatus::Failure;
        result.log = "main.tex:1: simulated error";
        result.messages.push_back(
            {"main.tex", 1, true, "simulated error"});
    }
    return result;
}

}  // namespace pf
