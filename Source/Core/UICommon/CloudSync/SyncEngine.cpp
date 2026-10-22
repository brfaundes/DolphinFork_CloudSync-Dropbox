// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/CloudSync/SyncEngine.h"

namespace CloudSync
{
std::string_view SyncDecisionToString(SyncDecision decision)
{
  switch (decision)
  {
  case SyncDecision::None:
    return "none";
  case SyncDecision::Download:
    return "download";
  case SyncDecision::Upload:
    return "upload";
  case SyncDecision::UploadManual:
    return "upload_manual";
  case SyncDecision::Conflict:
    return "conflict";
  }

  return "unknown";
}

SyncDecision DecideSync(const SyncInput& input)
{
  if (!input.enabled || !input.online)
    return SyncDecision::None;

  const bool local_matches_last =
      input.local_hash && input.last_synced_hash && *input.local_hash == *input.last_synced_hash;
  const bool remote_matches_last =
      input.remote_hash && input.last_synced_hash && *input.remote_hash == *input.last_synced_hash;

  if (!input.local_exists)
  {
    // A missing local save is only restored automatically on a fresh device. Deletions are never
    // propagated by the sync engine.
    return (input.remote_hash && !input.last_synced_hash) ? SyncDecision::Download :
                                                            SyncDecision::None;
  }

  if (!input.remote_hash)
    return input.pending_manual_upload ? SyncDecision::UploadManual : SyncDecision::Upload;

  if (input.local_hash && *input.local_hash == *input.remote_hash)
    return SyncDecision::None;

  if (!input.last_synced_hash)
    return SyncDecision::Conflict;

  if (local_matches_last)
    return SyncDecision::Download;

  if (remote_matches_last)
    return input.pending_manual_upload ? SyncDecision::UploadManual : SyncDecision::Upload;

  return SyncDecision::Conflict;
}
}  // namespace CloudSync
