// Copyright The OpenTelemetry Authors
// SPDX-License-Identifier: Apache-2.0

#include <spdlog/cfg/env.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/spdlog.h>

template <typename Out> Out &&operator<<(Out &&out, K8sMetadata const &what)
{
  out << "\"container_name\":\"" << what.container_name_ << "\",\"pod_name\":\"" << what.pod_name_ << "\",\"pod_ns\":\""
      << what.pod_ns_ << "\",\"pod_uid\":\"" << what.pod_uid_ << "\",\"sandbox_uid\":\"" << what.sandbox_uid_
      << "\",\"ports\":[";
  bool first = true;
  for (auto const &port : what.ports_) {
    if (first) {
      first = false;
    } else {
      out << ',';
    }
    out << '"' << port.second.port << "\":{\"name\":\"" << port.second.name << "\",\"protocol\":\"" << port.second.protocol
        << "\"}";
  }
  out << ']';

  return std::forward<Out>(out);
}

template <> struct fmt::formatter<K8sMetadata> : fmt::ostream_formatter {};
