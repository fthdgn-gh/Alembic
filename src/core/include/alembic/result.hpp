// SPDX-License-Identifier: GPL-3.0-or-later
// Errors as values. The PS5 build has no exceptions, so every fallible call
// returns a Status or a Result<T>.
#pragma once

#include <optional>
#include <string>
#include <utility>

namespace alembic {

enum class Errc {
    io,               // the OS refused a read, write, open, ...
    invalid_argument, // the caller passed something unusable
    invalid_data,     // the input file is malformed
    unsupported,      // valid input we do not handle
    too_large,        // a format limit was exceeded
    cancelled,        // the progress callback asked us to stop
};

struct Error {
    Errc code;
    std::string message;
};

class [[nodiscard]] Status {
public:
    Status() = default;
    Status(Error error) : error_(std::move(error)) {}

    bool ok() const { return !error_.has_value(); }
    explicit operator bool() const { return ok(); }
    const Error& error() const { return *error_; }
    Error take_error() { return std::move(*error_); }

private:
    std::optional<Error> error_;
};

template <class T>
class [[nodiscard]] Result {
public:
    Result(T value) : value_(std::move(value)) {}
    Result(Error error) : error_(std::move(error)) {}

    bool ok() const { return value_.has_value(); }
    explicit operator bool() const { return ok(); }
    T& value() { return *value_; }
    const T& value() const { return *value_; }
    T* operator->() { return &*value_; }
    const T* operator->() const { return &*value_; }
    T& operator*() { return *value_; }
    const T& operator*() const { return *value_; }
    const Error& error() const { return error_; }
    Error take_error() { return std::move(error_); }

private:
    std::optional<T> value_;
    Error error_{Errc::invalid_argument, {}};
};

inline Error make_error(Errc code, std::string message) {
    return Error{code, std::move(message)};
}

} // namespace alembic

#define ALEMBIC_CONCAT_INNER(a, b) a##b
#define ALEMBIC_CONCAT(a, b) ALEMBIC_CONCAT_INNER(a, b)

// Returns early from the enclosing function when `expr` (a Status) failed.
#define ALEMBIC_TRY(expr)                                                       \
    do {                                                                        \
        auto alembic_status_ = (expr);                                          \
        if (!alembic_status_.ok()) return alembic_status_.take_error();         \
    } while (0)

// `ALEMBIC_ASSIGN(auto x, f());` binds the value of a Result or returns its error.
#define ALEMBIC_ASSIGN(decl, expr) ALEMBIC_ASSIGN_IMPL(ALEMBIC_CONCAT(alembic_r_, __LINE__), decl, expr)
#define ALEMBIC_ASSIGN_IMPL(tmp, decl, expr)                                    \
    auto tmp = (expr);                                                          \
    if (!tmp.ok()) return tmp.take_error();                                     \
    decl = std::move(tmp.value())
