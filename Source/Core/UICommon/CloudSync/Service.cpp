// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/CloudSync/Service.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <map>
#include <utility>
#include <variant>

#include <fmt/format.h>

#include "Common/FileUtil.h"
#include "Common/Logging/Log.h"
#include "Common/Version.h"
#include "Core/Boot/Boot.h"
#include "Core/Config/MainSettings.h"
#include "Core/ConfigManager.h"
#include "Core/HW/EXI/EXI.h"
#include "Core/HW/EXI/EXI_Device.h"
#include "Core/HW/GCMemcard/GCMemcard.h"
#include "Core/HW/GCMemcard/GCMemcardUtils.h"
#include "Core/HW/WiiSave.h"
#include "Core/State.h"
#include "UICommon/CloudSync/Dropbox.h"
#include "UICommon/CloudSync/Manifest.h"
#include "UICommon/CloudSync/SyncEngine.h"

namespace CloudSync
{
namespace
{
constexpr char SETTINGS_FILE[] = "cloudsync.json";
constexpr char DATA_DIRECTORY[] = "CloudSync/";

enum class InstallKind
{
  CopyFile,
  GameCubeMemcard,
  GameCubeFolder,
  WiiSave,
  State,
};

struct LocalItem
{
  std::string key;
  std::string cache_path;
  std::string destination;
  std::string install_name;
  InstallKind install = InstallKind::CopyFile;
  bool is_state = false;
};

std::string SanitizeFilename(std::string_view name)
{
  std::string result;
  result.reserve(name.size());
  for (const char c : name)
  {
    const bool valid = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                       (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.';
    result.push_back(valid ? c : '_');
  }
  if (result.empty())
    result = "file";
  if (result.size() > 120)
    result.resize(120);
  return result;
}

std::string JoinPath(std::string_view directory, std::string_view name)
{
  return fmt::format("{}/{}", directory, name);
}

std::string GetRegionDirectory(DiscIO::Region region)
{
  return Config::GetDirectoryForRegion(Config::ToGameCubeRegion(region));
}

std::optional<DiscIO::Region> GetMemcardRegion(const GameIdentity& identity)
{
  if (identity.region == DiscIO::Region::Unknown)
    return std::nullopt;
  return identity.region;
}

std::array<u8, 4> GetGameCode(const GameIdentity& identity)
{
  std::array<u8, 4> code{{0, 0, 0, 0}};
  for (size_t i = 0; i < code.size() && i < identity.game_id.size(); ++i)
    code[i] = static_cast<u8>(identity.game_id[i]);
  return code;
}

bool MatchesGameCode(const Memcard::DEntry& entry, const GameIdentity& identity)
{
  return entry.m_gamecode == GetGameCode(identity);
}

bool WriteNormalizedGci(const Memcard::Savefile& save, const std::string& path)
{
  File::CreateFullPath(path);
  return Memcard::WriteSavefile(path, save, Memcard::SavefileFormat::GCI);
}

bool ParseGci(const std::string& path, Memcard::Savefile* save)
{
  auto result = Memcard::ReadSavefile(path);
  if (!std::holds_alternative<Memcard::Savefile>(result))
    return false;
  *save = std::move(std::get<Memcard::Savefile>(result));
  return true;
}

void CollectMemcardFiles(const GameIdentity& identity, ExpansionInterface::Slot slot,
                         const std::string& card_path, const std::string& staging,
                         std::vector<LocalItem>* items)
{
  auto [error, card] = Memcard::GCMemcard::Open(card_path);
  if (!card || !card->IsValid())
    return;

  for (u8 i = 0; i < Memcard::DIRLEN; ++i)
  {
    const auto entry = card->GetDEntry(i);
    if (!entry || entry->m_gamecode == Memcard::DEntry::UNINITIALIZED_GAMECODE ||
        !MatchesGameCode(*entry, identity))
    {
      continue;
    }

    const auto save = card->ExportFile(i);
    if (!save)
      continue;

    const char slot_name = slot == ExpansionInterface::Slot::A ? 'a' : 'b';
    const std::string filename =
        SanitizeFilename(fmt::format("{}.gci", Memcard::GenerateFilename(*entry)));
    LocalItem item;
    item.key = fmt::format("gc-{}-{}", slot_name, filename);
    item.cache_path = JoinPath(staging, item.key);
    item.destination = card_path;
    item.install_name = filename;
    item.install = InstallKind::GameCubeMemcard;
    if (WriteNormalizedGci(*save, item.cache_path))
      items->push_back(std::move(item));
  }
}

void CollectGciFolderFiles(const GameIdentity& identity, ExpansionInterface::Slot slot,
                           const std::string& folder, const std::string& staging,
                           std::vector<LocalItem>* items)
{
  const File::FSTEntry tree = File::ScanDirectoryTree(folder, false);
  for (const File::FSTEntry& child : tree.children)
  {
    if (child.isDirectory)
      continue;

    const std::string path = child.physicalName;
    Memcard::Savefile save;
    if (!ParseGci(path, &save) || !MatchesGameCode(save.dir_entry, identity))
      continue;

    const char slot_name = slot == ExpansionInterface::Slot::A ? 'a' : 'b';
    const std::string filename =
        SanitizeFilename(fmt::format("{}.gci", Memcard::GenerateFilename(save.dir_entry)));
    LocalItem item;
    item.key = fmt::format("gc-{}-{}", slot_name, filename);
    item.cache_path = JoinPath(staging, item.key);
    item.destination = folder;
    item.install_name = filename;
    item.install = InstallKind::GameCubeFolder;
    if (WriteNormalizedGci(save, item.cache_path))
      items->push_back(std::move(item));
  }
}

void CollectGameCubeFiles(const GameIdentity& identity, const std::string& staging,
                          std::vector<LocalItem>* items)
{
  if (identity.game_id.size() < 4)
    return;

  const auto region = GetMemcardRegion(identity);
  for (const ExpansionInterface::Slot slot : {ExpansionInterface::Slot::A,
                                               ExpansionInterface::Slot::B})
  {
    const auto device = Config::Get(Config::GetInfoForEXIDevice(slot));
    if (device == ExpansionInterface::EXIDeviceType::MemoryCard)
    {
      CollectMemcardFiles(identity, slot, Config::GetMemcardPath(slot, region), staging, items);
    }
    else if (device == ExpansionInterface::EXIDeviceType::MemoryCardFolder)
    {
      CollectGciFolderFiles(identity, slot, Config::GetGCIFolderPath(slot, region), staging,
                            items);
    }
  }
}

void CollectWiiFile(const GameIdentity& identity, const std::string& staging,
                    std::vector<LocalItem>* items)
{
  if (!identity.is_wii || identity.title_id == 0)
    return;

  const std::string export_dir = JoinPath(staging, "wii-export");
  if (WiiSave::Export(identity.title_id, export_dir) != WiiSave::CopyResult::Success)
    return;

  const std::string data_bin = fmt::format(
      "{}/private/wii/title/{}{}{}{}/data.bin", export_dir,
      static_cast<char>(identity.title_id >> 24), static_cast<char>(identity.title_id >> 16),
      static_cast<char>(identity.title_id >> 8), static_cast<char>(identity.title_id));
  if (!File::IsFile(data_bin))
    return;

  LocalItem item;
  item.key = "wii-save.bin";
  item.cache_path = data_bin;
  item.install = InstallKind::WiiSave;
  items->push_back(std::move(item));
}

bool IsCompatibleState(const std::string& path, const GameIdentity& identity)
{
  State::StateHeader header;
  if (!State::ReadStateHeader(path, header))
    return false;
  if (header.version_header.version_cookie != State::GetStateVersionCookie())
    return false;
  return std::string_view(header.legacy_header.game_id, sizeof(header.legacy_header.game_id)) ==
         identity.game_id.substr(0, sizeof(header.legacy_header.game_id));
}

void CollectStateFiles(const GameIdentity& identity, std::vector<LocalItem>* items)
{
  const std::string states_dir = File::GetUserPath(D_STATESAVES_IDX);
  if (!File::IsDirectory(states_dir))
    return;

  const std::string prefix = fmt::format("{}.s", identity.game_id);
  const File::FSTEntry tree = File::ScanDirectoryTree(states_dir, false);
  for (const File::FSTEntry& child : tree.children)
  {
    if (child.isDirectory || !child.virtualName.starts_with(prefix))
      continue;

    const std::string path = child.physicalName;
    if (!IsCompatibleState(path, identity))
      continue;

    LocalItem item;
    item.key = fmt::format("state-{}", SanitizeFilename(child.virtualName));
    item.cache_path = path;
    item.destination = states_dir;
    item.install_name = child.virtualName;
    item.install = InstallKind::State;
    item.is_state = true;
    items->push_back(std::move(item));
  }
}

LocalItem MakeDownloadItem(const std::string& key, const std::string& staging,
                           const GameIdentity& identity)
{
  LocalItem item;
  item.key = key;
  item.cache_path = JoinPath(staging, fmt::format("download-{}", key));

  if (key.starts_with("gc-a-") || key.starts_with("gc-b-"))
  {
    const auto region = GetMemcardRegion(identity);
    const auto slot = key.starts_with("gc-a-") ? ExpansionInterface::Slot::A :
                                                  ExpansionInterface::Slot::B;
    const auto device = Config::Get(Config::GetInfoForEXIDevice(slot));
    if (device == ExpansionInterface::EXIDeviceType::MemoryCard)
    {
      item.destination = Config::GetMemcardPath(slot, region);
      item.install = InstallKind::GameCubeMemcard;
    }
    else
    {
      item.destination = Config::GetGCIFolderPath(slot, region);
      item.install = InstallKind::GameCubeFolder;
    }
    item.install_name = key.substr(5);
  }
  else if (key == "wii-save.bin")
  {
    item.install = InstallKind::WiiSave;
  }
  else if (key.starts_with("state-"))
  {
    item.destination = File::GetUserPath(D_STATESAVES_IDX);
    item.install_name = key.substr(6);
    item.install = InstallKind::State;
    item.is_state = true;
  }

  return item;
}

bool InstallMemcard(const std::string& card_path, const std::string& downloaded, std::string* error)
{
  Memcard::Savefile save;
  if (!ParseGci(downloaded, &save))
  {
    *error = "Downloaded memory card save is corrupted";
    return false;
  }

  auto [open_error, card] = Memcard::GCMemcard::Open(card_path);
  if (!card || !card->IsValid())
  {
    *error = fmt::format("Unable to open memory card '{}'", card_path);
    return false;
  }

  for (u8 i = 0; i < Memcard::DIRLEN; ++i)
  {
    const auto entry = card->GetDEntry(i);
    if (entry && Memcard::HasSameIdentity(*entry, save.dir_entry))
    {
      if (card->RemoveFile(i) != Memcard::GCMemcardRemoveFileRetVal::SUCCESS)
      {
        *error = "Unable to replace the existing memory card save";
        return false;
      }
    }
  }

  if (card->ImportFile(save) != Memcard::GCMemcardImportFileRetVal::SUCCESS)
  {
    *error = "Unable to import the memory card save";
    return false;
  }

  if (!card->Save())
  {
    *error = "Unable to write the memory card";
    return false;
  }

  return true;
}

bool InstallGciFolder(const std::string& folder, const std::string& install_name,
                      const std::string& downloaded, std::string* error)
{
  Memcard::Savefile save;
  if (!ParseGci(downloaded, &save))
  {
    *error = "Downloaded GCI save is corrupted";
    return false;
  }

  const File::FSTEntry tree = File::ScanDirectoryTree(folder, false);
  for (const File::FSTEntry& child : tree.children)
  {
    if (child.isDirectory)
      continue;

    const std::string path = child.physicalName;
    Memcard::Savefile existing;
    if (ParseGci(path, &existing) && Memcard::HasSameIdentity(existing.dir_entry, save.dir_entry))
      File::Delete(path, File::IfAbsentBehavior::NoConsoleWarning);
  }

  File::CreateDirs(folder);
  if (!File::CopyRegularFile(downloaded, JoinPath(folder, install_name)))
  {
    *error = "Unable to write the GCI save";
    return false;
  }
  return true;
}

bool InstallWiiSave(const std::string& downloaded, std::string* error)
{
  const auto result = WiiSave::Import(downloaded, [] { return true; });
  if (result != WiiSave::CopyResult::Success)
  {
    *error = fmt::format("Unable to import the Wii save (error {})", static_cast<int>(result));
    return false;
  }
  return true;
}

bool InstallItem(const LocalItem& item, const std::string& downloaded, std::string* error)
{
  switch (item.install)
  {
  case InstallKind::GameCubeMemcard:
    return InstallMemcard(item.destination, downloaded, error);
  case InstallKind::GameCubeFolder:
    return InstallGciFolder(item.destination, item.install_name, downloaded, error);
  case InstallKind::WiiSave:
    return InstallWiiSave(downloaded, error);
  case InstallKind::State:
    File::CreateDirs(item.destination);
    if (!File::CopyRegularFile(downloaded, JoinPath(item.destination, item.install_name)))
    {
      *error = "Unable to write the save state";
      return false;
    }
    return true;
  case InstallKind::CopyFile:
    File::CreateFullPath(item.destination);
    if (!File::CopyRegularFile(downloaded, item.destination))
    {
      *error = "Unable to write the downloaded file";
      return false;
    }
    return true;
  }

  *error = "Unknown install type";
  return false;
}

std::string ConflictDirectory(const std::string& game_key, u16 revision)
{
  return fmt::format("{}/conflicts/{}/{}", Service::GetDataDir(), game_key,
                     MakeRevisionKey(revision));
}

bool PreserveConflictCopy(const std::string& path, const std::string& destination,
                          std::string* error)
{
  File::CreateFullPath(destination);
  if (!File::CopyRegularFile(path, destination))
  {
    *error = fmt::format("Unable to preserve conflict copy '{}'", destination);
    return false;
  }
  return true;
}

std::string ManifestPath(const std::string& game_key, u16 revision)
{
  return fmt::format("{}/manifests/{}-{}.json", Service::GetDataDir(), game_key,
                     MakeRevisionKey(revision));
}

std::string RemoteFile(const std::string& folder, std::string_view key)
{
  return fmt::format("{}/{}", folder, key);
}

void UpdateEntry(ManifestEntry* entry, const std::string& key, const std::string& kind,
                 const Dropbox::Metadata& remote, const LocalItem& local,
                 const std::string& local_hash)
{
  entry->key = key;
  entry->kind = kind;
  entry->remote_path = remote.path_display.empty() ? remote.path_lower : remote.path_display;
  entry->remote_rev = remote.rev;
  entry->remote_hash = remote.content_hash;
  entry->local_path = local.destination;
  entry->local_hash = local_hash;
  entry->last_synced_hash = remote.content_hash;
  entry->modified_time = remote.server_modified;
  entry->size = remote.size;
}
}  // namespace

std::optional<GameIdentity> IdentifyGame(const BootParameters& boot)
{
  GameIdentity identity;
  std::visit(
      [&](const auto& parameters) {
        using T = std::decay_t<decltype(parameters)>;
        if constexpr (std::is_same_v<T, BootParameters::Disc>)
        {
          if (!parameters.volume)
            return;
          identity.game_id = parameters.volume->GetGameID();
          identity.title = parameters.volume->GetInternalName();
          identity.title_id = parameters.volume->GetTitleID().value_or(0);
          identity.revision = parameters.volume->GetRevision().value_or(0);
          identity.region = parameters.volume->GetRegion();
          identity.is_wii = parameters.volume->GetVolumeType() == DiscIO::Platform::WiiDisc;
        }
        else if constexpr (std::is_same_v<T, DiscIO::VolumeWAD>)
        {
          identity.game_id = parameters.GetGameID();
          identity.title_id = parameters.GetTitleID().value_or(0);
          identity.revision = parameters.GetRevision().value_or(0);
          identity.region = parameters.GetRegion();
          identity.is_wii = true;
        }
        else if constexpr (std::is_same_v<T, BootParameters::NANDTitle>)
        {
          identity.game_id = fmt::format("{:016x}", parameters.id);
          identity.title_id = parameters.id;
          identity.is_wii = true;
        }
      },
      boot.parameters);

  if (!identity.IsValid())
    return std::nullopt;
  return identity;
}

std::string MakeGameKey(const GameIdentity& identity)
{
  std::string id = identity.game_id;
  if (id.empty())
    id = fmt::format("{:016x}", identity.title_id);
  return fmt::format("{}-{}", identity.is_wii ? "wii" : "gc", SanitizeFilename(id));
}

std::string MakeRevisionKey(u16 revision)
{
  return fmt::format("r{}", revision);
}

RemotePaths GetRemotePaths(const Settings& settings, const GameIdentity& identity)
{
  const std::string root = Dropbox::NormalizePath(settings.root_path);
  const std::string game_key = MakeGameKey(identity);
  const std::string revision = MakeRevisionKey(identity.revision);
  return {
      fmt::format("{}/juegos/{}/guardados/{}", root, game_key, revision),
      fmt::format("{}/juegos/{}/estados/{}/{}", root, game_key, SanitizeFilename(identity.game_id),
                  revision),
  };
}

Service& Service::GetInstance()
{
  static Service instance;
  return instance;
}

Service::Service()
{
  LoadSettings();
}

std::string Service::GetSettingsPath()
{
  return File::GetUserPath(D_CONFIG_IDX) + SETTINGS_FILE;
}

std::string Service::GetDataDir()
{
  return File::GetUserPath(D_CONFIG_IDX) + DATA_DIRECTORY;
}

bool Service::LoadSettings(std::string* error)
{
  const std::string path = GetSettingsPath();
  if (!File::Exists(path))
    return false;

  auto settings = CloudSync::LoadSettings(path, error);
  if (!settings)
    return false;
  m_settings = std::move(*settings);
  return true;
}

bool Service::SaveSettings(std::string* error)
{
  return CloudSync::SaveSettings(GetSettingsPath(), m_settings, error);
}

bool Service::HasAccount() const
{
  return !m_settings.refresh_token.empty() || !m_settings.access_token.empty();
}

bool Service::RefreshAccessToken(std::string* error)
{
  if (m_settings.refresh_token.empty())
  {
    if (error)
      *error = "CloudSync is not connected to a Dropbox account";
    return false;
  }

  const std::string client_id =
      m_settings.client_id.empty() ? Auth::DEFAULT_APP_KEY : m_settings.client_id;
  auto response = Auth::RefreshAccessToken(client_id, m_settings.refresh_token, error);
  if (!response)
    return false;

  m_settings.access_token = std::move(response->access_token);
  if (!response->refresh_token.empty())
    m_settings.refresh_token = std::move(response->refresh_token);
  if (response->expires_in)
  {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    m_settings.access_token_expires_at = static_cast<u64>(now) + *response->expires_in;
  }
  SaveSettings();
  return true;
}

bool Service::EnsureAccessToken(std::string* error)
{
  const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                       std::chrono::system_clock::now().time_since_epoch())
                       .count();
  if (!m_settings.access_token.empty() && m_settings.access_token_expires_at &&
      *m_settings.access_token_expires_at > static_cast<u64>(now) + 60)
  {
    return true;
  }

