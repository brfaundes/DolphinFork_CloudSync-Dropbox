// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "Common/CommonTypes.h"

namespace CloudSync::Dropbox
{
inline constexpr char API_BASE[] = "https://api.dropboxapi.com/2";
inline constexpr char CONTENT_BASE[] = "https://content.dropboxapi.com/2";
inline constexpr char REMOTE_ROOT[] = "";
inline constexpr u64 MAX_SIMPLE_UPLOAD_SIZE = 150ull * 1024 * 1024;

struct Metadata
{
  std::string tag;
  std::string name;
  std::string path_lower;
  std::string path_display;
  std::string id;
  std::string rev;
  std::string content_hash;
  std::string client_modified;
  std::string server_modified;
  u64 size = 0;

  bool IsFile() const { return tag == "file"; }
  bool IsFolder() const { return tag == "folder"; }
};

struct ListResult
{
  std::vector<Metadata> entries;
  std::string cursor;
  bool has_more = false;
};

std::string NormalizePath(std::string_view path);
std::string ParentPath(std::string_view path);

class Client
{
public:
  explicit Client(std::string access_token);

  void SetAccessToken(std::string access_token);

  std::optional<ListResult> ListFolder(std::string_view path, std::string* error);
  std::optional<ListResult> ContinueList(std::string_view cursor, std::string* error);
  std::optional<std::vector<Metadata>> ListAll(std::string_view path, std::string* error);

  std::optional<Metadata> GetMetadata(std::string_view path, std::string* error);
  bool CreateFolder(std::string_view path, std::string* error);
  bool EnsureFolder(std::string_view path, std::string* error);

  std::optional<Metadata> UploadFile(std::string_view path, const std::string& local_path,
                                     std::string* error);
  bool DownloadFile(std::string_view path, const std::string& local_path, std::string* error);
  bool Delete(std::string_view path, std::string* error);

  static std::optional<Metadata> ParseMetadata(std::string_view json, std::string* error);
  static std::optional<ListResult> ParseList(std::string_view json, std::string* error);
  static std::optional<std::string> ComputeContentHash(const std::string& local_path,
                                                       std::string* error);

private:
  std::optional<std::vector<u8>> Rpc(std::string_view endpoint, std::string_view body,
                                     std::string* error);

  std::string m_access_token;
};
}  // namespace CloudSync::Dropbox
