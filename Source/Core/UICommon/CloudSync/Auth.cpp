// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "UICommon/CloudSync/Auth.h"

#include <array>
#include <chrono>
#include <map>

#include <fmt/format.h>
#include <mbedtls/sha256.h>
#include <picojson.h>

#include "Common/HttpRequest.h"
#include "Common/Random.h"
#include "UICommon/CloudSync/Base64Url.h"
#include "UICommon/CloudSync/Url.h"

namespace CloudSync::Auth
{
namespace
{
std::optional<std::string> GetString(const picojson::object& object, const char* key)
{
  const auto it = object.find(key);
  if (it == object.end() || !it->second.is<std::string>())
    return std::nullopt;
  return it->second.get<std::string>();
}

std::optional<std::string> GetErrorFromResponse(std::string_view body)
{
  picojson::value value;
  const std::string error = picojson::parse(value, std::string{body});
  if (!error.empty() || !value.is<picojson::object>())
    return std::nullopt;

  const auto& object = value.get<picojson::object>();
  if (const auto description = GetString(object, "error_description"))
    return description;
  return GetString(object, "error");
}

std::optional<TokenResponse> ParseTokenJson(std::string_view json, std::string* error)
{
  picojson::value value;
  const std::string parse_error = picojson::parse(value, std::string{json});
  if (!parse_error.empty() || !value.is<picojson::object>())
  {
    if (error)
      *error = parse_error.empty() ? "Invalid token response" : parse_error;
    return std::nullopt;
  }

  const auto& object = value.get<picojson::object>();
  if (const auto response_error = GetString(object, "error"))
  {
    if (error)
      *error = GetErrorFromResponse(json).value_or(*response_error);
    return std::nullopt;
  }

  TokenResponse response;
  const auto access_token = GetString(object, "access_token");
  if (!access_token)
  {
    if (error)
      *error = "Token response is missing access_token";
    return std::nullopt;
  }

  response.access_token = *access_token;
  response.refresh_token = GetString(object, "refresh_token").value_or("");
  response.token_type = GetString(object, "token_type").value_or("Bearer");
  response.scope = GetString(object, "scope").value_or("");
  response.account_id = GetString(object, "account_id").value_or("");

  const auto expires_it = object.find("expires_in");
  if (expires_it != object.end())
  {
    if (expires_it->second.is<double>())
      response.expires_in = static_cast<u64>(expires_it->second.get<double>());
    else if (expires_it->second.is<std::string>())
      response.expires_in = std::stoull(expires_it->second.get<std::string>());
  }

  return response;
}

std::optional<TokenResponse> RequestToken(const std::map<std::string, std::string>& parameters,
                                          std::string* error)
{
  std::string body;
  for (const auto& [key, value] : parameters)
  {
    if (!body.empty())
      body.push_back('&');
    body += Url::Encode(key);
    body.push_back('=');
    body += Url::Encode(value);
  }

  Common::HttpRequest request{std::chrono::seconds{30}};
  const auto response =
      request.Post(TOKEN_ENDPOINT, body, {{"Content-Type", "application/x-www-form-urlencoded"}},
                   Common::HttpRequest::AllowedReturnCodes::All);
  if (!response)
  {
    if (error)
      *error = "No response from the token endpoint";
    return std::nullopt;
  }

  const std::string_view response_body(reinterpret_cast<const char*>(response->data()),
                                       response->size());
  auto token = ParseTokenJson(response_body, error);
  if (!token && error && error->empty())
  {
    *error = fmt::format("Token endpoint returned HTTP {}", request.GetLastResponseCode());
  }

  return token;
}
}  // namespace

PkcePair GeneratePkce()
{
  std::array<u8, 32> random_bytes{};
  Common::Random::Generate(random_bytes.data(), random_bytes.size());

  PkcePair pair;
  pair.verifier = Base64Url::Encode(random_bytes);

  pair.challenge = CreateCodeChallenge(pair.verifier);

  return pair;
}

std::string CreateCodeChallenge(std::string_view verifier)
{
  std::array<u8, 32> digest{};
  mbedtls_sha256_ret(reinterpret_cast<const u8*>(verifier.data()), verifier.size(), digest.data(),
                     0);
  return Base64Url::Encode(digest);
}

std::string GenerateState()
{
  std::array<u8, 16> random_bytes{};
  Common::Random::Generate(random_bytes.data(), random_bytes.size());
  return Base64Url::Encode(random_bytes);
}

std::string BuildAuthorizationUrl(std::string_view client_id, std::string_view redirect_uri,
                                  std::string_view state, std::string_view code_challenge,
                                  std::string_view scope)
{
  return fmt::format("{}?client_id={}&redirect_uri={}&response_type=code&scope={}&state={}"
                     "&code_challenge={}&code_challenge_method=S256&token_access_type=offline",
                     AUTHORIZATION_ENDPOINT, Url::Encode(client_id), Url::Encode(redirect_uri),
                     Url::Encode(scope), Url::Encode(state), Url::Encode(code_challenge));
}

AuthorizationResponse ParseAuthorizationResponse(std::string_view url)
{
  AuthorizationResponse response;

  const size_t query_start = url.find('?');
  if (query_start == std::string_view::npos)
    return response;

  std::string_view query = url.substr(query_start + 1);
  if (const size_t fragment_start = query.find('#'); fragment_start != std::string_view::npos)
    query = query.substr(0, fragment_start);

  size_t position = 0;
  while (position < query.size())
  {
    const size_t separator = query.find('&', position);
    const std::string_view pair =
        query.substr(position, separator == std::string_view::npos ? query.size() - position :
                                                                     separator - position);

    const size_t equals = pair.find('=');
    if (equals != std::string_view::npos)
    {
      const auto key = Url::Decode(pair.substr(0, equals));
      const auto value = Url::Decode(pair.substr(equals + 1));
      if (key && value)
      {
        if (*key == "code")
          response.code = *value;
        else if (*key == "state")
          response.state = *value;
        else if (*key == "error")
          response.error = *value;
        else if (*key == "error_description")
          response.error_description = *value;
      }
    }

    if (separator == std::string_view::npos)
      break;
    position = separator + 1;
  }

  return response;
}

std::optional<TokenResponse> ParseTokenResponse(std::string_view json, std::string* error)
{
  return ParseTokenJson(json, error);
}

std::optional<TokenResponse> ExchangeCode(std::string_view client_id, std::string_view redirect_uri,
                                          std::string_view code, std::string_view code_verifier,
                                          std::string* error)
{
  const std::map<std::string, std::string> parameters{
      {"client_id", std::string{client_id}},         {"code", std::string{code}},
      {"code_verifier", std::string{code_verifier}}, {"grant_type", "authorization_code"},
      {"redirect_uri", std::string{redirect_uri}},
  };

  return RequestToken(parameters, error);
}

std::optional<TokenResponse> RefreshAccessToken(std::string_view client_id,
                                                std::string_view refresh_token, std::string* error)
{
  const std::map<std::string, std::string> parameters{
      {"client_id", std::string{client_id}},
      {"refresh_token", std::string{refresh_token}},
      {"grant_type", "refresh_token"},
  };

  return RequestToken(parameters, error);
}
}  // namespace CloudSync::Auth
