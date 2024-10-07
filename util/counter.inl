// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <spdlog/cfg/env.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/spdlog.h>

namespace data {

template <typename Out, typename T> Out &&operator<<(Out &&out, Counter<T> const &what)
{
  if (what.empty()) {
    out << "{no_value}";
  } else {
    out << "{value=" << what.value() << '}';
  }
  return std::forward<Out>(out);
}

} // namespace data

template <typename T> struct fmt::formatter<data::Counter<T>> : fmt::ostream_formatter {};
