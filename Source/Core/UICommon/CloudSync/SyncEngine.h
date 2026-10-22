// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

namespace CloudSync
{
enum class SyncDecision
{
  None,
  Download,
  Upload,
  UploadManual,
  Conflict,
};

std::string_view SyncDecisionToString(SyncDecision decision);

struct SyncInput
{
  bool enabled = true;
  bool online = true;
  bool local_exists = false;
  bool pending_manual_upload = false;
  std::optional<std::string> local_hash;
  std::optional<std::string> remote_hash;
  std::optional<std::string> last_synced_hash;
};

SyncDecision DecideSync(const SyncInput& input);
}  // namespace CloudSync
