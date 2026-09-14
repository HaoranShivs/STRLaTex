#pragma once
// Result<T, E>: lightweight value-or-error type (C++20, no external deps).
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

// Convenience: StringError("...") converts to any error type via ToStringError.
struct StringError {
    StringError(std::string s) : value(std::move(s)) {}
    StringError(const char* s) : value(s) {}
    std::string value;
};

// Alias kept for call-site brevity: Unexpected("...") == StringError("...").
using Unexpected = StringError;

// Default: construct E from string (works for std::string).
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

// Void specialization
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
