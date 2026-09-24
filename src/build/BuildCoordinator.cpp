#include "build/BuildCoordinator.h"

#include <cstdio>
#include <exception>

#include "build/DiagnosticMapper.h"
#include "core/IdGenerator.h"
#include "render/LatexRenderer.h"
#include "validation/Validator.h"
#include <filesystem>
#include <fstream>

namespace pf {
namespace {

std::vector<std::string> ExtractBibliographyKeys(const std::string& bibtex) {
    std::vector<std::string> keys;
    size_t cursor = 0;
    while ((cursor = bibtex.find('@', cursor)) != std::string::npos) {
        const size_t open = bibtex.find_first_of("({", cursor + 1);
        if (open == std::string::npos)
            break;
        const size_t comma = bibtex.find(',', open + 1);
        if (comma == std::string::npos)
            break;
        std::string key = bibtex.substr(open + 1, comma - open - 1);
        const size_t first = key.find_first_not_of(" \t\r\n");
        const size_t last = key.find_last_not_of(" \t\r\n");
        if (first != std::string::npos) {
            keys.push_back(key.substr(first, last - first + 1));
        }
        cursor = comma + 1;
    }
    return keys;
}

} // namespace

BuildCoordinator::BuildCoordinator(Host host, ICompiler* compiler) : host_(std::move(host)), compiler_(compiler) {
    // 必须在所有成员初始化完成后再启动 worker：循环会立即读取
    // stopping_/phase_。
    worker_ = std::thread([this] { WorkerLoop(); });
}

BuildCoordinator::BuildCoordinator(Host host,
                                   std::function<std::unique_ptr<ICompiler>(const BuildToolchain&)> compiler_provider)
    : host_(std::move(host)), compiler_provider_(std::move(compiler_provider)) {
    worker_ = std::thread([this] { WorkerLoop(); });
}

BuildCoordinator::~BuildCoordinator() {
    {
        std::lock_guard lock(mutex_);
        stopping_ = true;
        pending_.reset();
        cancel_requested_.store(true);
    }
    condition_.notify_all();
    if (worker_.joinable())
        worker_.join();
}

void BuildCoordinator::set_debounce(std::chrono::milliseconds debounce) {
    std::lock_guard lock(mutex_);
    debounce_ = debounce;
}

BuildPhase BuildCoordinator::phase() const {
    std::lock_guard lock(mutex_);
    return phase_;
}

void BuildCoordinator::SetPhase(BuildPhase next) {
    BuildPhase previous;
    {
        std::lock_guard lock(mutex_);
        if (phase_ == next)
            return;
        previous = phase_;
        phase_ = next;
    }
    if (host_.on_phase_changed)
        host_.on_phase_changed(previous, next);
}

void BuildCoordinator::RequestBuild(BuildSnapshot snapshot, bool manual) {
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(snapshot);
        due_ = std::chrono::steady_clock::now() + (manual ? std::chrono::milliseconds{0} : debounce_);
        ++request_generation_;
    }
    SetPhase(manual ? BuildPhase::Rendering : BuildPhase::Debouncing);
    condition_.notify_all();
}

void BuildCoordinator::Cancel() {
    {
        std::lock_guard lock(mutex_);
        pending_.reset();
        ++request_generation_;
        cancel_requested_.store(true);
    }
    condition_.notify_all();
    SetPhase(BuildPhase::Idle);
}

void BuildCoordinator::WorkerLoop() {
    while (true) {
        BuildSnapshot snapshot;
        std::uint64_t generation = 0;
        {
            std::unique_lock lock(mutex_);
            condition_.wait(lock, [&] { return stopping_ || pending_; });
            if (stopping_)
                return;
            generation = request_generation_;
            const auto due = due_;
            condition_.wait_until(lock, due, [&] { return stopping_ || request_generation_ != generation; });
            if (stopping_)
                return;
            if (request_generation_ != generation)
                continue;
            snapshot = std::move(*pending_);
            pending_.reset();
            cancel_requested_.store(false);
        }

        SetPhase(BuildPhase::Rendering);
        BuildResult result = BuildOne(snapshot);
        bool superseded = false;
        {
            std::lock_guard lock(mutex_);
            superseded = request_generation_ != generation;
        }
        if (!superseded && host_.on_build_finished) {
            host_.on_build_finished(result);
        }
        {
            std::lock_guard lock(mutex_);
            superseded = pending_.has_value();
        }
        if (!superseded)
            SetPhase(BuildPhase::Idle);
    }
}

void BuildCoordinator::EmitEvent(const BuildId& build_id, BuildEventType type, std::string message) const {
    if (!host_.on_build_event)
        return;
    BuildEvent event;
    event.build_id = build_id;
    event.timestamp_ms = BuildEventNowMs();
    event.type = type;
    event.message = std::move(message);
    host_.on_build_event(event);
}

