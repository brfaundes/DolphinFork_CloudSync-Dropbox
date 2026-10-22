// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/CloudSync/Manifest.h"

#include <charconv>

#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/JsonUtil.h"

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

bool GetBool(const picojson::object& object, const char* key)
{
  const auto it = object.find(key);
  return it != object.end() && it->second.is<bool>() && it->second.get<bool>();
}

u64 GetU64(const picojson::object& object, const char* key)
{
  const auto it = object.find(key);
  if (it == object.end())
    return 0;
  if (it->second.is<double>())
    return static_cast<u64>(it->second.get<double>());
  if (it->second.is<std::string>())
  {
    const std::string& text = it->second.get<std::string>();
    u64 value = 0;
    const auto result = std::from_chars(text.data(), text.data() + text.size(), value);
    if (result.ec == std::errc() && result.ptr == text.data() + text.size())
      return value;
  }
  return 0;
}

picojson::object EntryToJson(const ManifestEntry& entry)
{
  picojson::object object;
  object.emplace("key", picojson::value(entry.key));
  object.emplace("kind", picojson::value(entry.kind));
  object.emplace("remote_path", picojson::value(entry.remote_path));
  object.emplace("remote_rev", picojson::value(entry.remote_rev));
  object.emplace("remote_hash", picojson::value(entry.remote_hash));
  object.emplace("local_path", picojson::value(entry.local_path));
  object.emplace("local_hash", picojson::value(entry.local_hash));
  object.emplace("last_synced_hash", picojson::value(entry.last_synced_hash));
  object.emplace("modified_time", picojson::value(entry.modified_time));
  object.emplace("size", picojson::value(static_cast<double>(entry.size)));
  object.emplace("pending_manual_upload", picojson::value(entry.pending_manual_upload));
  return object;
}

ManifestEntry EntryFromJson(const picojson::object& object)
{
  ManifestEntry entry;
  entry.key = GetString(object, "key");
  entry.kind = GetString(object, "kind");
  entry.remote_path = GetString(object, "remote_path");
  entry.remote_rev = GetString(object, "remote_rev");
  entry.remote_hash = GetString(object, "remote_hash");
  entry.local_path = GetString(object, "local_path");
  entry.local_hash = GetString(object, "local_hash");
  entry.last_synced_hash = GetString(object, "last_synced_hash");
  entry.modified_time = GetString(object, "modified_time");
  entry.size = GetU64(object, "size");
  entry.pending_manual_upload = GetBool(object, "pending_manual_upload");
  return entry;
}
}  // namespace

std::optional<Manifest> ParseManifest(std::string_view json, std::string* error)
{
  picojson::value value;
  const std::string parse_error = picojson::parse(value, std::string{json});
  if (!parse_error.empty() || !value.is<picojson::object>())
  {
    if (error)
      *error = parse_error.empty() ? "Manifest is not a JSON object" : parse_error;
    return std::nullopt;
  }

  const auto& object = value.get<picojson::object>();

  Manifest manifest;
  const auto version_it = object.find("version");
  if (version_it != object.end() && version_it->second.is<double>())
    manifest.version = static_cast<int>(version_it->second.get<double>());
  manifest.root_path = GetString(object, "root_path");

  const auto entries_it = object.find("entries");
  if (entries_it != object.end() && entries_it->second.is<picojson::object>())
  {
    for (const auto& [key, entry_value] : entries_it->second.get<picojson::object>())
    {
      if (!entry_value.is<picojson::object>())
        continue;

      ManifestEntry entry = EntryFromJson(entry_value.get<picojson::object>());
      if (entry.key.empty())
        entry.key = key;
      manifest.entries.emplace(key, std::move(entry));
    }
  }

  return manifest;
}

std::string SerializeManifest(const Manifest& manifest)
{
  picojson::object object;
  object.emplace("version", picojson::value(static_cast<double>(manifest.version)));
  object.emplace("root_path", picojson::value(manifest.root_path));

  picojson::object entries;
  for (const auto& [key, entry] : manifest.entries)
    entries.emplace(key, picojson::value(EntryToJson(entry)));
  object.emplace("entries", picojson::value(entries));

  return picojson::value(object).serialize(true);
}

std::optional<Manifest> LoadManifest(const std::string& path, std::string* error)
{
  std::string contents;
  if (!File::ReadFileToString(path, contents))
  {
    if (error)
      *error = "Unable to read manifest";
    return std::nullopt;
  }

  return ParseManifest(contents, error);
}

bool SaveManifest(const std::string& path, const Manifest& manifest, std::string* error)
{
  picojson::value value;
  const std::string parse_error = picojson::parse(value, SerializeManifest(manifest));
  if (!parse_error.empty())
  {
    if (error)
      *error = parse_error;
    return false;
  }

  File::CreateFullPath(path);
  if (!JsonToFile(path, value, true))
  {
    if (error)
      *error = "Unable to write manifest";
    return false;
  }

  return true;
}
}  // namespace CloudSync
