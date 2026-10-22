// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <filesystem>
#include <fstream>

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "UICommon/CloudSync/Auth.h"
#include "UICommon/CloudSync/Base64Url.h"
#include "UICommon/CloudSync/Dropbox.h"
#include "UICommon/CloudSync/Manifest.h"
#include "UICommon/CloudSync/Service.h"
#include "UICommon/CloudSync/Settings.h"
#include "UICommon/CloudSync/SyncEngine.h"
#include "UICommon/CloudSync/Url.h"

using namespace CloudSync;

TEST(CloudSync, Base64UrlEncoding)
{
  const std::string input = "hello";
  EXPECT_EQ(Base64Url::Encode(input), "aGVsbG8");
  EXPECT_EQ(Base64Url::Encode(std::string{}), "");
  EXPECT_EQ(Base64Url::Encode(std::string{"a"}), "YQ");
  EXPECT_EQ(Base64Url::Encode(std::string{"ab"}), "YWI");
}

TEST(CloudSync, UrlEncoding)
{
  EXPECT_EQ(Url::Encode("a b+c/d"), "a%20b%2Bc%2Fd");
  EXPECT_EQ(Url::Decode("a%20b%2Bc%2Fd").value(), "a b+c/d");
  EXPECT_EQ(Url::Decode("a+b").value(), "a b");
  EXPECT_FALSE(Url::Decode("%zz").has_value());
  EXPECT_FALSE(Url::Decode("%2").has_value());
}

TEST(CloudSync, PkceMatchesRfc7636Example)
{
  constexpr char verifier[] = "dBjftJeZ4CVP-mB92K27uhbUJU1p1r_wW1gFWFOEjXk";
  EXPECT_EQ(Auth::CreateCodeChallenge(verifier), "E9Melhoa2OwvFrEMTJguCHaoeK1t8URWbuGJSstw-cM");

  const auto pair = Auth::GeneratePkce();
  EXPECT_EQ(pair.verifier.size(), 43u);
  EXPECT_EQ(pair.challenge, Auth::CreateCodeChallenge(pair.verifier));
}

TEST(CloudSync, DropboxAuthorizationUrl)
{
  const std::string url = Auth::BuildAuthorizationUrl(
      Auth::DEFAULT_APP_KEY, "http://127.0.0.1:1234/", "state", "challenge");
  EXPECT_EQ(url.rfind(Auth::AUTHORIZATION_ENDPOINT, 0), 0u);
  EXPECT_NE(url.find("client_id=x1o52ig4tdm7llj"), std::string::npos);
  EXPECT_NE(url.find("redirect_uri=http%3A%2F%2F127.0.0.1%3A1234%2F"), std::string::npos);
  EXPECT_NE(url.find("scope=files.metadata.read%20files.content.read%20files.content.write"),
            std::string::npos);
  EXPECT_NE(url.find("code_challenge=challenge"), std::string::npos);
  EXPECT_NE(url.find("code_challenge_method=S256"), std::string::npos);
  EXPECT_NE(url.find("token_access_type=offline"), std::string::npos);
}

TEST(CloudSync, ParseAuthorizationResponse)
{
  const auto response = Auth::ParseAuthorizationResponse(
      "http://127.0.0.1:1234/?code=abc%2F123&state=xyz&scope=scope");
  ASSERT_TRUE(response.code.has_value());
  ASSERT_TRUE(response.state.has_value());
  EXPECT_EQ(*response.code, "abc/123");
  EXPECT_EQ(*response.state, "xyz");
  EXPECT_FALSE(response.error.has_value());

  const auto error = Auth::ParseAuthorizationResponse("http://127.0.0.1:1234/?error=access_denied");
  ASSERT_TRUE(error.error.has_value());
  EXPECT_EQ(*error.error, "access_denied");
}

