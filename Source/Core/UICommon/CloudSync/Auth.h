// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <optional>
#include <string>
#include <string_view>

#include "Common/CommonTypes.h"

namespace CloudSync::Auth
{
inline constexpr char AUTHORIZATION_ENDPOINT[] = "https://www.dropbox.com/oauth2/authorize";
inline constexpr char TOKEN_ENDPOINT[] = "https://api.dropboxapi.com/oauth2/token";
inline constexpr char DEFAULT_APP_KEY[] = "x1o52ig4tdm7llj";
inline constexpr char SCOPES[] = "files.metadata.read files.content.read files.content.write";

struct PkcePair
{
  std::string verifier;
  std::string challenge;
};

struct AuthorizationResponse
{
  std::optional<std::string> code;
  std::optional<std::string> state;
  std::optional<std::string> error;
  std::optional<std::string> error_description;
};

struct TokenResponse
{
  std::string access_token;
  std::string refresh_token;
  std::string token_type;
  std::string scope;
  std::string account_id;
  std::optional<u64> expires_in;
};

PkcePair GeneratePkce();
std::string CreateCodeChallenge(std::string_view verifier);
std::string GenerateState();

std::string BuildAuthorizationUrl(std::string_view client_id, std::string_view redirect_uri,
                                  std::string_view state, std::string_view code_challenge,
                                  std::string_view scope = SCOPES);

AuthorizationResponse ParseAuthorizationResponse(std::string_view url);

std::optional<TokenResponse> ParseTokenResponse(std::string_view json, std::string* error);
std::optional<TokenResponse> ExchangeCode(std::string_view client_id, std::string_view redirect_uri,
                                          std::string_view code, std::string_view code_verifier,
                                          std::string* error);
std::optional<TokenResponse> RefreshAccessToken(std::string_view client_id,
                                                std::string_view refresh_token, std::string* error);

}  // namespace CloudSync::Auth
