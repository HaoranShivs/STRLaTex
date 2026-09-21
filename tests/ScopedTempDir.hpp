#pragma once
// E-08：为测试提供唯一的临时目录。
//
// 此前测试使用固定名称，例如 /tmp/pf-e2e-workspaces。由此产生两个后果：
//   * 在 `ctest -j` 下并行运行的两个测试二进制会互相覆盖对方的目录
//     （这种偶发失败掩盖了真实结果）；
//   * 上一次运行遗留的陈旧目录可能让测试基于残留状态通过，
//     而非基于它本次新建的状态。
//
// ScopedTempDir 让每个测试实例拥有自己的目录，命名为
// "<prefix>-<pid>-<counter>-<random>"，并在析构时删除。

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

// RAII 临时目录。构造时创建目录，析构时（递归地）删除；删除操作绝不抛异常。
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