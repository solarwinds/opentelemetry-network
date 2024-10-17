/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#include <google/protobuf/io/zero_copy_stream_impl.h>
#include <google/protobuf/message.h>
#include <google/protobuf/text_format.h>

#include <spdlog/cfg/env.h>
#include <spdlog/fmt/fmt.h>
#include <spdlog/fmt/ostr.h>
#include <spdlog/spdlog.h>

std::ostream &operator<<(std::ostream &out, google::protobuf::Message const &message)
{
  google::protobuf::io::OstreamOutputStream output(&out);

  google::protobuf::TextFormat::Printer printer;
  printer.SetExpandAny(true);

  printer.Print(message, &output);

  return out;
}

template <> struct fmt::formatter<google::protobuf::Message> : fmt::ostream_formatter {};
