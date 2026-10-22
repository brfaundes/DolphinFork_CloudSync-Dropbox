// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#include "jni/CloudSyncAndroid.h"

#include <mutex>
#include <optional>
#include <string>
#include <string_view>

#include <fmt/format.h>
#include <picojson.h>

#include "Common/Logging/Log.h"
#include "Core/Boot/Boot.h"
#include "UICommon/CloudSync/Auth.h"
#include "UICommon/CloudSync/Service.h"
#include "UICommon/CloudSync/Settings.h"

#include "jni/AndroidCommon/AndroidCommon.h"
#include "jni/AndroidCommon/IDCache.h"

namespace CloudSyncAndroid
{
namespace
{
constexpr char TOKEN_STORE_CLASS[] = "org/dolphinemu/dolphinemu/CloudSyncTokenStore";
constexpr char REDIRECT_URI[] = "http://127.0.0.1:53682/";

JavaVM* s_java_vm;
jclass s_token_store_class;
jmethodID s_protect_token_method;
jmethodID s_unprotect_token_method;
std::once_flag s_initialize_once;
std::mutex s_mutex;
std::optional<CloudSync::GameIdentity> s_game_identity;
std::string s_last_result = "{}";

const char* GetStatusName(CloudSync::SyncStatus status)
{
  switch (status)
  {
  case CloudSync::SyncStatus::Success:
    return "success";
  case CloudSync::SyncStatus::Disabled:
    return "disabled";
  case CloudSync::SyncStatus::NotConfigured:
    return "not_configured";
  case CloudSync::SyncStatus::Offline:
    return "offline";
  case CloudSync::SyncStatus::Conflict:
    return "conflict";
  case CloudSync::SyncStatus::Error:
  default:
    return "error";
  }
}

std::string ResultToJson(const CloudSync::SyncResult& result)
{
  picojson::object object;
  object.emplace("status", picojson::value(GetStatusName(result.status)));
  object.emplace("message", picojson::value(result.message));

  picojson::array uploaded;
  for (const std::string& name : result.uploaded)
    uploaded.emplace_back(name);
  object.emplace("uploaded", picojson::value(uploaded));

  picojson::array downloaded;
  for (const std::string& name : result.downloaded)
    downloaded.emplace_back(name);
  object.emplace("downloaded", picojson::value(downloaded));

  picojson::array conflicts;
  for (const std::string& name : result.conflicts)
    conflicts.emplace_back(name);
  object.emplace("conflicts", picojson::value(conflicts));

  return picojson::value(object).serialize();
}

std::string SettingsToJson(const CloudSync::Settings& settings, bool connected)
{
  picojson::object object;
  object.emplace("enabled", picojson::value(settings.enabled));
  object.emplace("auto_sync", picojson::value(settings.auto_sync));
  object.emplace("sync_states", picojson::value(settings.sync_states));
  object.emplace("root_path", picojson::value(settings.root_path));
  object.emplace("connected", picojson::value(connected));
  return picojson::value(object).serialize();
}

std::string CallTokenStore(JNIEnv* env, jmethodID method, std::string_view value)
{
  if (!s_token_store_class || !method)
    return {};

  jstring argument = ToJString(env, value);
  auto result = static_cast<jstring>(
      env->CallStaticObjectMethod(s_token_store_class, method, argument));
  env->DeleteLocalRef(argument);
  if (env->ExceptionCheck())
  {
    env->ExceptionDescribe();
    env->ExceptionClear();
    return {};
  }
  if (!result)
    return {};

  std::string converted = GetJString(env, result);
  env->DeleteLocalRef(result);
  return converted;
}

JNIEnv* GetEnv(bool* attached)
{
  *attached = false;
  if (!s_java_vm)
    return nullptr;

  JNIEnv* env = nullptr;
  const jint status = s_java_vm->GetEnv(reinterpret_cast<void**>(&env), JNI_VERSION_1_6);
  if (status == JNI_EDETACHED)
  {
    if (s_java_vm->AttachCurrentThread(&env, nullptr) != JNI_OK)
      return nullptr;
    *attached = true;
  }
  else if (status != JNI_OK)
  {
    return nullptr;
  }
  return env;
}

std::string ProtectTokenOnAndroid(std::string_view token)
{
  bool attached = false;
  JNIEnv* env = GetEnv(&attached);
  if (!env)
    return {};
  std::string result = CallTokenStore(env, s_protect_token_method, token);
  if (attached)
    s_java_vm->DetachCurrentThread();
  return result;
}

std::string UnprotectTokenOnAndroid(std::string_view token)
{
  bool attached = false;
  JNIEnv* env = GetEnv(&attached);
  if (!env)
    return {};
  std::string result = CallTokenStore(env, s_unprotect_token_method, token);
  if (attached)
    s_java_vm->DetachCurrentThread();
  return result;
}

void ShowToast(JNIEnv* env, std::string_view message, bool long_duration)
{
  if (!env || message.empty())
    return;

  jstring text = ToJString(env, message);
  env->CallStaticVoidMethod(IDCache::GetNativeLibraryClass(), IDCache::GetDisplayToastMsg(), text,
                            long_duration ? JNI_TRUE : JNI_FALSE);
  env->DeleteLocalRef(text);
}

bool ShouldSyncAutomatically(CloudSync::Service& service)
{
  const CloudSync::Settings& settings = service.GetSettings();
  return settings.enabled && settings.auto_sync && service.HasAccount();
}

void ReportResult(JNIEnv* env, const CloudSync::SyncResult& result)
{
  s_last_result = ResultToJson(result);

  switch (result.status)
  {
  case CloudSync::SyncStatus::Success:
    if (!result.uploaded.empty() || !result.downloaded.empty())
    {
      ShowToast(env,
                fmt::format("Cloud sync: {} uploaded, {} downloaded", result.uploaded.size(),
                            result.downloaded.size()),
                false);
    }
    break;
  case CloudSync::SyncStatus::Disabled:
  case CloudSync::SyncStatus::NotConfigured:
    break;
  case CloudSync::SyncStatus::Conflict:
    ShowToast(env, "Cloud sync conflict: local and remote saves were kept", true);
    break;
  case CloudSync::SyncStatus::Offline:
    ShowToast(env, fmt::format("Cloud sync offline: {}", result.message), true);
    break;
  case CloudSync::SyncStatus::Error:
    ShowToast(env, fmt::format("Cloud sync failed: {}", result.message), true);
    break;
  }
}

CloudSync::Service& LoadService()
{
  CloudSync::Service& service = CloudSync::Service::GetInstance();
  service.LoadSettings();
  return service;
}
}  // namespace

void Initialize(JNIEnv* env)
{
  std::call_once(s_initialize_once, [env]() {
    env->GetJavaVM(&s_java_vm);
    jclass local_class = env->FindClass(TOKEN_STORE_CLASS);
    if (!local_class)
    {
      env->ExceptionClear();
      ERROR_LOG_FMT(COMMON, "Unable to find CloudSyncTokenStore");
      return;
    }

    s_token_store_class = static_cast<jclass>(env->NewGlobalRef(local_class));
    s_protect_token_method =
        env->GetStaticMethodID(s_token_store_class, "protect", "(Ljava/lang/String;)Ljava/lang/String;");
    s_unprotect_token_method = env->GetStaticMethodID(s_token_store_class, "unprotect",
                                                      "(Ljava/lang/String;)Ljava/lang/String;");
    env->DeleteLocalRef(local_class);

    if (!s_protect_token_method || !s_unprotect_token_method)
    {
      env->ExceptionClear();
      ERROR_LOG_FMT(COMMON, "Unable to find CloudSyncTokenStore methods");
      return;
    }

    CloudSync::SetTokenProtectionCallbacks(ProtectTokenOnAndroid, UnprotectTokenOnAndroid);
  });
}

void SynchronizeBeforeBoot(JNIEnv* env, const BootParameters& boot)
{
  auto identity = CloudSync::IdentifyGame(boot);
  if (!identity)
    return;

  std::lock_guard lock(s_mutex);
  s_game_identity = identity;

  CloudSync::Service& service = LoadService();
  if (!ShouldSyncAutomatically(service))
    return;

  ReportResult(env, service.Synchronize(*identity));
}

void SynchronizeAfterShutdown(JNIEnv* env)
{
  std::lock_guard lock(s_mutex);
  if (!s_game_identity)
    return;

  CloudSync::Service& service = LoadService();
  if (!ShouldSyncAutomatically(service))
    return;

  ReportResult(env, service.Synchronize(*s_game_identity));
}
}  // namespace CloudSyncAndroid