TEST(CloudSync, ParseTokenResponse)
{
  std::string error;
  const auto response = Auth::ParseTokenResponse(
      R"({"access_token":"token","refresh_token":"refresh","expires_in":14400,)"
      R"("token_type":"bearer","account_id":"dbid:123"})",
      &error);
  ASSERT_TRUE(response.has_value()) << error;
  EXPECT_EQ(response->access_token, "token");
  EXPECT_EQ(response->refresh_token, "refresh");
  EXPECT_EQ(response->account_id, "dbid:123");
  ASSERT_TRUE(response->expires_in.has_value());
  EXPECT_EQ(*response->expires_in, 14400u);

  error.clear();
  EXPECT_FALSE(Auth::ParseTokenResponse(R"({"error":"invalid_grant"})", &error).has_value());
  EXPECT_EQ(error, "invalid_grant");
}

TEST(CloudSync, DropboxPathNormalization)
{
  EXPECT_EQ(Dropbox::NormalizePath(""), "");
  EXPECT_EQ(Dropbox::NormalizePath("/"), "");
  EXPECT_EQ(Dropbox::NormalizePath("saves/game.sav"), "/saves/game.sav");
  EXPECT_EQ(Dropbox::NormalizePath("/saves/game.sav/"), "/saves/game.sav");
  EXPECT_EQ(Dropbox::NormalizePath("\\saves\\game.sav"), "/saves/game.sav");
  EXPECT_EQ(Dropbox::ParentPath("/saves/game.sav"), "/saves");
  EXPECT_EQ(Dropbox::ParentPath("/game.sav"), "");
}

TEST(CloudSync, ParseDropboxList)
{
  std::string error;
  const auto list = Dropbox::Client::ParseList(
      R"({"entries":[{".tag":"file","name":"save.sav","path_lower":"/game/save.sav",)"
      R"("path_display":"/game/save.sav","id":"id:abc","rev":"0123","size":1024,)"
      R"("content_hash":"4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358",)"
      R"("client_modified":"2026-10-21T00:00:00Z","server_modified":"2026-10-21T00:00:00Z"},)"
      R"({".tag":"folder","name":"game","path_lower":"/game","path_display":"/game",)"
      R"("id":"id:def"}],"cursor":"cursor-1","has_more":true})",
      &error);
  ASSERT_TRUE(list.has_value()) << error;
  ASSERT_EQ(list->entries.size(), 2u);
  EXPECT_TRUE(list->entries[0].IsFile());
  EXPECT_EQ(list->entries[0].path_display, "/game/save.sav");
  EXPECT_EQ(list->entries[0].rev, "0123");
  EXPECT_EQ(list->entries[0].size, 1024u);
  EXPECT_EQ(list->entries[0].content_hash,
            "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358");
  EXPECT_TRUE(list->entries[1].IsFolder());
  EXPECT_EQ(list->cursor, "cursor-1");
  EXPECT_TRUE(list->has_more);
}

TEST(CloudSync, DropboxContentHash)
{
  const auto path = std::filesystem::temp_directory_path() / "dolphin-cloudsync-hash-test.bin";

  {
    std::ofstream file(path, std::ios::binary);
    file << "abc";
  }

  std::string error;
  const auto hash = Dropbox::Client::ComputeContentHash(path.string(), &error);
  ASSERT_TRUE(hash.has_value()) << error;
  EXPECT_EQ(*hash, "4f8b42c22dd3729b519ba6f68d2da7cc5b2d606d05daed5ad5128cc03e6c6358");

  {
    std::ofstream file(path, std::ios::binary | std::ios::trunc);
  }

  error.clear();
  const auto empty_hash = Dropbox::Client::ComputeContentHash(path.string(), &error);
  ASSERT_TRUE(empty_hash.has_value()) << error;
  EXPECT_EQ(*empty_hash, "e3b0c44298fc1c149afbf4c8996fb92427ae41e4649b934ca495991b7852b855");

  std::filesystem::remove(path);
}

