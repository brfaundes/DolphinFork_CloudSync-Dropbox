// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "DolphinQt/CloudSyncDialog.h"

#include <QApplication>
#include <QCheckBox>
#include <QDesktopServices>
#include <QDialogButtonBox>
#include <QGroupBox>
#include <QHBoxLayout>
#include <QLabel>
#include <QMessageBox>
#include <QPushButton>
#include <QSignalBlocker>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <array>
#include <sstream>

#include "UICommon/CloudSync/Auth.h"

CloudSyncDialog::CloudSyncDialog(QWidget* parent, IdentityProvider identity_provider)
    : QDialog(parent), m_identity_provider(std::move(identity_provider))
{
  setWindowTitle(tr("Cloud Sync"));
  setMinimumWidth(520);
  CreateUI();
  Refresh();
}

CloudSyncDialog::~CloudSyncDialog()
{
  StopAuthorizationServer();
}

void CloudSyncDialog::CreateUI()
{
  auto* layout = new QVBoxLayout(this);

  m_status = new QLabel;
  m_status->setWordWrap(true);
  layout->addWidget(m_status);

  auto* account_box = new QGroupBox(tr("Dropbox account"));
  auto* account_layout = new QHBoxLayout(account_box);
  m_connect = new QPushButton(tr("Connect..."));
  m_disconnect = new QPushButton(tr("Disconnect"));
  m_test = new QPushButton(tr("Test connection"));
  account_layout->addWidget(m_connect);
  account_layout->addWidget(m_disconnect);
  account_layout->addWidget(m_test);
  account_layout->addStretch();
  layout->addWidget(account_box);

  auto* options_box = new QGroupBox(tr("Options"));
  auto* options_layout = new QVBoxLayout(options_box);
  m_enabled = new QCheckBox(tr("Enable cloud sync"));
  m_auto_sync = new QCheckBox(tr("Synchronize automatically before and after each game"));
  m_sync_states = new QCheckBox(tr("Synchronize save states"));
  options_layout->addWidget(m_enabled);
  options_layout->addWidget(m_auto_sync);
  options_layout->addWidget(m_sync_states);
  layout->addWidget(options_box);

  auto* actions_box = new QGroupBox(tr("Actions"));
  auto* actions_layout = new QHBoxLayout(actions_box);
  m_sync = new QPushButton(tr("Sync now"));
  m_upload = new QPushButton(tr("Upload local save"));
  actions_layout->addWidget(m_sync);
  actions_layout->addWidget(m_upload);
  actions_layout->addStretch();
  layout->addWidget(actions_box);

  auto* buttons = new QDialogButtonBox(QDialogButtonBox::Close);
  connect(buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
  layout->addWidget(buttons);

  connect(m_connect, &QPushButton::clicked, this, &CloudSyncDialog::ConnectAccount);
  connect(m_disconnect, &QPushButton::clicked, this, &CloudSyncDialog::DisconnectAccount);
  connect(m_test, &QPushButton::clicked, this, &CloudSyncDialog::TestConnection);
  connect(m_sync, &QPushButton::clicked, this, &CloudSyncDialog::SyncNow);
  connect(m_upload, &QPushButton::clicked, this, &CloudSyncDialog::UploadLocalSave);
  connect(m_enabled, &QCheckBox::toggled, this, &CloudSyncDialog::SaveOptions);
  connect(m_auto_sync, &QCheckBox::toggled, this, &CloudSyncDialog::SaveOptions);
  connect(m_sync_states, &QCheckBox::toggled, this, &CloudSyncDialog::SaveOptions);
}

void CloudSyncDialog::Refresh()
{
  auto& service = CloudSync::Service::GetInstance();
  const auto& settings = service.GetSettings();
  const bool connected = service.HasAccount();

  if (connected)
  {
    m_status->setText(tr("Connected to Dropbox account %1.")
                          .arg(QString::fromStdString(settings.account_id)));
  }
  else
  {
    m_status->setText(tr("Not connected to Dropbox. Connect an account to start syncing."));
  }

  const QSignalBlocker enabled_blocker(m_enabled);
  const QSignalBlocker auto_blocker(m_auto_sync);
  const QSignalBlocker states_blocker(m_sync_states);
  m_enabled->setChecked(settings.enabled);
  m_auto_sync->setChecked(settings.auto_sync);
  m_sync_states->setChecked(settings.sync_states);

  m_connect->setEnabled(!connected);
  m_disconnect->setEnabled(connected);
  m_test->setEnabled(connected);
  m_sync->setEnabled(connected);
  m_upload->setEnabled(connected);
}

void CloudSyncDialog::SaveOptions()
{
  auto& service = CloudSync::Service::GetInstance();
  auto settings = service.GetSettings();
  settings.enabled = m_enabled->isChecked();
  settings.auto_sync = m_auto_sync->isChecked();
  settings.sync_states = m_sync_states->isChecked();
  service.SetSettings(std::move(settings));

  std::string error;
  if (!service.SaveSettings(&error))
  {
    QMessageBox::warning(this, tr("Cloud Sync"),
                         tr("Unable to save settings: %1").arg(QString::fromStdString(error)));
  }
}

void CloudSyncDialog::ConnectAccount()
{
  StartAuthorization();
}

void CloudSyncDialog::StartAuthorization()
{
  if (m_server)
    return;

  const auto pkce = CloudSync::Auth::GeneratePkce();
  m_oauth_verifier = pkce.verifier;
  m_oauth_state = CloudSync::Auth::GenerateState();

  m_server = new sf::TcpListener;
  m_server->setBlocking(false);
  if (m_server->listen(53682, sf::IpAddress::LocalHost) != sf::Socket::Status::Done)
  {
    delete m_server;
    m_server = nullptr;
    QMessageBox::critical(this, tr("Cloud Sync"),
                          tr("Unable to listen on port 53682 for the Dropbox authorization "
                             "callback. Close any program using this port and try again."));
    return;
  }

  m_poll_timer = new QTimer(this);
  m_poll_timer->setInterval(100);
  connect(m_poll_timer, &QTimer::timeout, this, &CloudSyncDialog::PollAuthorizationServer);
  m_poll_timer->start();

  const std::string url =
      CloudSync::Auth::BuildAuthorizationUrl(CloudSync::Auth::DEFAULT_APP_KEY, REDIRECT_URI,
                                             m_oauth_state, pkce.challenge);
  QDesktopServices::openUrl(QUrl(QString::fromStdString(url)));
  m_status->setText(tr("Waiting for authorization in your browser..."));
}

void CloudSyncDialog::StopAuthorizationServer()
{
  if (m_poll_timer)
  {
    m_poll_timer->stop();
    m_poll_timer->deleteLater();
    m_poll_timer = nullptr;
  }

  if (m_server)
  {
    m_server->close();
    delete m_server;
    m_server = nullptr;
  }
}

void CloudSyncDialog::PollAuthorizationServer()
{
  if (!m_server)
    return;

  sf::TcpSocket socket;
  if (m_server->accept(socket) != sf::Socket::Status::Done)
    return;

  StopAuthorizationServer();
  socket.setBlocking(true);

  std::string request;
  std::array<char, 4096> buffer{};
  while (request.size() < 16384 && request.find("\r\n\r\n") == std::string::npos)
  {
    std::size_t received = 0;
    if (socket.receive(buffer.data(), buffer.size(), received) != sf::Socket::Status::Done)
      break;
    request.append(buffer.data(), received);
  }

  const std::string response =
      "HTTP/1.1 200 OK\r\nContent-Type: text/html; charset=utf-8\r\nConnection: close\r\n\r\n"
      "<!doctype html><html><head><meta charset=\"utf-8\"><title>Dolphin Cloud Sync</title></head>"
      "<body style=\"font-family:sans-serif;padding:2em\"><h2>Dolphin Cloud Sync</h2>"
      "<p>Authorization received. You can close this tab and return to Dolphin.</p></body></html>";
  const auto send_status = socket.send(response.data(), response.size());
  (void)send_status;
  socket.disconnect();

  const auto line_end = request.find("\r\n");
  std::istringstream first_line(request.substr(0, line_end));
  std::string method;
  std::string target;
  first_line >> method >> target;
  if (method != "GET" || target.empty())
    return;

  FinishAuthorization(QString::fromStdString(target));
}

void CloudSyncDialog::FinishAuthorization(const QString& target)
{
  const std::string target_string = target.toStdString();
  std::string url;
  if (target_string.starts_with("http://") || target_string.starts_with("https://"))
  {
    url = target_string;
  }
  else
  {
    url = std::string(REDIRECT_URI);
    if (!target_string.empty() && target_string.front() == '/')
      url.pop_back();
    url += target_string;
  }
  const auto response = CloudSync::Auth::ParseAuthorizationResponse(url);

  if (response.error)
  {
    QMessageBox::warning(this, tr("Cloud Sync"),
                         tr("Dropbox authorization failed: %1")
                             .arg(QString::fromStdString(response.error_description.value_or(
                                 *response.error))));
    Refresh();
    return;
  }

  if (!response.code || !response.state || *response.state != m_oauth_state)
  {
    QMessageBox::warning(this, tr("Cloud Sync"),
                         tr("Dropbox returned an invalid authorization response."));
    Refresh();
    return;
  }

  std::string error;
  auto token = CloudSync::Auth::ExchangeCode(CloudSync::Auth::DEFAULT_APP_KEY, REDIRECT_URI,
                                             *response.code, m_oauth_verifier, &error);
  if (!token)
  {
    QMessageBox::warning(this, tr("Cloud Sync"),
                         tr("Unable to exchange the authorization code: %1")
                             .arg(QString::fromStdString(error)));
    Refresh();
    return;
  }

  if (!CloudSync::Service::GetInstance().CompleteAuthorization(*token, &error))
  {
    QMessageBox::warning(this, tr("Cloud Sync"),
                         tr("Unable to save the Dropbox account: %1")
                             .arg(QString::fromStdString(error)));
    Refresh();
    return;
  }

  QMessageBox::information(this, tr("Cloud Sync"), tr("Dropbox account connected."));
  Refresh();
}

void CloudSyncDialog::DisconnectAccount()
{
  if (QMessageBox::question(this, tr("Cloud Sync"),
                            tr("Disconnect this Dropbox account? Automatic sync will stop.")) !=
      QMessageBox::Yes)
  {
    return;
  }

  CloudSync::Service::GetInstance().Disconnect();
  Refresh();
}

void CloudSyncDialog::TestConnection()
{
  QApplication::setOverrideCursor(Qt::WaitCursor);
  std::string error;
  const bool ok = CloudSync::Service::GetInstance().TestConnection(&error);
  QApplication::restoreOverrideCursor();

  if (ok)
    QMessageBox::information(this, tr("Cloud Sync"), tr("Connection successful."));
  else
    QMessageBox::warning(this, tr("Cloud Sync"),
                         tr("Connection failed: %1").arg(QString::fromStdString(error)));
  Refresh();
}

void CloudSyncDialog::SyncNow()
{
  if (!m_identity_provider)
    return;
  const auto identity = m_identity_provider();
  if (!identity)
  {
    QMessageBox::information(this, tr("Cloud Sync"),
                             tr("Start a game first, then synchronize its save data."));
    return;
  }

  QApplication::setOverrideCursor(Qt::WaitCursor);
  const auto result = CloudSync::Service::GetInstance().Synchronize(*identity);
  QApplication::restoreOverrideCursor();
  ShowSyncResult(result);
}

void CloudSyncDialog::UploadLocalSave()
{
  if (!m_identity_provider)
    return;
  const auto identity = m_identity_provider();
  if (!identity)
  {
    QMessageBox::information(this, tr("Cloud Sync"),
                             tr("Start a game first, then upload its local save."));
    return;
  }

  if (QMessageBox::question(this, tr("Cloud Sync"),
                            tr("Upload the local save to Dropbox and overwrite the cloud copy? "
                               "The previous cloud copy will be kept in the conflicts folder.")) !=
      QMessageBox::Yes)
  {
    return;
  }

  QApplication::setOverrideCursor(Qt::WaitCursor);
  const auto result = CloudSync::Service::GetInstance().UploadLocalSave(*identity);
  QApplication::restoreOverrideCursor();
  ShowSyncResult(result);
}

void CloudSyncDialog::ShowSyncResult(const CloudSync::SyncResult& result)
{
  const QString message = QString::fromStdString(result.message);
  if (result.status == CloudSync::SyncStatus::Conflict)
  {
    QMessageBox::warning(this, tr("Cloud Sync"), message);
  }
  else if (result.status == CloudSync::SyncStatus::Error ||
           result.status == CloudSync::SyncStatus::Offline ||
           result.status == CloudSync::SyncStatus::NotConfigured)
  {
    QMessageBox::warning(this, tr("Cloud Sync"), message);
  }
  else
  {
    QMessageBox::information(this, tr("Cloud Sync"), message);
  }

  Refresh();
}