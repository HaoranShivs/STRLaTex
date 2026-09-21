#pragma once
// Result<T, E>：轻量的值或错误类型（C++20，无外部依赖）。
#include <optional>
#include <string>
#include <type_traits>
#include <utility>
#include <variant>

namespace pf {

template <typename E>
struct Unexpected2 {
    explicit Unexpected2(E reason) : reason(std::move(reason)) {}
    E reason;
};

// 便捷机制：StringError("...") 通过 ToStringError 转换为任意错误类型。
struct StringError {
    StringError(std::string s) : value(std::move(s)) {}
    StringError(const char* s) : value(s) {}
    std::string value;
};

// 为调用处简洁而保留的别名：Unexpected("...") == StringError("...")。
using Unexpected = StringError;

// 默认实现：由字符串构造 E（对 std::string 适用）。
template <typename E>
E ToStringError(const std::string& value) {
    if constexpr (std::is_constructible_v<E, const std::string&>) {
        return E(value);
    } else {
        static_assert(sizeof(E) == 0,
                      "No ToStringError specialization for this error type");
        return E{};
    }
}

template <typename T, typename E = std::string>
class Result {
public:
    Result(T value) : data_(std::in_place_index<0>, std::move(value)) {}
    template <typename E2, typename = std::enable_if_t<std::is_convertible_v<E2, E>>>
    Result(Unexpected2<E2> e) : data_(std::in_place_index<1>, E(std::move(e.reason))) {}
    Result(StringError e) : data_(std::in_place_index<1>, ToStringError<E>(e.value)) {}

    bool ok() const noexcept { return data_.index() == 0; }
    explicit operator bool() const noexcept { return ok(); }

    T& value() { return std::get<0>(data_); }
    const T& value() const { return std::get<0>(data_); }
    const E& error() const { return std::get<1>(data_); }

    T value_or(T fallback) const {
        return ok() ? std::get<0>(data_) : std::move(fallback);
    }

private:
    std::variant<T, E> data_;
};

// void 特化
template <typename E>
class Result<void, E> {
public:
    Result() = default;
    template <typename E2, typename = std::enable_if_t<std::is_convertible_v<E2, E>>>
    Result(Unexpected2<E2> e) : error_(E(std::move(e.reason))) {}
    Result(StringError e) : error_(ToStringError<E>(e.value)) {}

    bool ok() const noexcept { return !error_.has_value(); }
    explicit operator bool() const noexcept { return ok(); }
    const E& error() const { return *error_; }

private:
    std::optional<E> error_;
};

}  // namespace pf
