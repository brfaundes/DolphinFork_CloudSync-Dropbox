// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>

#ifdef _WIN32
#include <winsock2.h>
#else
#include <arpa/inet.h>
#include <netinet/in.h>
#include <sys/socket.h>
#include <unistd.h>
#endif

#include <gtest/gtest.h>

#include "Common/FileUtil.h"
#include "Common/HttpRequest.h"
#include "Common/SocketContext.h"

namespace
{
#ifdef _WIN32
using SocketHandle = SOCKET;
using SocketLength = int;
constexpr SocketHandle INVALID_SOCKET_HANDLE = INVALID_SOCKET;
#else
using SocketHandle = int;
using SocketLength = socklen_t;
constexpr SocketHandle INVALID_SOCKET_HANDLE = -1;
#endif

void CloseSocket(SocketHandle socket)
{
#ifdef _WIN32
  closesocket(socket);
#else
  close(socket);
#endif
}

struct ReceivedRequest
{
  std::string method;
  std::string body;
  std::optional<size_t> content_length;
  bool chunked = false;
};

std::string ToLower(std::string text)
{
  std::ranges::transform(text, text.begin(),
                         [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
  return text;
}

class TestHttpServer
{
public:
  explicit TestHttpServer(std::string response_body) : m_response_body(std::move(response_body))
  {
    m_listener = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
    if (m_listener == INVALID_SOCKET_HANDLE)
    {
      ADD_FAILURE() << "Failed to create test HTTP server socket";
      return;
    }

    int reuse_address = 1;
    setsockopt(m_listener, SOL_SOCKET, SO_REUSEADDR, reinterpret_cast<const char*>(&reuse_address),
               sizeof(reuse_address));

    sockaddr_in address{};
    address.sin_family = AF_INET;
    address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    address.sin_port = 0;
    if (bind(m_listener, reinterpret_cast<const sockaddr*>(&address), sizeof(address)) != 0 ||
        listen(m_listener, 1) != 0)
    {
      CloseSocket(m_listener);
      m_listener = INVALID_SOCKET_HANDLE;
      ADD_FAILURE() << "Failed to bind test HTTP server socket";
      return;
    }

    SocketLength address_size = sizeof(address);
    if (getsockname(m_listener, reinterpret_cast<sockaddr*>(&address), &address_size) != 0)
    {
      CloseSocket(m_listener);
      m_listener = INVALID_SOCKET_HANDLE;
      ADD_FAILURE() << "Failed to query test HTTP server port";
      return;
    }
    m_port = ntohs(address.sin_port);

    m_thread = std::thread([this] { HandleConnection(); });
  }

  ~TestHttpServer()
  {
    if (m_listener != INVALID_SOCKET_HANDLE)
    {
      CloseSocket(m_listener);
      m_listener = INVALID_SOCKET_HANDLE;
    }
    if (m_thread.joinable())
      m_thread.join();
  }

  std::string Url() const { return "http://127.0.0.1:" + std::to_string(m_port) + "/"; }

  ReceivedRequest WaitForRequest()
  {
    if (m_thread.joinable())
      m_thread.join();
    return m_request;
  }

private:
  void HandleConnection()
  {
    fd_set read_set;
    FD_ZERO(&read_set);
    FD_SET(m_listener, &read_set);
    timeval timeout{10, 0};
    if (select(static_cast<int>(m_listener) + 1, &read_set, nullptr, nullptr, &timeout) <= 0)
      return;

    const SocketHandle client = accept(m_listener, nullptr, nullptr);
    if (client == INVALID_SOCKET_HANDLE)
      return;

#ifdef _WIN32
    const DWORD receive_timeout_ms = 10000;
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, reinterpret_cast<const char*>(&receive_timeout_ms),
               sizeof(receive_timeout_ms));
#else
    timeval receive_timeout{10, 0};
    setsockopt(client, SOL_SOCKET, SO_RCVTIMEO, &receive_timeout, sizeof(receive_timeout));
#endif

    std::string request;
    char buffer[4096];
    while (request.find("\r\n\r\n") == std::string::npos)
    {
      const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
      if (received <= 0)
      {
        CloseSocket(client);
        return;
      }
      request.append(buffer, static_cast<size_t>(received));
    }

    const size_t header_end = request.find("\r\n\r\n");
    const std::string headers = request.substr(0, header_end);
    m_request.method = headers.substr(0, headers.find(' '));
    m_request.body = request.substr(header_end + 4);

    size_t content_length = 0;
    size_t line_start = headers.find("\r\n");
    while (line_start != std::string::npos && line_start + 2 < headers.size())
    {
      line_start += 2;
      const size_t line_end = headers.find("\r\n", line_start);
      const std::string line = headers.substr(line_start, line_end - line_start);
      const std::string lower = ToLower(line);
      if (lower.starts_with("content-length:"))
      {
        content_length = std::stoul(line.substr(std::strlen("content-length:")));
        m_request.content_length = content_length;
      }
      else if (lower.starts_with("transfer-encoding:") &&
               lower.find("chunked") != std::string::npos)
      {
        m_request.chunked = true;
      }

      if (line_end == std::string::npos)
        break;
      line_start = line_end;
    }

    while (!m_request.chunked && m_request.body.size() < content_length)
    {
      const int received = recv(client, buffer, static_cast<int>(sizeof(buffer)), 0);
      if (received <= 0)
        break;
      m_request.body.append(buffer, static_cast<size_t>(received));
    }

    const std::string response =
        "HTTP/1.1 200 OK\r\nContent-Length: " + std::to_string(m_response_body.size()) +
        "\r\nConnection: close\r\n\r\n" + m_response_body;
    size_t sent = 0;
    while (sent < response.size())
    {
      const int result =
          send(client, response.data() + sent, static_cast<int>(response.size() - sent), 0);
      if (result <= 0)
        break;
      sent += static_cast<size_t>(result);
    }

    CloseSocket(client);
  }

  SocketHandle m_listener = INVALID_SOCKET_HANDLE;
  uint16_t m_port = 0;
  std::string m_response_body;
  std::thread m_thread;
  ReceivedRequest m_request;
};

class HttpRequestTest : public testing::Test
{
protected:
  void SetUp() override
  {
    m_temp_directory = File::CreateTempDir();
    ASSERT_FALSE(m_temp_directory.empty());
  }

  void TearDown() override
  {
    if (!m_temp_directory.empty())
      File::DeleteDirRecursively(m_temp_directory);
  }

  Common::SocketContext m_socket_context;
  std::string m_temp_directory;
};
}  // namespace

TEST_F(HttpRequestTest, PostToFileWithEmptyBody)
{
  TestHttpServer server("downloaded");
  const std::string output_path = m_temp_directory + "/download.bin";

  Common::HttpRequest request{std::chrono::seconds{5}};
  const auto response = request.PostToFile(server.Url(), output_path, {},
                                           Common::HttpRequest::AllowedReturnCodes::All);
  const ReceivedRequest received = server.WaitForRequest();

  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(received.method, "POST");
  EXPECT_EQ(received.body, "");
  EXPECT_EQ(received.content_length, 0u);
  EXPECT_FALSE(received.chunked);

  std::string contents;
  ASSERT_TRUE(File::ReadFileToString(output_path, contents));
  EXPECT_EQ(contents, "downloaded");
}

TEST_F(HttpRequestTest, PostWithBody)
{
  TestHttpServer server("ok");

  Common::HttpRequest request{std::chrono::seconds{5}};
  const auto response = request.Post(server.Url(), std::string_view{"hello=world"}, {},
                                     Common::HttpRequest::AllowedReturnCodes::All);
  const ReceivedRequest received = server.WaitForRequest();

  ASSERT_TRUE(response.has_value());
  EXPECT_EQ(std::string(response->begin(), response->end()), "ok");
  EXPECT_EQ(received.method, "POST");
  EXPECT_EQ(received.body, "hello=world");
  EXPECT_EQ(received.content_length, std::string_view{"hello=world"}.size());
  EXPECT_FALSE(received.chunked);
}

TEST_F(HttpRequestTest, ReusedRequestStillSendsEmptyPostBody)
{
  TestHttpServer first_server("first");
  TestHttpServer second_server("second");
  const std::string output_path = m_temp_directory + "/download.bin";

  Common::HttpRequest request{std::chrono::seconds{5}};
  const auto first_response = request.Post(first_server.Url(), std::string_view{"first"}, {},
                                           Common::HttpRequest::AllowedReturnCodes::All);
  const ReceivedRequest first_received = first_server.WaitForRequest();
  const auto second_response = request.PostToFile(second_server.Url(), output_path, {},
                                                  Common::HttpRequest::AllowedReturnCodes::All);
  const ReceivedRequest second_received = second_server.WaitForRequest();

  ASSERT_TRUE(first_response.has_value());
  EXPECT_EQ(first_received.body, "first");
  ASSERT_TRUE(second_response.has_value());
  EXPECT_EQ(second_received.method, "POST");
  EXPECT_EQ(second_received.body, "");
  EXPECT_EQ(second_received.content_length, 0u);
  EXPECT_FALSE(second_received.chunked);

  std::string contents;
  ASSERT_TRUE(File::ReadFileToString(output_path, contents));
  EXPECT_EQ(contents, "second");
}
