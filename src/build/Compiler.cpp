#include "build/Compiler.h"

#include <chrono>
#include <cstdlib>
#include <fstream>
#include <sstream>
#include <thread>

#ifndef _WIN32
#include <csignal>
#include <fcntl.h>
#include <sys/wait.h>
#include <unistd.h>
#endif

namespace pf {
namespace {

std::string QuoteShell(const std::string &value) {
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

bool StagePackage(const CompileRequest &request, std::string *error) {
  std::error_code ec;
  // 每次 build 都拥有独立的工作目录（方案 §25），重试的 build 绝不能
  // 继承上一次尝试的 latexmk 状态：否则 latexmk 会报告
  // "gave an error in previous invocation" 并拒绝运行，即使新暂存的源码
  // 完全正常。
  std::filesystem::remove_all(request.workspace, ec);
  std::filesystem::create_directories(request.workspace, ec);
  if (ec) {
    *error = "cannot create build workspace: " + ec.message();
    return false;
  }
  for (const auto &file : request.package.files) {
    const auto destination = request.workspace / file.path;
    std::filesystem::create_directories(destination.parent_path(), ec);
    std::ofstream output(destination, std::ios::binary | std::ios::trunc);
    if (!output) {
      *error = "cannot write generated file: " + destination.string();
      return false;
    }
    output << file.content;
  }
  for (const auto &[destination_name, source] : request.asset_sources) {
    const auto destination = request.workspace / destination_name;
    // 资源可能位于包的子目录中（assets/<file>）：
    // 复制前先创建该目录，否则 copy_file 会报 ENOENT。
    std::filesystem::create_directories(destination.parent_path(), ec);
    std::filesystem::copy_file(
        source, destination, std::filesystem::copy_options::overwrite_existing,
        ec);
    if (ec) {
      *error = "cannot stage asset " + source.string() + ": " + ec.message();
      return false;
    }
  }
  return true;
}

// ---- 诊断解析（方案 §16） ----
//
// pdfLaTeX 的日志是自由文本，因此只能按模式识别。任何解析失败的内容都会
// 原样保留在消息文本中：文档绝不会因为解析器无法分类而丢失一条诊断信息。

// "file:line:" 形式的编译器引用，例如 "main.tex:13: <message>"。
bool ParseFileLine(const std::string &line, CompilerMessage *message) {
  const size_t first = line.find(':');
  if (first == std::string::npos)
    return false;
  const size_t second = line.find(':', first + 1);
  if (second == std::string::npos)
    return false;
  try {
    const std::uint32_t number = static_cast<std::uint32_t>(
        std::stoul(line.substr(first + 1, second - first - 1)));
    message->file = line.substr(0, first);
    message->line = number;
    message->text = line.substr(second + 1);
    return true;
  } catch (...) {
    return false;
  }
}

// TeX 日志中的主要错误标记：以 "!" 开头、后跟消息。其源码行号来自紧随
// 其后的 "l.<number>" 行或最近一次 file:line 引用，二者都出现在它之后。
std::vector<CompilerMessage> ParseMessages(const std::string &log,
                                           CompileFailureKind *failure_kind) {
  std::vector<CompilerMessage> messages;
  CompileFailureKind kind = CompileFailureKind::None;
  auto note_kind = [&](CompileFailureKind candidate) {
    if (kind == CompileFailureKind::None ||
        kind == CompileFailureKind::LatexError) {
      kind = candidate;
    }
  };

  std::istringstream lines(log);
  std::string line;
  std::uint32_t last_source_line = 1;
  std::string last_file = "main.tex";
  bool saw_font_warning = false;
  bool saw_missing_package = false;
  bool saw_bibliography = false;
  while (std::getline(lines, line)) {
    // 记录最近一次 "file:line:" 引用，以便把 "!" 块归到某个源码位置。
    {
      CompilerMessage reference;
      if (ParseFileLine(line, &reference)) {
        last_file = reference.file;
        last_source_line = reference.line;
      }
    }

    if (!line.empty() && line[0] == '!') {
      CompilerMessage message;
      message.file = last_file;
      message.line = last_source_line;
      message.is_error = true;
      message.text = line.substr(line.size() > 1 ? 1 : 0);
      messages.push_back(std::move(message));
      note_kind(CompileFailureKind::LatexError);
      continue;
    }
    if (line.rfind("l.", 0) == 0) {
      // "l.13 The offending text" 携带前一条错误的行号。
      try {
        const size_t space = line.find(' ');
        const std::uint32_t number =
            static_cast<std::uint32_t>(std::stoul(line.substr(2, space - 2)));
        if (!messages.empty() && messages.back().line == 1) {
          messages.back().line = number;
        }
      } catch (...) {
      }
      continue;
    }

    CompilerMessage message;
    const bool file_line = ParseFileLine(line, &message);
    const bool warning = line.find("Warning") != std::string::npos;
    const bool overfull = line.rfind("Overfull \\hbox", 0) == 0 ||
                          line.rfind("Underfull \\hbox", 0) == 0;
    if (!file_line && !warning && !overfull &&
        line.find("!") == std::string::npos) {
      continue;
    }

    message.is_error = !warning;
    if (message.file.empty())
      message.file = last_file;
    if (message.line == 0)
      message.line = last_source_line;
    if (message.text.empty())
      message.text = line;
    messages.push_back(message);

    if (line.find("Font Warning") != std::string::npos ||
        line.find("font substitution") != std::string::npos ||
        line.find("substituted") != std::string::npos) {
      saw_font_warning = true;
    }
    if (line.find("File `") != std::string::npos &&
        line.find("' not found") != std::string::npos) {
      saw_missing_package = true;
    }
    if (line.find("Citation") != std::string::npos ||
        line.find("Bibliography") != std::string::npos) {
      saw_bibliography = true;
    }
  }
  if (saw_missing_package)
    note_kind(CompileFailureKind::PackageMissing);
  if (saw_font_warning)
    note_kind(CompileFailureKind::FontMissing);
  if (saw_bibliography)
    note_kind(CompileFailureKind::BibliographyError);
  if (failure_kind)
    *failure_kind = kind;
  return messages;
}

// 运行 `command`，最多等待到 deadline，并在取消或超时时杀掉整个进程组
// （方案 §36）：latexmk 会派生 pdflatex/bibtex 子进程，只杀父进程会让这些
// 写者残留在工作目录上。
//
// 等待期间，子进程的 stdout 与 stderr 通过两条管道抽取，并逐块转发给
// `on_output`（Build Diagnostics 方案 §44），使 Build Log 能实时显示编译
// 进度且一个字节都不丢失。两路流的完整文本都会累积给调用方：stdout 进入
// `combined`（解析器读取的日志），stderr 单独进入 `stderr_text`。
int RunCommand(
    const std::string &command, const std::chrono::seconds timeout,
    const std::atomic<bool> *cancel_requested, bool *cancelled,
    std::string *combined, std::string *stderr_text,
    const std::function<void(const CompileOutputChunk &)> &on_output) {
#ifdef _WIN32
  (void)cancel_requested;
  (void)timeout;
  (void)combined;
  (void)stderr_text;
  (void)on_output;
  return std::system(command.c_str());
#else
  int out_pipe[2] = {-1, -1};
  int err_pipe[2] = {-1, -1};
  if (pipe(out_pipe) != 0)
    return -1;
  if (pipe(err_pipe) != 0) {
    close(out_pipe[0]);
    close(out_pipe[1]);
    return -1;
  }

  const pid_t child = fork();
  if (child == 0) {
    setpgid(0, 0); // 独立进程组，这样 kill 能波及子进程
    dup2(out_pipe[1], STDOUT_FILENO);
    dup2(err_pipe[1], STDERR_FILENO);
    close(out_pipe[0]);
    close(out_pipe[1]);
    close(err_pipe[0]);
    close(err_pipe[1]);
    execl("/bin/sh", "sh", "-c", command.c_str(), static_cast<char *>(nullptr));
    _exit(127);
  }
  close(out_pipe[1]);
  close(err_pipe[1]);
  if (child < 0) {
    close(out_pipe[0]);
    close(err_pipe[0]);
    return -1;
  }

  // 非阻塞读取让等待循环对两路流都保持公平。
  fcntl(out_pipe[0], F_SETFL, fcntl(out_pipe[0], F_GETFL) | O_NONBLOCK);
  fcntl(err_pipe[0], F_SETFL, fcntl(err_pipe[0], F_GETFL) | O_NONBLOCK);

  const auto deadline = std::chrono::steady_clock::now() + timeout;
  int status = 0;
  bool reaped = false;
  char buffer[8192];
  auto drain = [&](bool stop_reading) {
    for (int i = 0; i < 2; ++i) {
      const bool is_err = i == 1;
      const int fd = is_err ? err_pipe[0] : out_pipe[0];
      if (fd < 0)
        continue;
      ssize_t got = 0;
      while ((got = ::read(fd, buffer, sizeof(buffer))) > 0) {
        const std::string chunk(buffer, static_cast<size_t>(got));
        if (is_err) {
          if (stderr_text)
            *stderr_text += chunk;
        } else if (combined) {
          *combined += chunk;
        }
        if (on_output)
          on_output(CompileOutputChunk{is_err, chunk});
      }
      if (stop_reading && got == 0) {
        close(fd);
        if (is_err)
          err_pipe[0] = -1;
        else
          out_pipe[0] = -1;
      }
    }
  };

  while (true) {
    const pid_t gone = waitpid(child, &status, WNOHANG);
    if (gone == child)
      reaped = true;
    drain(/*stop_reading=*/reaped);
    if (reaped && out_pipe[0] < 0 && err_pipe[0] < 0)
      break;
    if (!reaped && ((cancel_requested && cancel_requested->load()) ||
                    std::chrono::steady_clock::now() >= deadline)) {
      *cancelled = true;
      // 先向整个进程组发 SIGTERM，若仍存活再发 SIGKILL。
      kill(-child, SIGTERM);
      for (int grace = 0; grace < 20 && !reaped; ++grace) {
        if (waitpid(child, &status, WNOHANG) == child)
          reaped = true;
        drain(/*stop_reading=*/false);
        std::this_thread::sleep_for(std::chrono::milliseconds(50));
      }
      if (!reaped) {
        kill(-child, SIGKILL);
        waitpid(child, &status, 0);
        reaped = true;
      }
      drain(/*stop_reading=*/true);
      // 把剩余内容抽干后再关闭。
      while (out_pipe[0] >= 0 || err_pipe[0] >= 0) {
        const size_t before = (combined ? combined->size() : 0) +
                              (stderr_text ? stderr_text->size() : 0);
        drain(/*stop_reading=*/true);
        const size_t after = (combined ? combined->size() : 0) +
                             (stderr_text ? stderr_text->size() : 0);
        if (before == after)
          break;
      }
      break;
    }
    if (!reaped)
      std::this_thread::sleep_for(std::chrono::milliseconds(20));
  }
  if (out_pipe[0] >= 0)
    close(out_pipe[0]);
  if (err_pipe[0] >= 0)
    close(err_pipe[0]);
  return WIFEXITED(status) ? WEXITSTATUS(status) : -1;
#endif
}

} // namespace

const char *ToString(LatexEngine engine) {
  switch (engine) {
  case LatexEngine::PdfLatex:
    return "pdflatex";
  case LatexEngine::XeLatex:
    return "xelatex";
  case LatexEngine::LuaLatex:
    return "lualatex";
  }
  return "?";
}

const char *ToString(BibliographyEngine engine) {
  switch (engine) {
  case BibliographyEngine::None:
    return "none";
  case BibliographyEngine::BibTex:
    return "bibtex";
  case BibliographyEngine::Biber:
    return "biber";
  }
  return "?";
}

const char *ToString(CompileFailureKind kind) {
  switch (kind) {
  case CompileFailureKind::None:
    return "None";
  case CompileFailureKind::RuntimeMissing:
    return "RuntimeMissing";
  case CompileFailureKind::RuntimeCorrupted:
    return "RuntimeCorrupted";
  case CompileFailureKind::PackageMissing:
    return "PackageMissing";
  case CompileFailureKind::FontMissing:
    return "FontMissing";
  case CompileFailureKind::LatexError:
    return "LatexError";
  case CompileFailureKind::BibliographyError:
    return "BibliographyError";
  case CompileFailureKind::Timeout:
    return "Timeout";
  case CompileFailureKind::Cancelled:
    return "Cancelled";
  case CompileFailureKind::InternalError:
    return "InternalError";
  }
  return "?";
}

// ---- TectonicCompiler（保留，但已不再是生产路径，§26） ----

TectonicCompiler::TectonicCompiler(std::string executable,
                                   std::string cache_dir)
    : executable_(std::move(executable)), cache_dir_(std::move(cache_dir)) {
  std::error_code ec;
  if (!executable_.empty() && std::filesystem::exists(executable_, ec)) {
    executable_ = std::filesystem::absolute(executable_, ec).string();
  }
  (void)cache_dir_; // bundle 缓存解析保持原样；见下文
  cache_dir_ = std::move(cache_dir);
  std::error_code ec2;
  if (!cache_dir_.empty() && std::filesystem::exists(cache_dir_, ec2)) {
    cache_dir_ = std::filesystem::absolute(cache_dir_, ec2).string();
  } else {
    cache_dir_.clear();
  }
}

CompileResult
TectonicCompiler::Compile(const CompileRequest &request,
                          const std::atomic<bool> *cancel_requested) {
  CompileResult result;
  if (cancel_requested && cancel_requested->load()) {
    result.status = CompileStatus::Cancelled;
    result.failure_kind = CompileFailureKind::Cancelled;
    return result;
  }
  if (executable_.empty()) {
    result.log = "tectonic executable was not configured";
    result.failure_kind = CompileFailureKind::RuntimeMissing;
    result.messages.push_back({"main.tex", 1, true, result.log});
    return result;
  }
  std::string stage_error;
  if (!StagePackage(request, &stage_error)) {
    result.log = stage_error;
    result.failure_kind = CompileFailureKind::InternalError;
    result.messages.push_back({"main.tex", 1, true, stage_error});
    return result;
  }

  const auto log_path = request.workspace / "build.log";
  // 当本次 build 配置了 bundle 缓存时将其固定，以保证 build 可离线复现。
  std::string env_prefix;
  if (!cache_dir_.empty()) {
    env_prefix = "TECTONIC_CACHE_DIR=" + QuoteShell(cache_dir_) +
                 " HOME=" + QuoteShell(cache_dir_) + " ";
  }
  const std::string command = "cd " + QuoteShell(request.workspace.string()) +
                              " && " + env_prefix + QuoteShell(executable_) +
                              " -X compile --keep-logs --outdir " +
                              QuoteShell(request.workspace.string()) + " " +
                              QuoteShell(request.package.entry_file);
  bool cancelled = false;
  std::string stdout_text;
  std::string stderr_text;
  const int exit_code =
      RunCommand(command, std::chrono::minutes{2}, cancel_requested, &cancelled,
                 &stdout_text, &stderr_text, request.on_output);
  result.exit_code = exit_code;
  // 磁盘上的日志作为产物保留；解析器读取的是内存中的文本（方案 §44/§45）。
  {
    std::ofstream log_file(log_path, std::ios::binary | std::ios::trunc);
    log_file << stdout_text << stderr_text;
  }
  result.log = stdout_text + stderr_text;
  CompileFailureKind kind = CompileFailureKind::None;
  result.messages = ParseMessages(result.log, &kind);
  result.auxiliary_logs.push_back(log_path);
  result.pdf_path =
      request.workspace /
      (std::filesystem::path(request.package.entry_file).stem().string() +
       ".pdf");
  if (cancelled || (cancel_requested && cancel_requested->load())) {
    result.status = CompileStatus::Cancelled;
    result.failure_kind = CompileFailureKind::Cancelled;
  } else if (exit_code == 0 && std::filesystem::exists(result.pdf_path)) {
    result.status = CompileStatus::Success;
  } else {
    result.status = CompileStatus::Failure;
    result.failure_kind = kind == CompileFailureKind::None
                              ? CompileFailureKind::LatexError
                              : kind;
    if (result.messages.empty()) {
      result.messages.push_back(
          {"main.tex", 1, true, "Tectonic compilation failed"});
    }
  }
  return result;
}

// ---- TexLiveCompiler（方案 §10、§30） ----

TexLiveCompiler::TexLiveCompiler(CompilerConfig config)
    : config_(std::move(config)) {}

CompileResult
TexLiveCompiler::Compile(const CompileRequest &request,
                         const std::atomic<bool> *cancel_requested) {
  CompileResult result;
  if (cancel_requested && cancel_requested->load()) {
    result.status = CompileStatus::Cancelled;
    result.failure_kind = CompileFailureKind::Cancelled;
    return result;
  }

  // 内置运行时是唯一受支持的 TeX 环境（方案 §3、§12）：
  // 刻意不查询用户的 PATH、系统 TeX Live 与 MiKTeX。可执行文件位于
  // bin/<platform>/。
  std::error_code ec;
  std::filesystem::path bin_dir;
  if (!config_.texlive_root.empty()) {
    for (const auto &entry : std::filesystem::directory_iterator(
             config_.texlive_root / "bin", ec)) {
      if (entry.is_directory()) {
        bin_dir = entry.path();
        break;
      }
    }
  }
  if (ec || bin_dir.empty() ||
      !std::filesystem::exists(bin_dir / "latexmk", ec)) {
    result.log = "STRLaTex TeX runtime is incomplete or corrupted: " +
                 (config_.texlive_root / "bin").string() +
                 "/<platform>/latexmk is missing";
    result.failure_kind = CompileFailureKind::RuntimeMissing;
    result.messages.push_back({"runtime", 0, true, result.log});
    return result;
  }
  const std::string bin_dir_str = bin_dir.string();
  const std::string latexmk = (bin_dir / "latexmk").string();

  std::string stage_error;
  if (!StagePackage(request, &stage_error)) {
    result.log = stage_error;
    result.failure_kind = CompileFailureKind::InternalError;
    result.messages.push_back({"main.tex", 1, true, stage_error});
    return result;
  }

  // 整条链路由 latexmk 驱动（方案 §11）：engine 分支用于选择编译器，
  // 其余参数对所有引擎都相同。
  const char *engine_flag = "-pdf";
  switch (request.toolchain.engine) {
  case LatexEngine::PdfLatex:
    engine_flag = "-pdf";
    break;
  case LatexEngine::XeLatex:
    engine_flag = "-xelatex";
    break;
  case LatexEngine::LuaLatex:
    engine_flag = "-lualatex";
    break;
  }

  const auto log_path = request.workspace / "latexmk.log";
  // 环境隔离（方案 §12）：PATH 上只有内置 bin 目录，
  // TeX 缓存位于运行时目录下，外部 shell 环境不得影响结果。
  const std::string command =
      "cd " + QuoteShell(request.workspace.string()) + " && " +
      "PATH=" + QuoteShell(bin_dir_str + ":/usr/bin:/bin") + " " +
      "HOME=" + QuoteShell(config_.texlive_root.string()) + " " + "TEXMFHOME=" +
      QuoteShell((config_.texlive_root / "texmf-home").string()) + " " +
      "TEXMFVAR=" + QuoteShell((config_.texlive_root / "texmf-var").string()) +
      " " + "TEXMFCACHE=" +
      QuoteShell((config_.texlive_root / "texmf-cache").string()) + " " +
      QuoteShell(latexmk) + " " + engine_flag +
      " -interaction=nonstopmode -file-line-error -halt-on-error " +
      QuoteShell(request.package.entry_file);
  bool cancelled = false;
  std::string stdout_text;
  std::string stderr_text;
  const int exit_code =
      RunCommand(command, std::chrono::seconds{120}, cancel_requested,
                 &cancelled, &stdout_text, &stderr_text, request.on_output);
  result.exit_code = exit_code;
  // 保留磁盘上的 latexmk.log 产物（方案 §15/§45）：内容即子进程写入其
  // 终端流的内容，通过管道收集。
  {
    std::ofstream log_file(log_path, std::ios::binary | std::ios::trunc);
    log_file << stdout_text << stderr_text;
  }
  result.log = stdout_text + stderr_text;
  CompileFailureKind kind = CompileFailureKind::None;
  result.messages = ParseMessages(result.log, &kind);
  result.auxiliary_logs.push_back(log_path);
  // main.log 是编译器自身的日志；与 latexmk.log 放在一起（§15）。
  const auto main_log = request.workspace / "main.log";
  if (std::filesystem::exists(main_log, ec)) {
    result.auxiliary_logs.push_back(main_log);
  }
  result.pdf_path =
      request.workspace /
      (std::filesystem::path(request.package.entry_file).stem().string() +
       ".pdf");

  if (cancelled || (cancel_requested && cancel_requested->load())) {
    result.status = CompileStatus::Cancelled;
    result.failure_kind = CompileFailureKind::Cancelled;
  } else if (exit_code == 0 && std::filesystem::exists(result.pdf_path)) {
    result.status = CompileStatus::Success;
  } else {
    result.status = CompileStatus::Failure;
    result.failure_kind = kind == CompileFailureKind::None
                              ? CompileFailureKind::LatexError
                              : kind;
    if (result.messages.empty()) {
      result.messages.push_back(
          {"main.tex", 1, true, "LaTeX compilation failed"});
    }
  }
  return result;
}

// ---- MockCompiler ----

CompileResult MockCompiler::Compile(const CompileRequest &request,
                                    const std::atomic<bool> *cancel_requested) {
  CompileResult result;
  if (cancel_requested && cancel_requested->load()) {
    result.status = CompileStatus::Cancelled;
    result.failure_kind = CompileFailureKind::Cancelled;
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
    result.exit_code = 0;
  } else {
    result.status = CompileStatus::Failure;
    result.failure_kind = CompileFailureKind::LatexError;
    result.log = "main.tex:1: simulated error";
    result.exit_code = 1;
    result.messages.push_back({"main.tex", 1, true, "simulated error"});
  }
  // 在输出契约上，mock 与真实编译器的行为一致：若提供了输出端，
  // 日志会送达该输出端（方案 §44）。
  if (request.on_output && !result.log.empty()) {
    request.on_output(CompileOutputChunk{false, result.log + "\n"});
  }
  return result;
}

} // namespace pf