TEST(CloudSync, DirectoryScanReturnsFullPhysicalPaths)
{
  const auto directory =
      std::filesystem::temp_directory_path() / "dolphin cloudsync directory scan test";
  std::filesystem::remove_all(directory);
  std::filesystem::create_directories(directory);

  const auto file = directory / "GZ2E01.gci";
  {
    std::ofstream output(file, std::ios::binary);
    output << "save";
  }

  const File::FSTEntry tree = File::ScanDirectoryTree(directory.string(), false);
  ASSERT_EQ(tree.children.size(), 1u);
  EXPECT_EQ(tree.children[0].virtualName, "GZ2E01.gci");
  EXPECT_EQ(std::filesystem::path(tree.children[0].physicalName), file);
  EXPECT_TRUE(File::IsFile(tree.children[0].physicalName));

  std::filesystem::remove_all(directory);
}

TEST(CloudSync, ManifestRoundTrip)
{
  Manifest manifest;
  manifest.root_path = "";
  ManifestEntry entry;
  entry.key = "GC/GAME01";
  entry.kind = "gamecube";
  entry.remote_path = "/juegos/GC/GAME01/guardados/rev1/GAME01.gci";
  entry.remote_rev = "0123456789abcdef";
  entry.remote_hash = "remote-hash";
  entry.local_path = "C:/saves/Card A/GAME01.gci";
  entry.local_hash = "local-hash";
  entry.last_synced_hash = "remote-hash";
  entry.size = 1234;
  entry.pending_manual_upload = true;
  manifest.entries.emplace(entry.key, entry);

  std::string error;
  const auto parsed = ParseManifest(SerializeManifest(manifest), &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_EQ(parsed->version, 2);
  EXPECT_EQ(parsed->root_path, "");
  ASSERT_EQ(parsed->entries.size(), 1u);
  const auto& parsed_entry = parsed->entries.at("GC/GAME01");
  EXPECT_EQ(parsed_entry.kind, "gamecube");
  EXPECT_EQ(parsed_entry.remote_path, entry.remote_path);
  EXPECT_EQ(parsed_entry.remote_rev, entry.remote_rev);
  EXPECT_EQ(parsed_entry.local_path, entry.local_path);
  EXPECT_EQ(parsed_entry.size, 1234u);
  EXPECT_TRUE(parsed_entry.pending_manual_upload);
}

TEST(CloudSync, SyncDecisions)
{
  SyncInput input;
  EXPECT_EQ(DecideSync(input), SyncDecision::None);

  input.online = false;
  input.local_exists = true;
  EXPECT_EQ(DecideSync(input), SyncDecision::None);

  input.online = true;
  EXPECT_EQ(DecideSync(input), SyncDecision::Upload);

  input.local_hash = "local";
  input.remote_hash = "remote";
  EXPECT_EQ(DecideSync(input), SyncDecision::Conflict);

  input.last_synced_hash = "local";
  EXPECT_EQ(DecideSync(input), SyncDecision::Download);

  input.last_synced_hash = "remote";
  EXPECT_EQ(DecideSync(input), SyncDecision::Upload);

  input.pending_manual_upload = true;
  EXPECT_EQ(DecideSync(input), SyncDecision::UploadManual);

  input.last_synced_hash.reset();
  EXPECT_EQ(DecideSync(input), SyncDecision::Conflict);

  input.local_hash = "remote";
  EXPECT_EQ(DecideSync(input), SyncDecision::None);

  input.local_hash = "local";
  input.remote_hash = "remote";
  input.last_synced_hash = "local";
  EXPECT_EQ(DecideSync(input), SyncDecision::Download);

  input.local_hash = "local";
  input.remote_hash = "remote";
  input.last_synced_hash = "remote";
  EXPECT_EQ(DecideSync(input), SyncDecision::UploadManual);
}

TEST(CloudSync, Base64UrlDecoding)
{
  const auto hello = Base64Url::Decode("aGVsbG8");
  ASSERT_TRUE(hello.has_value());
  EXPECT_EQ(std::string(hello->begin(), hello->end()), "hello");
  EXPECT_EQ(Base64Url::Decode("aGVsbG8=").value(), *hello);
  EXPECT_FALSE(Base64Url::Decode("a").has_value());
  EXPECT_FALSE(Base64Url::Decode("!!!!").has_value());
}

TEST(CloudSync, SettingsRoundTrip)
{
  Settings settings;
  settings.enabled = true;
  settings.auto_sync = false;
  settings.sync_states = false;
  settings.client_id = Auth::DEFAULT_APP_KEY;
  settings.root_path = "/DolphinFork";
  settings.access_token = "access-token";
  settings.refresh_token = "refresh-token";
  settings.account_id = "dbid:123";
  settings.access_token_expires_at = 123456789;

  std::string error;
  const auto parsed = ParseSettings(SerializeSettings(settings), &error);
  ASSERT_TRUE(parsed.has_value()) << error;
  EXPECT_TRUE(parsed->enabled);
  EXPECT_FALSE(parsed->auto_sync);
  EXPECT_FALSE(parsed->sync_states);
  EXPECT_EQ(parsed->client_id, Auth::DEFAULT_APP_KEY);
  EXPECT_EQ(parsed->root_path, "/DolphinFork");
  EXPECT_EQ(parsed->access_token, "access-token");
  EXPECT_EQ(parsed->refresh_token, "refresh-token");
  EXPECT_EQ(parsed->account_id, "dbid:123");
  ASSERT_TRUE(parsed->access_token_expires_at.has_value());
  EXPECT_EQ(*parsed->access_token_expires_at, 123456789u);
}

TEST(CloudSync, TokenProtectionRoundTrip)
{
  const std::string token = "sensitive-refresh-token";
  const std::string protected_token = ProtectToken(token);
  EXPECT_EQ(UnprotectToken(protected_token), token);
  EXPECT_EQ(UnprotectToken(""), "");
}

TEST(CloudSync, CustomTokenProtection)
{
  SetTokenProtectionCallbacks(
      +[](std::string_view token) { return "test:" + std::string(token); },
      +[](std::string_view token) -> std::string {
        constexpr std::string_view prefix = "test:";
        return token.starts_with(prefix) ? std::string(token.substr(prefix.size())) : std::string{};
      });

  Settings settings;
  settings.access_token = "access";
  settings.refresh_token = "refresh";
  const std::string serialized = SerializeSettings(settings);
  EXPECT_NE(serialized.find("test:access"), std::string::npos);
  EXPECT_NE(serialized.find("test:refresh"), std::string::npos);

  const auto parsed = ParseSettings(serialized, nullptr);
  ASSERT_TRUE(parsed.has_value());
  EXPECT_EQ(parsed->access_token, "access");
  EXPECT_EQ(parsed->refresh_token, "refresh");

  SetTokenProtectionCallbacks(nullptr, nullptr);
}

TEST(CloudSync, GameIdentityPaths)
{
  GameIdentity identity;
  identity.game_id = "GALE01";
  identity.title_id = 0;
  identity.revision = 2;
  EXPECT_EQ(MakeGameKey(identity), "gc-GALE01");
  EXPECT_EQ(MakeRevisionKey(2), "r2");

  Settings settings;
  settings.root_path = "DolphinFork";
  const auto paths = GetRemotePaths(settings, identity);
  EXPECT_EQ(paths.saves, "/DolphinFork/juegos/gc-GALE01/guardados/r2");
  EXPECT_EQ(paths.states, "/DolphinFork/juegos/gc-GALE01/estados/GALE01/r2");

  identity.is_wii = true;
  identity.game_id = "RZDE01";
  EXPECT_EQ(MakeGameKey(identity), "wii-RZDE01");
}