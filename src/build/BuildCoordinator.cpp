#include "build/BuildCoordinator.h"

#include <filesystem>
#include "build/DiagnosticMapper.h"
#include "core/IdGenerator.h"
#include "render/LatexRenderer.h"
#include "validation/Validator.h"

namespace pf {
namespace {

std::vector<std::string> ExtractBibliographyKeys(const std::string& bibtex) {
    std::vector<std::string> keys;
    size_t cursor = 0;
    while ((cursor = bibtex.find('@', cursor)) != std::string::npos) {
        const size_t open = bibtex.find_first_of("({", cursor + 1);
        if (open == std::string::npos) break;
        const size_t comma = bibtex.find(',', open + 1);
        if (comma == std::string::npos) break;
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

}  // namespace

BuildCoordinator::BuildCoordinator(Host host, ICompiler* compiler)
    : host_(std::move(host)), compiler_(compiler) {
    // Start the worker only after every member is initialized: the loop reads
    // stopping_/phase_ immediately.
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
    if (worker_.joinable()) worker_.join();
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
        if (phase_ == next) return;
        previous = phase_;
        phase_ = next;
    }
    if (host_.on_phase_changed) host_.on_phase_changed(previous, next);
}

void BuildCoordinator::RequestBuild(BuildSnapshot snapshot, bool manual) {
    {
        std::lock_guard lock(mutex_);
        pending_ = std::move(snapshot);
        due_ = std::chrono::steady_clock::now() +
               (manual ? std::chrono::milliseconds{0} : debounce_);
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
            if (stopping_) return;
            generation = request_generation_;
            const auto due = due_;
            condition_.wait_until(lock, due, [&] {
                return stopping_ || request_generation_ != generation;
            });
            if (stopping_) return;
            if (request_generation_ != generation) continue;
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
        if (!superseded) SetPhase(BuildPhase::Idle);
    }
}

BuildResult BuildCoordinator::BuildOne(const BuildSnapshot& snapshot) {
    BuildResult result;
    result.project_id = snapshot.project_id;
    result.snapshot_id = snapshot.snapshot_id;
    // Every attempt carries a build id, even one that fails validation before
    // rendering, so the result is always attributable.
    result.build_id = snapshot.build_id.empty()
                          ? BuildId(IdGenerator::NewBuildId())
                          : snapshot.build_id;
    result.revision = snapshot.revision;

    ValidationInput validation_input;
    validation_input.snapshot_id = snapshot.snapshot_id;
    validation_input.revision = snapshot.revision;
    validation_input.document = snapshot.document.get();
    validation_input.template_id = snapshot.template_id;
    validation_input.has_bibliography =
        !snapshot.bibliography_bibtex.empty();
    validation_input.bibliography_keys =
        ExtractBibliographyKeys(snapshot.bibliography_bibtex);
    for (const auto& [destination, source] : snapshot.asset_sources) {
        (void)source;
        validation_input.asset_paths.push_back(destination);
    }
    auto validation = Validator().Validate(validation_input);
    result.diagnostics = validation.diagnostics;
    if (!validation.can_render) {
        result.outcome = BuildResult::Outcome::Failure;
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
    result.diagnostics.insert(result.diagnostics.end(),
                              rendered.diagnostics.begin(),
                              rendered.diagnostics.end());
    if (rendered.status != RenderResult::Status::Ok) {
        result.outcome = BuildResult::Outcome::Failure;
        return result;
    }

    SetPhase(BuildPhase::Compiling);
    CompileRequest compile_request;
    compile_request.package = std::move(rendered.package);
    compile_request.workspace =
        std::filesystem::path(host_.workspace_root()) /
        snapshot.project_id.value() / snapshot.snapshot_id;
    compile_request.asset_sources = snapshot.asset_sources;
    auto compiled = compiler_->Compile(compile_request, &cancel_requested_);
    result.pdf_path = compiled.pdf_path;
    result.log = compiled.log;
    auto compiler_diagnostics =
        DiagnosticMapper().Map(compiled, rendered.source_map,
                               snapshot.revision);
    result.diagnostics.insert(result.diagnostics.begin(),
                              compiler_diagnostics.begin(),
                              compiler_diagnostics.end());
    switch (compiled.status) {
        case CompileStatus::Success:
            result.outcome = BuildResult::Outcome::Success;
            break;
        case CompileStatus::Failure:
            result.outcome = BuildResult::Outcome::Failure;
            break;
        case CompileStatus::Cancelled:
            result.outcome = BuildResult::Outcome::Cancelled;
            break;
    }
    return result;
}

}  // namespace pf
