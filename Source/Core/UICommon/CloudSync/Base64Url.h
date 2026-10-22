// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace CloudSync::Base64Url
{
std::string Encode(std::span<const u8> input);
std::string Encode(std::string_view input);
std::optional<std::vector<u8>> Decode(std::string_view input);
}  // namespace CloudSync::Base64Url