  if (!m_settings.access_token.empty() && !m_settings.access_token_expires_at)
    return true;

  return RefreshAccessToken(error);
}

bool Service::CompleteAuthorization(const Auth::TokenResponse& response, std::string* error)
{
  if (response.access_token.empty() || response.refresh_token.empty())
  {
    if (error)
      *error = "Dropbox did not return a refresh token";
    return false;
  }

  m_settings.access_token = response.access_token;
  m_settings.refresh_token = response.refresh_token;
  m_settings.account_id = response.account_id;
  if (response.expires_in)
  {
    const auto now = std::chrono::duration_cast<std::chrono::seconds>(
                         std::chrono::system_clock::now().time_since_epoch())
                         .count();
    m_settings.access_token_expires_at = static_cast<u64>(now) + *response.expires_in;
  }
  m_settings.enabled = true;
  return SaveSettings(error);
}

void Service::Disconnect()
{
  m_settings.access_token.clear();
  m_settings.refresh_token.clear();
  m_settings.account_id.clear();
  m_settings.access_token_expires_at.reset();
  m_settings.enabled = false;
  SaveSettings();
}

bool Service::TestConnection(std::string* error)
{
  if (!EnsureAccessToken(error))
    return false;

  Dropbox::Client client(m_settings.access_token);
  const auto entries = client.ListFolder("", error);
  return entries.has_value();
}

SyncResult Service::Synchronize(const GameIdentity& identity)
{
  return SynchronizeInternal(identity, false);
}

SyncResult Service::UploadLocalSave(const GameIdentity& identity)
{
  return SynchronizeInternal(identity, true);
}

SyncResult Service::SynchronizeInternal(const GameIdentity& identity, bool manual_upload)
{
  SyncResult result;
  if (!identity.IsValid())
  {
    result.status = SyncStatus::Error;
    result.message = "No game is running";
    return result;
  }

  if (!m_settings.enabled)
  {
    result.status = SyncStatus::Disabled;
    result.message = "Cloud sync is disabled";
    return result;
  }

  if (!HasAccount())
  {
    result.status = SyncStatus::NotConfigured;
    result.message = "Cloud sync is not connected to Dropbox";
    return result;
  }

  std::string error;
  if (!EnsureAccessToken(&error))
  {
    result.status = SyncStatus::Offline;
    result.message = error;
    return result;
  }

  const RemotePaths paths = GetRemotePaths(m_settings, identity);
  Dropbox::Client client(m_settings.access_token);
  if (!client.EnsureFolder(paths.saves, &error))
  {
    result.status = SyncStatus::Offline;
    result.message = error;
    return result;
  }
  if (m_settings.sync_states && !client.EnsureFolder(paths.states, &error))
  {
    result.status = SyncStatus::Offline;
    result.message = error;
    return result;
  }

  const std::string staging = File::CreateTempDir();
  if (staging.empty())
  {
    result.status = SyncStatus::Error;
    result.message = "Unable to create a temporary directory";
    return result;
  }

  const std::string game_key = MakeGameKey(identity);
  const std::string manifest_path = ManifestPath(game_key, identity.revision);
  Manifest manifest;
  if (File::Exists(manifest_path))
  {
    std::string manifest_error;
    if (const auto loaded = CloudSync::LoadManifest(manifest_path, &manifest_error))
      manifest = std::move(*loaded);
  }
  manifest.root_path = paths.saves;

  std::vector<LocalItem> local_items;
  if (!identity.is_wii)
    CollectGameCubeFiles(identity, staging, &local_items);
  else
    CollectWiiFile(identity, staging, &local_items);
  if (m_settings.sync_states)
    CollectStateFiles(identity, &local_items);

  std::map<std::string, Dropbox::Metadata> remote_files;
  const auto save_entries = client.ListAll(paths.saves, &error);
  if (!save_entries)
  {
    File::DeleteDirRecursively(staging);
    result.status = SyncStatus::Offline;
    result.message = error;
    return result;
  }
  for (const auto& entry : *save_entries)
  {
    if (entry.IsFile())
      remote_files.emplace(entry.name, entry);
  }

  if (m_settings.sync_states)
  {
    const auto state_entries = client.ListAll(paths.states, &error);
    if (!state_entries)
    {
      File::DeleteDirRecursively(staging);
      result.status = SyncStatus::Offline;
      result.message = error;
      return result;
    }
    for (const auto& entry : *state_entries)
    {
      if (entry.IsFile())
        remote_files.emplace(entry.name, entry);
    }
  }

  bool had_error = false;
  bool had_conflict = false;
  std::map<std::string, LocalItem> local_by_key;
  for (const auto& item : local_items)
    local_by_key.emplace(item.key, item);

  auto synchronize_item = [&](const LocalItem& item, const Dropbox::Metadata* remote,
                              std::optional<std::string> local_hash) {
    const std::string kind = item.is_state ? "state" : "save";
    const std::string folder = item.is_state ? paths.states : paths.saves;
    const auto manifest_it = manifest.entries.find(item.key);
    const std::string last_hash =
        manifest_it == manifest.entries.end() ? std::string{} : manifest_it->second.last_synced_hash;

    SyncInput input;
    input.local_exists = true;
    input.local_hash = std::move(local_hash);
    if (remote)
    {
      input.remote_hash = remote->content_hash;
      input.last_synced_hash = last_hash.empty() ? std::nullopt : std::optional(last_hash);
    }
    input.pending_manual_upload = manual_upload;

    const SyncDecision decision = manual_upload ? SyncDecision::UploadManual : DecideSync(input);
    if (decision == SyncDecision::None)
    {
      if (remote)
      {
        ManifestEntry& entry = manifest.entries[item.key];
        UpdateEntry(&entry, item.key, kind, *remote, item, input.local_hash.value_or(""));
      }
      return;
    }

    if (decision == SyncDecision::Conflict)
    {
      had_conflict = true;
      result.conflicts.push_back(item.key);
      const std::string conflict_dir = ConflictDirectory(game_key, identity.revision);
      PreserveConflictCopy(item.cache_path, JoinPath(conflict_dir, "local-" + item.key), &error);
      if (remote)
      {
        std::string remote_copy = JoinPath(staging, "conflict-" + item.key);
        if (client.DownloadFile(RemoteFile(folder, item.key), remote_copy, &error))
          PreserveConflictCopy(remote_copy, JoinPath(conflict_dir, "remote-" + item.key), &error);
      }
      return;
    }

    if (decision == SyncDecision::Upload || decision == SyncDecision::UploadManual)
    {
      if (manual_upload && remote && !remote->content_hash.empty() &&
          remote->content_hash != input.local_hash)
      {
        const std::string conflict_dir = ConflictDirectory(game_key, identity.revision);
        std::string remote_copy = JoinPath(staging, "backup-" + item.key);
        if (client.DownloadFile(RemoteFile(folder, item.key), remote_copy, &error))
          PreserveConflictCopy(remote_copy, JoinPath(conflict_dir, "remote-" + item.key), &error);
      }

      const auto uploaded =
          client.UploadFile(RemoteFile(folder, item.key), item.cache_path, &error);
      if (!uploaded)
      {
        had_error = true;
        return;
      }
      ManifestEntry& entry = manifest.entries[item.key];
      UpdateEntry(&entry, item.key, kind, *uploaded, item, input.local_hash.value_or(""));
      result.uploaded.push_back(item.key);
      return;
    }

    if (decision == SyncDecision::Download && remote)
    {
      const std::string downloaded = item.cache_path;
      if (!client.DownloadFile(RemoteFile(folder, item.key), downloaded, &error))
      {
        had_error = true;
        return;
      }

      if (item.is_state && !IsCompatibleState(downloaded, identity))
      {
        had_error = true;
        result.message = "A remote save state is not compatible with this Dolphin version";
        return;
      }

      if (!InstallItem(item, downloaded, &error))
      {
        had_error = true;
        return;
      }

      ManifestEntry& entry = manifest.entries[item.key];
      UpdateEntry(&entry, item.key, kind, *remote, item, remote->content_hash);
      result.downloaded.push_back(item.key);
    }
  };

  for (const auto& [key, item] : local_by_key)
  {
    const auto remote_it = remote_files.find(key);
    const auto local_hash = Dropbox::Client::ComputeContentHash(item.cache_path, &error);
    if (!local_hash)
    {
      had_error = true;
      continue;
    }
    synchronize_item(item, remote_it == remote_files.end() ? nullptr : &remote_it->second,
                     local_hash);
  }

  for (const auto& [key, remote] : remote_files)
  {
    if (local_by_key.contains(key))
      continue;

    const auto manifest_it = manifest.entries.find(key);
    const bool known = manifest_it != manifest.entries.end();
    const bool manual = !key.starts_with("state-") || m_settings.sync_states;
    if (!manual)
      continue;

    LocalItem item = MakeDownloadItem(key, staging, identity);
    if (item.install == InstallKind::CopyFile && item.destination.empty())
      continue;

    SyncInput input;
    input.local_exists = false;
    input.remote_hash = remote.content_hash;
    if (known)
      input.last_synced_hash = manifest_it->second.last_synced_hash.empty() ?
                                   std::nullopt :
                                   std::optional(manifest_it->second.last_synced_hash);

    const SyncDecision decision = manual_upload ? SyncDecision::None : DecideSync(input);
    if (decision != SyncDecision::Download)
      continue;

    if (!client.DownloadFile(RemoteFile(item.is_state ? paths.states : paths.saves, key),
                             item.cache_path, &error))
    {
      had_error = true;
      continue;
    }

    if (item.is_state && !IsCompatibleState(item.cache_path, identity))
    {
      had_error = true;
      result.message = "A remote save state is not compatible with this Dolphin version";
      continue;
    }

    if (!InstallItem(item, item.cache_path, &error))
    {
      had_error = true;
      continue;
    }

    ManifestEntry& entry = manifest.entries[key];
    UpdateEntry(&entry, key, item.is_state ? "state" : "save", remote, item, remote.content_hash);
    result.downloaded.push_back(key);
  }

  std::string manifest_error;
  CloudSync::SaveManifest(manifest_path, manifest, &manifest_error);
  File::DeleteDirRecursively(staging);

  if (had_conflict)
  {
    result.status = SyncStatus::Conflict;
    result.message = "Cloud and local saves differ. Both versions were preserved.";
  }
  else if (had_error)
  {
    result.status = SyncStatus::Error;
    if (result.message.empty())
      result.message = error;
  }
  else
  {
    result.status = SyncStatus::Success;
    if (result.message.empty())
    {
      result.message = fmt::format("Cloud sync completed: {} uploaded, {} downloaded",
                                   result.uploaded.size(), result.downloaded.size());
    }
  }
  return result;
}
}  // namespace CloudSync