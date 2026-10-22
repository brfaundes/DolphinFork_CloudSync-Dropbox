// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <map>
#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace CloudSync
{
struct ManifestEntry
{
  std::string key;
  std::string kind;
  std::string remote_path;
  std::string remote_rev;
  std::string remote_hash;
  std::string local_path;
  std::string local_hash;
  std::string last_synced_hash;
  std::string modified_time;
  u64 size = 0;
  bool pending_manual_upload = false;
};

struct Manifest
{
  int version = 2;
  std::string root_path;
  std::map<std::string, ManifestEntry> entries;
};

std::optional<Manifest> ParseManifest(std::string_view json, std::string* error);
std::string SerializeManifest(const Manifest& manifest);
std::optional<Manifest> LoadManifest(const std::string& path, std::string* error);
bool SaveManifest(const std::string& path, const Manifest& manifest, std::string* error);
}  // namespace CloudSync
