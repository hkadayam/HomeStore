#pragma once

#include <string>

#include <fmt/format.h>
#include <folly/Expected.h>

#include "common/defs.h"

namespace homedb {

// Every HomeDB API returns Result<T> = folly::Expected<T, HomeDbError>.  Errors are constructed at the callsite
// with a message; the `kind` classifies them so callers can react without string-matching.
enum class ErrorKind : uint16_t {
    Ok = 0,
    NotFound,          // Key/table not found on read.
    AlreadyExists,     // Duplicate table name on create, INSERT-only conflict, etc.
    InvalidArgument,   // Malformed key/value bytes against a TableSpec, empty table name, etc.
    InvalidOperation,  // Snapshot ops on non-mvcc table, unsupported feature (e.g. sharded in v1).
    Io,                // Underlying HomeStore/device error surfaced up.
    NotYetImplemented, // Reserved slots for future phases.
};

class HomeDbError {
public:
    HomeDbError() = default;
    HomeDbError(ErrorKind k, std::string msg) : kind_{k}, msg_{std::move(msg)} {}

    ErrorKind kind() const { return kind_; }
    std::string const& message() const { return msg_; }
    std::string to_string() const { return fmt::format("[{}] {}", to_u16(kind_), msg_); }

private:
    ErrorKind kind_{ErrorKind::Ok};
    std::string msg_;
};

template < typename T >
using Result = folly::Expected< T, HomeDbError >;

// Convenience constructors.  Return by value so callers write `return err_not_found("foo")`.
inline HomeDbError make_error(ErrorKind k, std::string msg) {
    return HomeDbError{k, std::move(msg)};
}
inline folly::Unexpected< HomeDbError > err_not_found(std::string m) {
    return folly::makeUnexpected(HomeDbError{ErrorKind::NotFound, std::move(m)});
}
inline folly::Unexpected< HomeDbError > err_already_exists(std::string m) {
    return folly::makeUnexpected(HomeDbError{ErrorKind::AlreadyExists, std::move(m)});
}
inline folly::Unexpected< HomeDbError > err_invalid_argument(std::string m) {
    return folly::makeUnexpected(HomeDbError{ErrorKind::InvalidArgument, std::move(m)});
}
inline folly::Unexpected< HomeDbError > err_invalid_op(std::string m) {
    return folly::makeUnexpected(HomeDbError{ErrorKind::InvalidOperation, std::move(m)});
}
inline folly::Unexpected< HomeDbError > err_io(std::string m) {
    return folly::makeUnexpected(HomeDbError{ErrorKind::Io, std::move(m)});
}
inline folly::Unexpected< HomeDbError > err_not_impl(std::string m) {
    return folly::makeUnexpected(HomeDbError{ErrorKind::NotYetImplemented, std::move(m)});
}

} // namespace homedb