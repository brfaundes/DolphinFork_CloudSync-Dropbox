// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <functional>
#include <optional>
#include <utility>

#include <SFML/Network.hpp>

#include <QDialog>

#include "UICommon/CloudSync/Service.h"

class QCheckBox;
class QLabel;
class QPushButton;
class QTimer;

class CloudSyncDialog final : public QDialog
{
  Q_OBJECT
public:
  using IdentityProvider = std::function<std::optional<CloudSync::GameIdentity>()>;

  explicit CloudSyncDialog(QWidget* parent, IdentityProvider identity_provider);
  ~CloudSyncDialog() override;

private:
  static constexpr char REDIRECT_URI[] = "http://127.0.0.1:53682/";

  void CreateUI();
  void Refresh();
  void SaveOptions();

  void ConnectAccount();
  void DisconnectAccount();
  void TestConnection();
  void SyncNow();
  void UploadLocalSave();

  void StartAuthorization();
  void StopAuthorizationServer();
  void PollAuthorizationServer();
  void FinishAuthorization(const QString& target);

  void ShowSyncResult(const CloudSync::SyncResult& result);

  IdentityProvider m_identity_provider;

  QLabel* m_status = nullptr;
  QCheckBox* m_enabled = nullptr;
  QCheckBox* m_auto_sync = nullptr;
  QCheckBox* m_sync_states = nullptr;
  QPushButton* m_connect = nullptr;
  QPushButton* m_disconnect = nullptr;
  QPushButton* m_test = nullptr;
  QPushButton* m_sync = nullptr;
  QPushButton* m_upload = nullptr;

  sf::TcpListener* m_server = nullptr;
  QTimer* m_poll_timer = nullptr;
  std::string m_oauth_state;
  std::string m_oauth_verifier;
};