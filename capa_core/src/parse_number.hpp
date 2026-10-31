#ifndef CAPA_PARSE_NUMBER_HPP
#define CAPA_PARSE_NUMBER_HPP

#include <charconv>
#include <optional>
#include <string_view>
#include <system_error>

namespace capa::detail
{

// A whole decimal number, or nothing. atoi reads "12x" as 12 and anything else as 0, so a record
// or a kernel file that is not what it should be would still pass as a number.
template <typename T>
auto parse_number(std::string_view s) -> std::optional<T>
{
  T v{};
  // NOLINTNEXTLINE(cppcoreguidelines-pro-bounds-pointer-arithmetic): one past the view's end
  const char * end = s.data() + s.size();
  const auto [ptr, ec] = std::from_chars(s.data(), end, v);
  if (s.empty() || ec != std::errc() || ptr != end) {
    return std::nullopt;
  }
  return v;
}

}  // namespace capa::detail

#endif  // CAPA_PARSE_NUMBER_HPP
