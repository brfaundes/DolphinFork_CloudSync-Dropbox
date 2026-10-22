// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/CloudSync/Settings.h"

#include <span>

#include <picojson.h>

#ifdef _WIN32
#include <windows.h>
#include <wincrypt.h>
#endif

#include "Common/FileUtil.h"
#include "UICommon/CloudSync/Base64Url.h"

namespace CloudSync
{
namespace
{
std::string GetString(const picojson::object& object, const char* key)
{
  const auto it = object.find(key);
  if (it == object.end() || !it->second.is<std::string>())
    return {};
  return it->second.get<std::string>();
}

bool GetBool(const picojson::object& object, const char* key, bool fallback)
{
  const auto it = object.find(key);
  return it != object.end() && it->second.is<bool>() ? it->second.get<bool>() : fallback;
}

std::optional<u64> GetU64(const picojson::object& object, const char* key)
{
  const auto it = object.find(key);
  if (it == object.end() || !it->second.is<double>())
    return std::nullopt;
  return static_cast<u64>(it->second.get<double>());
}

TokenProtectionCallback s_protect_token;
TokenProtectionCallback s_unprotect_token;
}  // namespace

void SetTokenProtectionCallbacks(TokenProtectionCallback protect,
                                 TokenProtectionCallback unprotect)
{
  s_protect_token = protect;
  s_unprotect_token = unprotect;
}

std::string ProtectToken(std::string_view token)
{
  if (token.empty())
    return {};

  if (s_protect_token)
    return s_protect_token(token);

#ifdef _WIN32
  DATA_BLOB input;
  input.cbData = static_cast<DWORD>(token.size());
  input.pbData = reinterpret_cast<BYTE*>(const_cast<char*>(token.data()));

  DATA_BLOB output{};
  if (!CryptProtectData(&input, L"Dolphin CloudSync", nullptr, nullptr, nullptr,
                        CRYPTPROTECT_UI_FORBIDDEN, &output))
  {
    return {};
  }

  const auto bytes = std::span<const u8>(output.pbData, output.cbData);
  std::string result = "dpapi:" + Base64Url::Encode(bytes);
  LocalFree(output.pbData);
  return result;
#else
  return std::string{token};
#endif
}

std::string UnprotectToken(std::string_view token)
{
  if (token.empty())
    return {};

  if (s_unprotect_token)
    return s_unprotect_token(token);

  constexpr std::string_view prefix = "dpapi:";
  if (!token.starts_with(prefix))
    return std::string{token};

#ifdef _WIN32
  const auto decoded = Base64Url::Decode(token.substr(prefix.size()));
  if (!decoded)
    return {};

  DATA_BLOB input;
  input.cbData = static_cast<DWORD>(decoded->size());
  input.pbData = const_cast<BYTE*>(decoded->data());

  DATA_BLOB output{};
  if (!CryptUnprotectData(&input, nullptr, nullptr, nullptr, nullptr, CRYPTPROTECT_UI_FORBIDDEN,
                          &output))
  {
    return {};
  }

  std::string result(reinterpret_cast<const char*>(output.pbData), output.cbData);
  LocalFree(output.pbData);
  return result;
#else
  return {};
#endif
}

std::string SerializeSettings(const Settings& settings)
{
  picojson::object object;
  object.emplace("version", picojson::value(1.0));
  object.emplace("enabled", picojson::value(settings.enabled));
  object.emplace("auto_sync", picojson::value(settings.auto_sync));
  object.emplace("sync_states", picojson::value(settings.sync_states));
  object.emplace("client_id", picojson::value(settings.client_id));
  object.emplace("root_path", picojson::value(settings.root_path));
  object.emplace("access_token", picojson::value(ProtectToken(settings.access_token)));
  object.emplace("refresh_token", picojson::value(ProtectToken(settings.refresh_token)));
  object.emplace("account_id", picojson::value(settings.account_id));
  if (settings.access_token_expires_at)
  {
    object.emplace("access_token_expires_at",
                   picojson::value(static_cast<double>(*settings.access_token_expires_at)));
  }

  return picojson::value(object).serialize(true);
}

std::optional<Settings> ParseSettings(std::string_view json, std::string* error)
{
  picojson::value value;
  const std::string parse_error = picojson::parse(value, std::string{json});
  if (!parse_error.empty() || !value.is<picojson::object>())
  {
    if (error)
      *error = parse_error.empty() ? "Settings is not a JSON object" : parse_error;
    return std::nullopt;
  }

  const auto& object = value.get<picojson::object>();
  Settings settings;
  settings.enabled = GetBool(object, "enabled", false);
  settings.auto_sync = GetBool(object, "auto_sync", true);
  settings.sync_states = GetBool(object, "sync_states", settings.sync_states);
  settings.client_id = GetString(object, "client_id");
  settings.root_path = GetString(object, "root_path");
  if (settings.root_path.empty())
    settings.root_path = "/DolphinFork";
  settings.access_token = UnprotectToken(GetString(object, "access_token"));
  settings.refresh_token = UnprotectToken(GetString(object, "refresh_token"));
  settings.account_id = GetString(object, "account_id");
  settings.access_token_expires_at = GetU64(object, "access_token_expires_at");
  return settings;
}

std::optional<Settings> LoadSettings(const std::string& path, std::string* error)
{
  std::string contents;
  if (!File::ReadFileToString(path, contents))
  {
    if (error)
      *error = "Unable to read CloudSync settings";
    return std::nullopt;
  }

  return ParseSettings(contents, error);
}

bool SaveSettings(const std::string& path, const Settings& settings, std::string* error)
{
  File::CreateFullPath(path);
  if (!File::WriteStringToFile(path, SerializeSettings(settings)))
  {
    if (error)
      *error = "Unable to write CloudSync settings";
    return false;
  }

  return true;
}
}  // namespace CloudSync