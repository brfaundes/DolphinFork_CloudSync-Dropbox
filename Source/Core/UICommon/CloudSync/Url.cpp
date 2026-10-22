// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/CloudSync/Url.h"

#include <cctype>

namespace CloudSync::Url
{
std::string Encode(std::string_view value)
{
  static constexpr char hex[] = "0123456789ABCDEF";

  std::string result;
  result.reserve(value.size());

  for (const char c : value)
  {
    const auto uc = static_cast<unsigned char>(c);
    if (std::isalnum(uc) || c == '-' || c == '_' || c == '.' || c == '~')
    {
      result.push_back(c);
    }
    else
    {
      result.push_back('%');
      result.push_back(hex[(uc >> 4) & 0xF]);
      result.push_back(hex[uc & 0xF]);
    }
  }

  return result;
}

std::optional<std::string> Decode(std::string_view value)
{
  const auto hex_value = [](const char c) -> int {
    if (c >= '0' && c <= '9')
      return c - '0';
    if (c >= 'a' && c <= 'f')
      return c - 'a' + 10;
    if (c >= 'A' && c <= 'F')
      return c - 'A' + 10;
    return -1;
  };

  std::string result;
  result.reserve(value.size());

  for (size_t i = 0; i < value.size(); ++i)
  {
    if (value[i] == '+')
    {
      result.push_back(' ');
      continue;
    }

    if (value[i] != '%')
    {
      result.push_back(value[i]);
      continue;
    }

    if (i + 2 >= value.size())
      return std::nullopt;

    const int high = hex_value(value[i + 1]);
    const int low = hex_value(value[i + 2]);
    if (high < 0 || low < 0)
      return std::nullopt;

    result.push_back(static_cast<char>((high << 4) | low));
    i += 2;
  }

  return result;
}
}  // namespace CloudSync::Url
