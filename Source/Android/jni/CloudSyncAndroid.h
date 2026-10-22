// Copyright 2026 Dolphin Emulator Project
// SPDX-License-Identifier: GPL-2.0-or-later

#pragma once

#include <jni.h>

struct BootParameters;

namespace CloudSyncAndroid
{
void Initialize(JNIEnv* env);
void SynchronizeBeforeBoot(JNIEnv* env, const BootParameters& boot);
void SynchronizeAfterShutdown(JNIEnv* env);
}  // namespace CloudSyncAndroid