using namespace CloudSyncAndroid;

extern "C" {

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_CloudSyncBridge_nativeInitialize(JNIEnv* env, jclass)
{
  Initialize(env);
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_CloudSyncBridge_nativeGetState(JNIEnv* env, jclass)
{
  std::lock_guard lock(s_mutex);
  CloudSync::Service& service = LoadService();
  return ToJString(env, SettingsToJson(service.GetSettings(), service.HasAccount()));
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_CloudSyncBridge_nativeUpdateSettings(
    JNIEnv* env, jclass, jboolean enabled, jboolean auto_sync, jboolean sync_states,
    jstring root_path)
{
  std::lock_guard lock(s_mutex);
  CloudSync::Service& service = LoadService();
  CloudSync::Settings settings = service.GetSettings();
  settings.enabled = enabled == JNI_TRUE;
  settings.auto_sync = auto_sync == JNI_TRUE;
  settings.sync_states = sync_states == JNI_TRUE;
  settings.root_path = GetJString(env, root_path);
  if (settings.root_path.empty())
    settings.root_path = "/DolphinFork";
  service.SetSettings(std::move(settings));

  std::string error;
  if (!service.SaveSettings(&error))
    return ToJString(env, error);
  return ToJString(env, "");
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_CloudSyncBridge_nativeCreateAuthorization(JNIEnv* env, jclass)
{
  std::lock_guard lock(s_mutex);
  CloudSync::Service& service = LoadService();
  const CloudSync::Settings& settings = service.GetSettings();
  const std::string client_id =
      settings.client_id.empty() ? CloudSync::Auth::DEFAULT_APP_KEY : settings.client_id;

  const CloudSync::Auth::PkcePair pkce = CloudSync::Auth::GeneratePkce();
  const std::string state = CloudSync::Auth::GenerateState();
  const std::string url = CloudSync::Auth::BuildAuthorizationUrl(
      client_id, REDIRECT_URI, state, pkce.challenge);

  picojson::object object;
  object.emplace("url", picojson::value(url));
  object.emplace("state", picojson::value(state));
  object.emplace("verifier", picojson::value(pkce.verifier));
  return ToJString(env, picojson::value(object).serialize());
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_CloudSyncBridge_nativeCompleteAuthorization(
    JNIEnv* env, jclass, jstring code, jstring verifier)
{
  std::lock_guard lock(s_mutex);
  CloudSync::Service& service = LoadService();
  const CloudSync::Settings& settings = service.GetSettings();
  const std::string client_id =
      settings.client_id.empty() ? CloudSync::Auth::DEFAULT_APP_KEY : settings.client_id;

  std::string error;
  const auto response =
      CloudSync::Auth::ExchangeCode(client_id, REDIRECT_URI, GetJString(env, code),
                                    GetJString(env, verifier), &error);
  if (!response)
    return ToJString(env, error);

  if (!service.CompleteAuthorization(*response, &error))
    return ToJString(env, error);
  return ToJString(env, "");
}

JNIEXPORT void JNICALL
Java_org_dolphinemu_dolphinemu_CloudSyncBridge_nativeDisconnect(JNIEnv*, jclass)
{
  std::lock_guard lock(s_mutex);
  LoadService().Disconnect();
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_CloudSyncBridge_nativeTestConnection(JNIEnv* env, jclass)
{
  std::lock_guard lock(s_mutex);
  std::string error;
  if (!LoadService().TestConnection(&error))
    return ToJString(env, error);
  return ToJString(env, "");
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_CloudSyncBridge_nativeSyncLastGame(JNIEnv* env, jclass)
{
  std::lock_guard lock(s_mutex);
  if (!s_game_identity)
  {
    CloudSync::SyncResult result;
    result.status = CloudSync::SyncStatus::Error;
    result.message = "Open a game before synchronizing";
    s_last_result = ResultToJson(result);
    return ToJString(env, s_last_result);
  }

  CloudSync::SyncResult result = LoadService().Synchronize(*s_game_identity);
  s_last_result = ResultToJson(result);
  return ToJString(env, s_last_result);
}

JNIEXPORT jstring JNICALL
Java_org_dolphinemu_dolphinemu_CloudSyncBridge_nativeGetLastResult(JNIEnv* env, jclass)
{
  std::lock_guard lock(s_mutex);
  return ToJString(env, s_last_result);
}
}
