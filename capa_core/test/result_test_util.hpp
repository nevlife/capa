#ifndef CAPA_RESULT_TEST_UTIL_HPP
#define CAPA_RESULT_TEST_UTIL_HPP

#include "capa/result.hpp"

#include <gtest/gtest.h>

#include <cstdlib>
#include <optional>
#include <utility>

// The kind of a failed Result, nullopt for a success, so EXPECT_EQ prints which one it got.
template <class T>
auto kind_of(const capa::Result<T> & r) -> std::optional<capa::ErrorKind>
{
  if (r) {
    return std::nullopt;
  }
  return r.error().kind;
}

// The value of a call the test depends on: a failure ends the test run with the reason.
template <class T>
auto must(capa::Result<T> r) -> T
{
  if (!r) {
    ADD_FAILURE() << r.error().what;
    std::abort();
  }
  return *std::move(r);
}

inline void must(const capa::Result<void> & r)
{
  if (!r) {
    ADD_FAILURE() << r.error().what;
    std::abort();
  }
}

#endif  // CAPA_RESULT_TEST_UTIL_HPP
