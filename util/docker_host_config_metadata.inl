// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <spdlog/cfg/env.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/spdlog.h>

template <typename Out> Out &&operator<<(Out &&out, DockerHostConfigMetadata const &what)
{
  out << "\"cpu_shares\":" << what.cpu_shares_ << ",\"cpu_period\":" << what.cpu_period_ << ",\"cpu_quota\":" << what.cpu_quota_
      << ",\"memory_swappiness\":" << static_cast<std::uint16_t>(what.memory_swappiness_)
      << ",\"memory_limit\":" << what.memory_limit_ << ",\"memory_soft_limit\":" << what.memory_soft_limit_
      << ",\"total_memory_limit\":" << what.total_memory_limit_;

  return std::forward<Out>(out);
}

template <> struct fmt::formatter<DockerHostConfigMetadata> : fmt::ostream_formatter {};
