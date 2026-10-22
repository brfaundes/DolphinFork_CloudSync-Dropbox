// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace CloudSync
{
struct Settings
{
  bool enabled = false;
  bool auto_sync = true;
#ifdef __ANDROID__
  bool sync_states = false;
#else
  bool sync_states = true;
#endif
  std::string client_id;
  std::string root_path = "/DolphinFork";
  std::string access_token;
  std::string refresh_token;
  std::string account_id;
  std::optional<u64> access_token_expires_at;
};

std::string SerializeSettings(const Settings& settings);
std::optional<Settings> ParseSettings(std::string_view json, std::string* error);
std::optional<Settings> LoadSettings(const std::string& path, std::string* error);
bool SaveSettings(const std::string& path, const Settings& settings, std::string* error);

using TokenProtectionCallback = std::string (*)(std::string_view);

void SetTokenProtectionCallbacks(TokenProtectionCallback protect, TokenProtectionCallback unprotect);

// Uses the registered platform callbacks when available, otherwise DPAPI on Windows.
std::string ProtectToken(std::string_view token);
std::string UnprotectToken(std::string_view token);
}  // namespace CloudSync
