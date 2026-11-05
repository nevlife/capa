#ifndef CAPA_RESULT_HPP
#define CAPA_RESULT_HPP

#include <cstdint>
#include <optional>
#include <stdexcept>
#include <string>
#include <utility>
#include <variant>

namespace capa
{

enum class ErrorKind : std::uint8_t {
  InvalidArgument,  // the caller's input is wrong
  Failed,           // a chain file, a preflight or a read-back refused it
  System,           // the kernel refused it; Error::code is the errno
};

struct Error
{
  ErrorKind kind = ErrorKind::Failed;
  std::string what;
  int code = 0;
};

// A value or an Error. Reading the value of a failed Result is undefined: check it first.
template <class T>
class [[nodiscard]] Result
{
public:
  // Implicit, so a function returns a value or an Error as it is.
  Result(const T & value)
  : v_(std::in_place_index<0>, value) {}  // NOLINT(google-explicit-constructor)
  Result(T && value)
  : v_(std::in_place_index<0>, std::move(value)) {}  // NOLINT(google-explicit-constructor)
  Result(Error error)
  : v_(std::in_place_index<1>, std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] auto ok() const noexcept -> bool { return v_.index() == 0; }
  explicit operator bool() const noexcept { return ok(); }

  auto operator*() & noexcept -> T & { return *std::get_if<0>(&v_); }
  auto operator*() const & noexcept -> const T & { return *std::get_if<0>(&v_); }
  auto operator*() && noexcept -> T && { return std::move(*std::get_if<0>(&v_)); }
  auto operator->() noexcept -> T * { return std::get_if<0>(&v_); }
  auto operator->() const noexcept -> const T * { return std::get_if<0>(&v_); }

  [[nodiscard]] auto error() const noexcept -> const Error & { return *std::get_if<1>(&v_); }

private:
  std::variant<T, Error> v_;
};

template <>
class [[nodiscard]] Result<void>
{
public:
  Result() = default;
  Result(Error error) : error_(std::move(error)) {}  // NOLINT(google-explicit-constructor)

  [[nodiscard]] auto ok() const noexcept -> bool { return !error_.has_value(); }
  explicit operator bool() const noexcept { return ok(); }

  [[nodiscard]] auto error() const noexcept -> const Error & { return *error_; }

private:
  std::optional<Error> error_;
};

namespace detail
{

// A kernel refusal as an exception. Error::what already names the errno, which
// std::system_error would append a second time.
class SystemFailure : public std::runtime_error
{
public:
  explicit SystemFailure(const Error & e) : std::runtime_error(e.what), code_(e.code) {}

  [[nodiscard]] auto code() const noexcept -> int { return code_; }

private:
  int code_;
};

// For the callers whose own contract is an exception and nothing else: nanobind, the cxx bridge,
// flux's thread-start and node-added hooks, a component's constructor. The only throw of capa.
[[noreturn]] inline void raise(const Error & e)
{
  switch (e.kind) {
    case ErrorKind::InvalidArgument:
      throw std::invalid_argument(e.what);
    case ErrorKind::System:
      throw SystemFailure(e);
    case ErrorKind::Failed:
      break;
  }
  throw std::runtime_error(e.what);
}

template <class T>
auto unwrap(Result<T> r) -> T
{
  if (!r) {
    raise(r.error());
  }
  return *std::move(r);
}

inline void unwrap(const Result<void> & r)
{
  if (!r) {
    raise(r.error());
  }
}

}  // namespace detail

}  // namespace capa

#endif  // CAPA_RESULT_HPP
