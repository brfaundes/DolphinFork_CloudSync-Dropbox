// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/CloudSync/Base64Url.h"

#include <array>

namespace CloudSync::Base64Url
{
std::string Encode(std::span<const u8> input)
{
  static constexpr char alphabet[] =
      "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";

  std::string result;
  result.reserve((input.size() + 2) / 3 * 4);

  size_t i = 0;
  while (i + 3 <= input.size())
  {
    const u32 value = (static_cast<u32>(input[i]) << 16) | (static_cast<u32>(input[i + 1]) << 8) |
                      static_cast<u32>(input[i + 2]);
    result.push_back(alphabet[(value >> 18) & 0x3F]);
    result.push_back(alphabet[(value >> 12) & 0x3F]);
    result.push_back(alphabet[(value >> 6) & 0x3F]);
    result.push_back(alphabet[value & 0x3F]);
    i += 3;
  }

  const size_t remainder = input.size() - i;
  if (remainder == 1)
  {
    const u32 value = static_cast<u32>(input[i]) << 16;
    result.push_back(alphabet[(value >> 18) & 0x3F]);
    result.push_back(alphabet[(value >> 12) & 0x3F]);
  }
  else if (remainder == 2)
  {
    const u32 value = (static_cast<u32>(input[i]) << 16) | (static_cast<u32>(input[i + 1]) << 8);
    result.push_back(alphabet[(value >> 18) & 0x3F]);
    result.push_back(alphabet[(value >> 12) & 0x3F]);
    result.push_back(alphabet[(value >> 6) & 0x3F]);
  }

  return result;
}

std::string Encode(std::string_view input)
{
  return Encode(std::span<const u8>{reinterpret_cast<const u8*>(input.data()), input.size()});
}

std::optional<std::vector<u8>> Decode(std::string_view input)
{
  auto decode_char = [](char c) -> int {
    if (c >= 'A' && c <= 'Z')
      return c - 'A';
    if (c >= 'a' && c <= 'z')
      return c - 'a' + 26;
    if (c >= '0' && c <= '9')
      return c - '0' + 52;
    if (c == '-')
      return 62;
    if (c == '_')
      return 63;
    return -1;
  };

  while (!input.empty() && input.back() == '=')
    input.remove_suffix(1);

  if (input.size() % 4 == 1)
    return std::nullopt;

  std::vector<u8> result;
  result.reserve(input.size() * 3 / 4);

  u32 accumulator = 0;
  int bits = 0;
  for (const char c : input)
  {
    const int value = decode_char(c);
    if (value < 0)
      return std::nullopt;

    accumulator = (accumulator << 6) | static_cast<u32>(value);
    bits += 6;
    if (bits >= 8)
    {
      bits -= 8;
      result.push_back(static_cast<u8>((accumulator >> bits) & 0xFF));
    }
  }

  return result;
}
}  // namespace CloudSync::Base64Url
