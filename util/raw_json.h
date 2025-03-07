/*
 * Copyright The OpenTelemetry Authors
 * SPDX-License-Identifier: Apache-2.0
 */

#pragma once

#include <jitbuf/jb.h>

#include <iomanip>
#include <string_view>
#include <type_traits>
#include <utility>

#include <cstdint>
#include <cstdlib>

static constexpr std::string_view json_printable_characters = " !#$%&'()*+,-./0123456789:;<=>?"
                                                              "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_"
                                                              "`abcdefghijklmnopqrstuvwxyz{|}~";

static constexpr std::string_view json_backslash_escape_characters = "\"\\\b\f\n\r\t";

static constexpr std::string_view json_non_hex_escape_characters = " !#$%&'()*+,-./0123456789:;<=>?"
                                                                   "@ABCDEFGHIJKLMNOPQRSTUVWXYZ[]^_"
                                                                   "`abcdefghijklmnopqrstuvwxyz{|}~"
                                                                   "\"\\\b\f\n\r\t";

template <bool DoubleQuoteWrap = true, typename Out> Out &&print_escaped_json_string(Out &&out, std::string_view value)
{
  if constexpr (DoubleQuoteWrap) {
    out << '"';
  }

  while (!value.empty()) {
    // characters that don't need escaping
    {
      auto index = value.find_first_not_of(json_printable_characters);
      if (index == std::string_view::npos) {
        index = value.size();
      }

      out.write(value.data(), index);
      value.remove_prefix(index);
    }

    // characters that need backslash escaping
    {
      auto index = value.find_first_not_of(json_backslash_escape_characters);
      if (index == std::string_view::npos) {
        index = value.size();
      }

      for (auto const c : value.substr(0, index)) {
        out << '\\';
        switch (c) {
        case '\b':
          out << 'b';
          break;
        case '\f':
          out << 'f';
          break;
        case '\n':
          out << 'n';
          break;
        case '\r':
          out << 'r';
          break;
        case '\t':
          out << 't';
          break;
        default:
          out << c;
          break;
        }
      }

      value.remove_prefix(index);
    }

    // characters that need hex escaping
    {
      auto index = value.find_first_of(json_non_hex_escape_characters);
      if (index == std::string_view::npos) {
        index = value.size();
      }

      for (auto const c : value.substr(0, index)) {
        out << "\\u" << std::setw(4) << std::setfill('0') << std::hex
            << static_cast<std::uint16_t>(static_cast<std::uint8_t>(c)) << std::dec << std::setw(0);
      }

      value.remove_prefix(index);
    }
  }

  if constexpr (DoubleQuoteWrap) {
    out << '"';
  }

  return std::forward<Out>(out);
}

template <bool DoubleQuoteWrap = true, typename Out> Out &&print_escaped_json_string(Out &&out, jb_blob value)
{
  print_escaped_json_string<DoubleQuoteWrap>(out, value.string_view());
  return std::forward<Out>(out);
}

template <bool DoubleQuoteWrap = true, typename Out, std::size_t Size>
Out &&print_escaped_json_string(Out &&out, std::uint8_t const (&value)[Size])
{
  print_escaped_json_string<DoubleQuoteWrap>(out, std::string_view(reinterpret_cast<char const *>(value), Size));
  return std::forward<Out>(out);
}

template <bool DoubleQuoteWrap = true, typename Out, std::size_t Size>
Out &&print_escaped_json_string(Out &&out, std::array<unsigned char, Size> const &value)
{
  print_escaped_json_string<DoubleQuoteWrap>(out, to_jb_blob(value));
  return std::forward<Out>(out);
}

inline std::string print_int128(__int128 value) {
    std::ostringstream oss;
    bool is_negative = value < 0;
    __uint128_t uvalue = is_negative ? -static_cast<__uint128_t>(value) : value;  // Treat as unsigned for conversion
    
    // Handle zero case
    if (uvalue == 0) {
        return "0";
    }

    std::string result;
    while (uvalue > 0) {
        result = static_cast<char>('0' + (uvalue % 10)) + result;
        uvalue /= 10;
    }

    if (is_negative) {
        result = "-" + result;
    }

    return result;
}

template <typename T, typename Out>
Out &&print_json_value(Out &&out, T const &value) {
  if constexpr (std::is_same_v<bool, T>) {
    constexpr char const *literals[] = {"false", "true"};
    out << literals[value];
  } else if constexpr (std::is_same_v<std::int8_t, T> || std::is_same_v<char, T>) {
    // avoid printing the raw character
    out << static_cast<std::int16_t>(value);
  } else if constexpr (std::is_same_v<std::uint8_t, T> || std::is_same_v<unsigned char, T>) {
    // avoid printing the raw character
    out << static_cast<std::uint16_t>(value);
  } else if constexpr (std::is_same_v<__int128, T> || std::is_same_v<unsigned __int128, T>) {
    // handle 128-bit integers
    out << print_int128(value);
  } else if constexpr (std::is_integral_v<T>) {
    out << value;
  } else {
    print_escaped_json_string(out, value);
  }
  return std::forward<Out>(out);
}
