// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/CloudSync/Dropbox.h"

#include <array>
#include <charconv>
#include <chrono>
#include <cstdio>
#include <vector>

#include <fmt/format.h>
#include <mbedtls/sha256.h>
#include <picojson.h>

#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/IOFile.h"

namespace CloudSync::Dropbox
{
namespace
{
constexpr size_t CONTENT_HASH_BLOCK_SIZE = 4 * 1024 * 1024;

std::optional<picojson::object> ParseObject(std::string_view json, std::string* error)
{
  picojson::value value;
  const std::string parse_error = picojson::parse(value, std::string{json});
  if (!parse_error.empty() || !value.is<picojson::object>())
  {
    if (error)
      *error = parse_error.empty() ? "Dropbox returned a non-object JSON document" : parse_error;
    return std::nullopt;
  }

  return value.get<picojson::object>();
}

std::string GetString(const picojson::object& object, const char* key)
{
  const auto it = object.find(key);
  if (it == object.end() || !it->second.is<std::string>())
    return {};
  return it->second.get<std::string>();
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

bool GetBool(const picojson::object& object, const char* key)
{
  const auto it = object.find(key);
  return it != object.end() && it->second.is<bool>() && it->second.get<bool>();
}

Common::HttpRequest::Headers AuthorizationHeader(const std::string& access_token)
{
  return {{"Authorization", fmt::format("Bearer {}", access_token)}};
}

std::string ErrorFromResponse(const Common::HttpRequest& request,
                              const Common::HttpRequest::Response& response)
{
  if (!response || response->empty())
    return fmt::format("Dropbox request failed with HTTP {}", request.GetLastResponseCode());

  const std::string_view body(reinterpret_cast<const char*>(response->data()), response->size());
  picojson::value value;
  const std::string parse_error = picojson::parse(value, std::string{body});
  if (parse_error.empty() && value.is<picojson::object>())
  {
    const std::string summary = GetString(value.get<picojson::object>(), "error_summary");
    if (!summary.empty())
      return fmt::format("Dropbox request failed with HTTP {}: {}", request.GetLastResponseCode(),
                         summary);
  }

  return fmt::format("Dropbox request failed with HTTP {}: {}", request.GetLastResponseCode(),
                     body);
}

std::string HexEncode(const u8* data, size_t size)
{
  static constexpr char HEX[] = "0123456789abcdef";
  std::string result;
  result.resize(size * 2);
  for (size_t i = 0; i < size; ++i)
  {
    result[i * 2] = HEX[data[i] >> 4];
    result[i * 2 + 1] = HEX[data[i] & 0xf];
  }
  return result;
}
}  // namespace

std::string NormalizePath(std::string_view path)
{
  std::string result;
  result.reserve(path.size() + 1);

  for (const char character : path)
  {
    if (character == '\\')
      result.push_back('/');
    else
      result.push_back(character);
  }

  while (result.size() > 1 && result.back() == '/')
    result.pop_back();

  if (result.empty() || result == "/")
    return {};

  if (result.front() != '/')
    result.insert(result.begin(), '/');

  return result;
}

std::string ParentPath(std::string_view path)
{
  const std::string normalized = NormalizePath(path);
  const size_t slash = normalized.rfind('/');
  if (slash == std::string::npos || slash == 0)
    return {};
  return normalized.substr(0, slash);
}

Client::Client(std::string access_token) : m_access_token(std::move(access_token))
{
}

void Client::SetAccessToken(std::string access_token)
{
  m_access_token = std::move(access_token);
}

std::optional<std::vector<u8>> Client::Rpc(std::string_view endpoint, std::string_view body,
                                           std::string* error)
{
  Common::HttpRequest request{std::chrono::seconds{60}};
  auto headers = AuthorizationHeader(m_access_token);
  headers.emplace("Content-Type", "application/json");

  const std::string url = fmt::format("{}/{}", API_BASE, endpoint);
  const auto response =
      request.Post(url, body, headers, Common::HttpRequest::AllowedReturnCodes::All);
  if (!response)
  {
    if (error)
      *error = ErrorFromResponse(request, response);
    return std::nullopt;
  }

  const s32 code = request.GetLastResponseCode();
  if (code < 200 || code >= 300)
  {
    if (error)
      *error = ErrorFromResponse(request, response);
    return std::nullopt;
  }

  return response;
}

std::optional<ListResult> Client::ListFolder(std::string_view path, std::string* error)
{
  picojson::object arguments;
  arguments.emplace("path", picojson::value(NormalizePath(path)));
  arguments.emplace("recursive", picojson::value(true));
  arguments.emplace("include_deleted", picojson::value(false));
  arguments.emplace("include_non_downloadable_files", picojson::value(false));
  arguments.emplace("limit", picojson::value(static_cast<double>(2000)));

  const auto response = Rpc("files/list_folder", picojson::value(arguments).serialize(), error);
  if (!response)
    return std::nullopt;

  return ParseList(
      std::string_view(reinterpret_cast<const char*>(response->data()), response->size()), error);
}

std::optional<ListResult> Client::ContinueList(std::string_view cursor, std::string* error)
{
  picojson::object arguments;
  arguments.emplace("cursor", picojson::value(std::string{cursor}));

  const auto response =
      Rpc("files/list_folder/continue", picojson::value(arguments).serialize(), error);
  if (!response)
    return std::nullopt;

  return ParseList(
      std::string_view(reinterpret_cast<const char*>(response->data()), response->size()), error);
}

std::optional<std::vector<Metadata>> Client::ListAll(std::string_view path, std::string* error)
{
  std::vector<Metadata> entries;

  auto page = ListFolder(path, error);
  if (!page)
    return std::nullopt;

  while (true)
  {
    entries.insert(entries.end(), page->entries.begin(), page->entries.end());

    if (!page->has_more)
      break;

    if (page->cursor.empty())
    {
      if (error)
        *error = "Dropbox returned has_more without a cursor";
      return std::nullopt;
    }

    page = ContinueList(page->cursor, error);
    if (!page)
      return std::nullopt;
  }

  return entries;
}

std::optional<Metadata> Client::GetMetadata(std::string_view path, std::string* error)
{
  picojson::object arguments;
  arguments.emplace("path", picojson::value(NormalizePath(path)));

  const auto response = Rpc("files/get_metadata", picojson::value(arguments).serialize(), error);
  if (!response)
    return std::nullopt;

  return ParseMetadata(
      std::string_view(reinterpret_cast<const char*>(response->data()), response->size()), error);
}

bool Client::CreateFolder(std::string_view path, std::string* error)
{
  picojson::object arguments;
  arguments.emplace("path", picojson::value(NormalizePath(path)));
  arguments.emplace("autorename", picojson::value(false));

  std::string local_error;
  const auto response =
      Rpc("files/create_folder_v2", picojson::value(arguments).serialize(), &local_error);
  if (response)
    return true;

  // Creating a folder that already exists is harmless. Dropbox reports this as a 409 conflict.
  if (local_error.find("HTTP 409") != std::string::npos ||
      local_error.find("conflict") != std::string::npos)
  {
    return true;
  }

  if (error)
    *error = std::move(local_error);
  return false;
}

bool Client::EnsureFolder(std::string_view path, std::string* error)
{
  const std::string normalized = NormalizePath(path);
  if (normalized.empty())
    return true;

  std::string current;
  size_t position = 1;
  while (position <= normalized.size())
  {
    const size_t slash = normalized.find('/', position);
    const size_t end = slash == std::string::npos ? normalized.size() : slash;
    current.push_back('/');
    current.append(normalized, position, end - position);

    if (!CreateFolder(current, error))
      return false;

    if (slash == std::string::npos)
      break;
    position = slash + 1;
  }

  return true;
}

std::optional<Metadata> Client::UploadFile(std::string_view path, const std::string& local_path,
                                           std::string* error)
{
  const std::string remote_path = NormalizePath(path);
  if (remote_path.empty())
  {
    if (error)
      *error = "Cannot upload to the Dropbox root";
    return std::nullopt;
  }

  File::IOFile file(local_path, "rb");
  if (!file.IsOpen())
  {
    if (error)
      *error = fmt::format("Unable to open '{}' for upload", local_path);
    return std::nullopt;
  }

  const u64 file_size = file.GetSize();
  file.Close();

  if (file_size > MAX_SIMPLE_UPLOAD_SIZE)
  {
    if (error)
      *error = fmt::format("File is too large for a simple Dropbox upload ({} bytes)", file_size);
    return std::nullopt;
  }

  if (!EnsureFolder(ParentPath(remote_path), error))
    return std::nullopt;

  picojson::object mode;
  mode.emplace(".tag", picojson::value("overwrite"));

  picojson::object arguments;
  arguments.emplace("path", picojson::value(remote_path));
  arguments.emplace("mode", picojson::value(mode));
  arguments.emplace("autorename", picojson::value(false));
  arguments.emplace("mute", picojson::value(false));
  arguments.emplace("strict_conflict", picojson::value(false));

  Common::HttpRequest request{std::chrono::seconds{120}};
  auto headers = AuthorizationHeader(m_access_token);
  headers.emplace("Content-Type", "application/octet-stream");
  headers.emplace("Dropbox-API-Arg", picojson::value(arguments).serialize());

  const auto response = request.PostFile(fmt::format("{}/files/upload", CONTENT_BASE), local_path,
                                         headers, Common::HttpRequest::AllowedReturnCodes::All2xx);
  if (!response)
  {
    if (error)
      *error = ErrorFromResponse(request, response);
    return std::nullopt;
  }

  return ParseMetadata(
      std::string_view(reinterpret_cast<const char*>(response->data()), response->size()), error);
}

bool Client::DownloadFile(std::string_view path, const std::string& local_path, std::string* error)
{
  const std::string remote_path = NormalizePath(path);
  if (remote_path.empty())
  {
    if (error)
      *error = "Cannot download from the Dropbox root";
    return false;
  }

  picojson::object arguments;
  arguments.emplace("path", picojson::value(remote_path));

  Common::HttpRequest request{std::chrono::seconds{180}};
  auto headers = AuthorizationHeader(m_access_token);
  headers.emplace("Dropbox-API-Arg", picojson::value(arguments).serialize());

  const std::string temporary_path = local_path + ".dropbox-part";
  File::Delete(temporary_path);

  const auto response =
      request.PostToFile(fmt::format("{}/files/download", CONTENT_BASE), temporary_path, headers,
                         Common::HttpRequest::AllowedReturnCodes::All2xx);
  if (!response)
  {
    if (error)
      *error = ErrorFromResponse(request, response);
    File::Delete(temporary_path);
    return false;
  }

  File::Delete(local_path);
  if (!File::Rename(temporary_path, local_path))
  {
    if (error)
      *error = fmt::format("Unable to move downloaded file into '{}'", local_path);
    return false;
  }

  return true;
}

bool Client::Delete(std::string_view path, std::string* error)
{
  picojson::object arguments;
  arguments.emplace("path", picojson::value(NormalizePath(path)));

  return Rpc("files/delete_v2", picojson::value(arguments).serialize(), error).has_value();
}

std::optional<Metadata> Client::ParseMetadata(std::string_view json, std::string* error)
{
  const auto object = ParseObject(json, error);
  if (!object)
    return std::nullopt;

  Metadata metadata;
  metadata.tag = GetString(*object, ".tag");
  metadata.name = GetString(*object, "name");
  metadata.path_lower = GetString(*object, "path_lower");
  metadata.path_display = GetString(*object, "path_display");
  metadata.id = GetString(*object, "id");
  metadata.rev = GetString(*object, "rev");
  metadata.content_hash = GetString(*object, "content_hash");
  metadata.client_modified = GetString(*object, "client_modified");
  metadata.server_modified = GetString(*object, "server_modified");
  metadata.size = GetU64(*object, "size");

  if (metadata.tag.empty())
    metadata.tag = metadata.id.empty() ? "folder" : "file";

  return metadata;
}

std::optional<ListResult> Client::ParseList(std::string_view json, std::string* error)
{
  const auto object = ParseObject(json, error);
  if (!object)
    return std::nullopt;

  ListResult result;
  result.cursor = GetString(*object, "cursor");
  result.has_more = GetBool(*object, "has_more");

  const auto entries_it = object->find("entries");
  if (entries_it == object->end() || !entries_it->second.is<picojson::array>())
    return result;

  for (const auto& value : entries_it->second.get<picojson::array>())
  {
    if (!value.is<picojson::object>())
      continue;

    if (auto metadata = ParseMetadata(picojson::value(value).serialize(), nullptr))
      result.entries.push_back(std::move(*metadata));
  }

  return result;
}

std::optional<std::string> Client::ComputeContentHash(const std::string& local_path,
                                                      std::string* error)
{
  File::IOFile file(local_path, "rb");
  if (!file.IsOpen())
  {
    if (error)
      *error = fmt::format("Unable to open '{}' for hashing", local_path);
    return std::nullopt;
  }

  std::vector<u8> block_hashes;
  std::vector<u8> buffer(CONTENT_HASH_BLOCK_SIZE);

  while (true)
  {
    const size_t read = std::fread(buffer.data(), 1, buffer.size(), file.GetHandle());
    if (read == 0)
    {
      if (std::ferror(file.GetHandle()))
      {
        if (error)
          *error = fmt::format("Unable to read '{}' for hashing", local_path);
        return std::nullopt;
      }
      break;
    }

    std::array<u8, 32> digest{};
    mbedtls_sha256_ret(buffer.data(), read, digest.data(), 0);
    block_hashes.insert(block_hashes.end(), digest.begin(), digest.end());

    if (read < buffer.size())
      break;
  }

  std::array<u8, 32> digest{};
  mbedtls_sha256_ret(block_hashes.data(), block_hashes.size(), digest.data(), 0);
  return HexEncode(digest.data(), digest.size());
}
}  // namespace CloudSync::Dropbox
