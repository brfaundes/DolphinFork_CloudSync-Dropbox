// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu

import androidx.annotation.Keep
import org.json.JSONObject

@Keep
object CloudSyncBridge {
    const val REDIRECT_URI = "http://127.0.0.1:53682/"

    private var initialized = false

    fun initialize() {
        if (initialized) {
            return
        }
        nativeInitialize()
        initialized = true
    }

    fun getState(): JSONObject {
        initialize()
        return JSONObject(nativeGetState())
    }

    fun updateSettings(
        enabled: Boolean,
        autoSync: Boolean,
        syncStates: Boolean,
        rootPath: String
    ): String {
        initialize()
        return nativeUpdateSettings(enabled, autoSync, syncStates, rootPath)
    }

    fun createAuthorization(): JSONObject {
        initialize()
        return JSONObject(nativeCreateAuthorization())
    }

    fun completeAuthorization(code: String, verifier: String): String {
        initialize()
        return nativeCompleteAuthorization(code, verifier)
    }

    fun disconnect() {
        initialize()
        nativeDisconnect()
    }

    fun testConnection(): String {
        initialize()
        return nativeTestConnection()
    }

    fun syncLastGame(): JSONObject {
        initialize()
        return JSONObject(nativeSyncLastGame())
    }

    fun getLastResult(): JSONObject {
        initialize()
        return JSONObject(nativeGetLastResult())
    }

    private external fun nativeInitialize()

    private external fun nativeGetState(): String

    private external fun nativeUpdateSettings(
        enabled: Boolean,
        autoSync: Boolean,
        syncStates: Boolean,
        rootPath: String
    ): String

    private external fun nativeCreateAuthorization(): String

    private external fun nativeCompleteAuthorization(code: String, verifier: String): String

    private external fun nativeDisconnect()

    private external fun nativeTestConnection(): String

    private external fun nativeSyncLastGame(): String

    private external fun nativeGetLastResult(): String
}
