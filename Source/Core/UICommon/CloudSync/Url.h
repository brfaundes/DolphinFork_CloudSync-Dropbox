// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace CloudSync::Url
{
// Percent-encodes a value for use in a query string (RFC 3986 unreserved characters are kept).
std::string Encode(std::string_view value);

// Decodes a percent-encoded query string value. '+' is decoded as a space.
std::optional<std::string> Decode(std::string_view value);
}  // namespace CloudSync::Url