BuildResult BuildCoordinator::BuildOne(const BuildSnapshot& snapshot) {
    BuildResult result;
    result.project_id = snapshot.project_id;
    result.snapshot_id = snapshot.snapshot_id;
    // 每次尝试都携带 build id，即使在 rendering 之前就未通过校验的尝试也是
    // 如此，因此结果始终可追溯。
    result.build_id = snapshot.build_id.empty() ? BuildId(IdGenerator::NewBuildId()) : snapshot.build_id;
    result.revision = snapshot.revision;
    result.started_ms = BuildEventNowMs();
    const BuildId& id = result.build_id;

    EmitEvent(id, BuildEventType::BuildStarted, "Build started (snapshot " + snapshot.snapshot_id + ")");
    EmitEvent(id, BuildEventType::GenerationStarted, "Generating LaTeX");

    ValidationInput validation_input;
    validation_input.snapshot_id = snapshot.snapshot_id;
    validation_input.revision = snapshot.revision;
    validation_input.document = snapshot.document.get();
    validation_input.template_id = snapshot.template_id;
    validation_input.has_bibliography = !snapshot.bibliography_bibtex.empty();
    validation_input.bibliography_keys = ExtractBibliographyKeys(snapshot.bibliography_bibtex);
    for (const auto& [destination, source] : snapshot.asset_sources) {
        (void)source;
        validation_input.asset_paths.push_back(destination);
    }
    auto validation = Validator().Validate(validation_input);
    result.diagnostics = validation.diagnostics;
    if (!validation.can_render) {
        result.outcome = BuildResult::Outcome::Failure;
        result.exit_code = -1;
        result.finished_ms = BuildEventNowMs();
        EmitEvent(id, BuildEventType::BuildFailed, "Build failed: validation blocked rendering");
        return result;
    }

    RenderRequest render_request;
    render_request.build_id = result.build_id.value();
    render_request.snapshot_id = snapshot.snapshot_id;
    render_request.revision = snapshot.revision;
    render_request.document = snapshot.document.get();
    render_request.template_id = snapshot.template_id;
    render_request.asset_files = snapshot.asset_files;
    render_request.bibliography_bibtex = snapshot.bibliography_bibtex;
    auto rendered = LatexRenderer().Render(render_request);
    result.diagnostics.insert(result.diagnostics.end(), rendered.diagnostics.begin(), rendered.diagnostics.end());
    if (rendered.status != RenderResult::Status::Ok) {
        result.outcome = BuildResult::Outcome::Failure;
        result.exit_code = -1;
        result.finished_ms = BuildEventNowMs();
        EmitEvent(id, BuildEventType::BuildFailed, "Build failed: LaTeX generation failed");
        return result;
    }
    EmitEvent(id, BuildEventType::GenerationFinished, "Generated " + rendered.package.entry_file);

    // TEMP-DEBUG：将生成的 LaTeX 转储到项目旁边，便于用户在 tectonic 编译
    // 之前检查 renderer 产出了什么。写入 <project>/.paperforge/build/main.tex，
    // 诊断结束后移除。
    // host 钩子是可选的：测试构建 coordinator 时不会设置它。
    if (host_.debug_dump_dir) {
        std::error_code dump_ec;
        for (const auto& file : rendered.package.files) {
            const auto dump_path = host_.debug_dump_dir().value_or("").empty()
                                       ? std::filesystem::path{}
                                       : std::filesystem::path(*host_.debug_dump_dir()) / file.path;
            if (dump_path.empty())
                break;
            std::filesystem::create_directories(dump_path.parent_path(), dump_ec);
            std::ofstream dump(dump_path, std::ios::binary | std::ios::trunc);
            if (dump) {
                dump << file.content;
            }
        }
        // 同时将引用的 assets 放置到转储的 main.tex 旁边，使转储源码中的
        // \includegraphics 路径能解析到真实文件（assets/<name>.<ext>），
        // 而不是悬空的名称。
        const auto dump_root = host_.debug_dump_dir().value_or("");
        if (!dump_root.empty()) {
            for (const auto& [destination_name, source] : snapshot.asset_sources) {
                const auto dump_asset = std::filesystem::path(dump_root) / destination_name;
                std::filesystem::create_directories(dump_asset.parent_path(), dump_ec);
                std::filesystem::copy_file(source, dump_asset, std::filesystem::copy_options::overwrite_existing,
                                           dump_ec);
            }
        }
    }

    SetPhase(BuildPhase::Compiling);
    CompileRequest compile_request;
    compile_request.package = std::move(rendered.package);
    compile_request.workspace =
        std::filesystem::path(host_.workspace_root()) / snapshot.project_id.value() / snapshot.snapshot_id;
    compile_request.asset_sources = snapshot.asset_sources;
    compile_request.toolchain = snapshot.toolchain;
    // 将编译器的实时输出以结构化事件流式发出（方案 §44）：进程运行期间
    // Build Log 持续增长。回调在此（worker）线程上触发并携带当前 build id，
    // 因此若有更新的 build 已启动，应用侧可以丢弃它们。
    compile_request.on_output = [this, id](const CompileOutputChunk& chunk) {
        EmitEvent(id, chunk.is_stderr ? BuildEventType::StdErr : BuildEventType::StdOut, chunk.text);
    };
    // compiler 依据模板的 toolchain 需求选定（方案 §13）；为测试起见，
    // 仍然支持单次请求的 compiler。
    const std::unique_ptr<ICompiler> selected = compiler_provider_ ? compiler_provider_(snapshot.toolchain) : nullptr;
    ICompiler* const used = selected ? selected.get() : (compiler_ ? compiler_ : nullptr);
    if (!used) {
        result.outcome = BuildResult::Outcome::Failure;
        result.failure_kind = CompileFailureKind::RuntimeMissing;
        result.exit_code = -1;
        result.finished_ms = BuildEventNowMs();
        Diagnostic diagnostic;
        diagnostic.build_id = id;
        diagnostic.source = DiagnosticSource::Compiler;
        diagnostic.severity = DiagnosticSeverity::Error;
        diagnostic.code = "BUILD_PROCESS_START_FAILED";
        diagnostic.message = "Failed to start LaTeX compiler: STRLaTex TeX runtime is incomplete "
                             "or corrupted";
        diagnostic.revision = snapshot.revision;
        result.diagnostics.push_back(std::move(diagnostic));
        result.log = "STRLaTex TeX runtime is incomplete or corrupted";
        EmitEvent(id, BuildEventType::InternalMessage, "Compiler unavailable: " + result.log);
        EmitEvent(id, BuildEventType::BuildFailed, "Build failed");
        return result;
    }
    EmitEvent(id, BuildEventType::ProcessStarted,
              "Running compiler (" + std::string(ToString(snapshot.toolchain.engine)) + " via latexmk)");
    auto compiled = used->Compile(compile_request, &cancel_requested_);
    result.pdf_path = compiled.pdf_path;
    result.log = compiled.log;
    result.failure_kind = compiled.failure_kind;
    result.exit_code = compiled.exit_code;

    // TEMP-DEBUG：将编译出的 PDF 复制到项目旁边，用于与预览窗口显示的内容
    // 进行比对。
    if (compiled.status == CompileStatus::Success && host_.debug_dump_dir) {
        const auto dump_dir = host_.debug_dump_dir().value_or("");
        if (!dump_dir.empty()) {
            std::error_code copy_ec;
            std::filesystem::copy_file(compiled.pdf_path, std::filesystem::path(dump_dir) / "main.pdf",
                                       std::filesystem::copy_options::overwrite_existing, copy_ec);
        }
    }
    std::vector<Diagnostic> compiler_diagnostics;
    // Parser 健壮性（方案 §46）：分类是尽力而为的。parser 故障只记一条内部
    // 日志消息——它可能使 Problems 列表丢失其 compiler 条目，但绝不能使
    // 原本良好的 build 失败，也不能使流水线崩溃；无论如何，BuildResult::log
    // 中的原始日志始终完整。
    try {
        compiler_diagnostics = DiagnosticMapper().Map(compiled, rendered.source_map, snapshot.revision, id);
    } catch (const std::exception& e) {
        EmitEvent(id, BuildEventType::InternalMessage, std::string("Diagnostic parser failure: ") + e.what());
    } catch (...) {
        EmitEvent(id, BuildEventType::InternalMessage, "Diagnostic parser failure (unknown exception)");
    }
    // 合并顺序（Build Diagnostics 方案 §18）：validator、generator、compiler。
    result.diagnostics.insert(result.diagnostics.end(), compiler_diagnostics.begin(), compiler_diagnostics.end());
    result.finished_ms = BuildEventNowMs();

    EmitEvent(id, BuildEventType::ProcessFinished, "Process exited with code " + std::to_string(compiled.exit_code));
    const double seconds = static_cast<double>(result.finished_ms - result.started_ms) / 1000.0;
    char duration[32];
    std::snprintf(duration, sizeof(duration), " (%.3f s)", seconds);
    switch (compiled.status) {
    case CompileStatus::Success:
        result.outcome = BuildResult::Outcome::Success;
        EmitEvent(id, BuildEventType::BuildSucceeded, std::string("Build succeeded") + duration);
        break;
    case CompileStatus::Failure:
        result.outcome = BuildResult::Outcome::Failure;
        EmitEvent(id, BuildEventType::BuildFailed,
                  std::string("Build failed: ") + ToString(compiled.failure_kind) + duration);
        break;
    case CompileStatus::Cancelled:
        result.outcome = BuildResult::Outcome::Cancelled;
        EmitEvent(id, BuildEventType::BuildCancelled, std::string("Build cancelled") + duration);
        break;
    }
    return result;
}

} // namespace pf
