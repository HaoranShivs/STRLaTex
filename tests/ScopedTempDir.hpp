#pragma once
// E-08: unique temporary directories for tests.
//
// Tests used fixed names such as /tmp/pf-e2e-workspaces. Two consequences:
//   * two test binaries running under `ctest -j` clobbered each other's
//     directories (a flaky failure that hid real results);
//   * a stale directory from a previous run could make a test pass on
//     leftover state instead of on what it just created.
//
// ScopedTempDir gives every test instance its own directory named
// "<prefix>-<pid>-<counter>-<random>", removed on destruction.

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <random>
#include <string>

#if defined(_WIN32)
#include <process.h>
#define PF_TEST_GETPID _getpid
#else
#include <unistd.h>
#define PF_TEST_GETPID getpid
#endif

namespace pf::test {

// RAII temporary directory. Creates the directory on construction and removes
// it (recursively) on destruction; the removal never throws.
class ScopedTempDir {
public:
    explicit ScopedTempDir(std::string_view prefix) {
        static std::atomic<std::uint64_t> counter{0};
        static std::mt19937_64 rng([] {
            std::random_device device;
            return std::mt19937_64(device());
        }());
        const std::uint64_t unique = rng();
        const std::uint64_t sequence = counter.fetch_add(1);
        std::string name = std::string(prefix) + "-" + std::to_string(PF_TEST_GETPID()) +
                           "-" + std::to_string(sequence) + "-" +
                           std::to_string(unique);
        path_ = std::filesystem::temp_directory_path() / name;
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
        std::filesystem::create_directories(path_, ec);
    }

    ~ScopedTempDir() {
        std::error_code ec;
        std::filesystem::remove_all(path_, ec);
    }

    ScopedTempDir(const ScopedTempDir&) = delete;
    ScopedTempDir& operator=(const ScopedTempDir&) = delete;

    const std::filesystem::path& path() const { return path_; }
    std::string string() const { return path_.string(); }
    operator std::filesystem::path() const { return path_; }
    std::filesystem::path operator/(std::string_view child) const {
        return path_ / std::string(child);
    }

private:
    std::filesystem::path path_;
};

}  // namespace pf::test