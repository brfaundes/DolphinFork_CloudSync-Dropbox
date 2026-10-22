// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <vector>

#include "Common/CommonTypes.h"
#include "DiscIO/Enums.h"
#include "UICommon/CloudSync/Auth.h"
#include "UICommon/CloudSync/Settings.h"

struct BootParameters;

namespace CloudSync
{
struct GameIdentity
{
  std::string game_id;
  std::string title;
  u64 title_id = 0;
  u16 revision = 0;
  DiscIO::Region region = DiscIO::Region::Unknown;
  bool is_wii = false;

  bool IsValid() const { return !game_id.empty() || title_id != 0; }
};

std::optional<GameIdentity> IdentifyGame(const BootParameters& boot);
std::string MakeGameKey(const GameIdentity& identity);
std::string MakeRevisionKey(u16 revision);

struct RemotePaths
{
  std::string saves;
  std::string states;
};

RemotePaths GetRemotePaths(const Settings& settings, const GameIdentity& identity);

enum class SyncStatus
{
  Success,
  Disabled,
  NotConfigured,
  Offline,
  Conflict,
  Error,
};

struct SyncResult
{
  SyncStatus status = SyncStatus::Error;
  std::string message;
  std::vector<std::string> uploaded;
  std::vector<std::string> downloaded;
  std::vector<std::string> conflicts;
};

class Service
{
public:
  static Service& GetInstance();

  static std::string GetSettingsPath();
  static std::string GetDataDir();

  const Settings& GetSettings() const { return m_settings; }
  void SetSettings(Settings settings) { m_settings = std::move(settings); }

  bool LoadSettings(std::string* error = nullptr);
  bool SaveSettings(std::string* error = nullptr);

  bool HasAccount() const;
  bool EnsureAccessToken(std::string* error);
  bool CompleteAuthorization(const Auth::TokenResponse& response, std::string* error);
  void Disconnect();

  SyncResult Synchronize(const GameIdentity& identity);
  SyncResult UploadLocalSave(const GameIdentity& identity);
  bool TestConnection(std::string* error);

private:
  Service();

  SyncResult SynchronizeInternal(const GameIdentity& identity, bool manual_upload);
  bool RefreshAccessToken(std::string* error);

  Settings m_settings;
};
}  // namespace CloudSync