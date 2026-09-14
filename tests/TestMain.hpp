// Minimal test framework (no external deps).
#include <algorithm>
#include <chrono>
#include <cstdlib>
#include <functional>
#include <iostream>
#include <string>
#include <vector>

namespace testfw {

struct TestCase {
    std::string name;
    std::function<void()> fn;
};

inline std::vector<TestCase>& Registry() {
    static std::vector<TestCase> registry;
    return registry;
}

inline int& Failures() {
    static int failures = 0;
    return failures;
}

inline bool& CurrentFailed() {
    static bool failed = false;
    return failed;
}

struct Registrar {
    Registrar(std::string name, std::function<void()> fn) {
        Registry().push_back({std::move(name), std::move(fn)});
    }
};

inline int RunAll() {
    int ran = 0;
    for (const auto& tc : Registry()) {
        CurrentFailed() = false;
        auto start = std::chrono::steady_clock::now();
        try {
            tc.fn();
        } catch (const std::exception& e) {
            std::cout << "  EXCEPTION: " << e.what() << "\n";
            CurrentFailed() = true;
        } catch (...) {
            std::cout << "  EXCEPTION: unknown\n";
            CurrentFailed() = true;
        }
        auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(
                      std::chrono::steady_clock::now() - start)
                      .count();
        std::cout << (CurrentFailed() ? "[FAIL] " : "[ OK  ] ") << tc.name << " ("
                  << ms << " ms)\n";
        if (CurrentFailed()) ++Failures();
        ++ran;
    }
    std::cout << "\n" << ran << " tests, " << Failures() << " failures\n";
    return Failures() == 0 ? 0 : 1;
}

}  // namespace testfw

#define PF_TEST(name)                                                        \
    static void pf_test_##name();                                           \
    static ::testfw::Registrar pf_reg_##name(#name, pf_test_##name);        \
    static void pf_test_##name()

#define PF_CHECK(cond)                                                       \
    do {                                                                     \
        if (!(cond)) {                                                       \
            std::cout << "  CHECK FAILED at " << __FILE__ << ":"            \
                      << __LINE__ << ": " << #cond << "\n";                  \
            ::testfw::CurrentFailed() = true;                                \
        }                                                                    \
    } while (0)

#define PF_CHECK_EQ(a, b)                                                    \
    do {                                                                     \
        auto va_ = (a);                                                      \
        auto vb_ = (b);                                                      \
        if (!(va_ == vb_)) {                                                 \
            std::cout << "  CHECK FAILED at " << __FILE__ << ":"            \
                      << __LINE__ << ": " << #a " == " #b << "\n";           \
            ::testfw::CurrentFailed() = true;                                \
        }                                                                    \
    } while (0)

