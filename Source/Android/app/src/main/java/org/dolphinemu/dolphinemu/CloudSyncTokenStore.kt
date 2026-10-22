// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu

import android.security.keystore.KeyGenParameterSpec
import android.security.keystore.KeyProperties
import android.util.Base64
import android.util.Log
import androidx.annotation.Keep
import java.nio.ByteBuffer
import java.security.KeyStore
import javax.crypto.Cipher
import javax.crypto.KeyGenerator
import javax.crypto.SecretKey

@Keep
object CloudSyncTokenStore {
    private const val TAG = "CloudSyncTokenStore"
    private const val KEYSTORE = "AndroidKeyStore"
    private const val KEY_ALIAS = "DolphinCloudSyncTokens"
    private const val PREFIX = "keystore:"
    private const val TRANSFORMATION = "AES/GCM/NoPadding"

    @JvmStatic
    @Keep
    fun protect(value: String): String {
        if (value.isEmpty()) {
            return ""
        }

        return try {
            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(Cipher.ENCRYPT_MODE, getOrCreateKey())
            val encrypted = cipher.doFinal(value.toByteArray(Charsets.UTF_8))
            val payload = ByteBuffer.allocate(4 + cipher.iv.size + encrypted.size)
                .putInt(cipher.iv.size)
                .put(cipher.iv)
                .put(encrypted)
                .array()
            PREFIX + Base64.encodeToString(payload, Base64.NO_WRAP)
        } catch (e: Exception) {
            Log.e(TAG, "Unable to protect Cloud Sync token", e)
            ""
        }
    }

    @JvmStatic
    @Keep
    fun unprotect(value: String): String {
        if (!value.startsWith(PREFIX)) {
            return value
        }

        return try {
            val payload = Base64.decode(value.substring(PREFIX.length), Base64.NO_WRAP)
            val buffer = ByteBuffer.wrap(payload)
            val ivSize = buffer.int
            if (ivSize <= 0 || ivSize > buffer.remaining()) {
                return ""
            }

            val iv = ByteArray(ivSize)
            buffer.get(iv)
            val encrypted = ByteArray(buffer.remaining())
            buffer.get(encrypted)

            val cipher = Cipher.getInstance(TRANSFORMATION)
            cipher.init(
                Cipher.DECRYPT_MODE,
                getOrCreateKey(),
                javax.crypto.spec.GCMParameterSpec(128, iv)
            )
            String(cipher.doFinal(encrypted), Charsets.UTF_8)
        } catch (e: Exception) {
            Log.e(TAG, "Unable to unprotect Cloud Sync token", e)
            ""
        }
    }

    private fun getOrCreateKey(): SecretKey {
        val keyStore = KeyStore.getInstance(KEYSTORE)
        keyStore.load(null)
        val existing = keyStore.getKey(KEY_ALIAS, null)
        if (existing is SecretKey) {
            return existing
        }

        val generator = KeyGenerator.getInstance(KeyProperties.KEY_ALGORITHM_AES, KEYSTORE)
        val specification = KeyGenParameterSpec.Builder(
            KEY_ALIAS,
            KeyProperties.PURPOSE_ENCRYPT or KeyProperties.PURPOSE_DECRYPT
        )
            .setBlockModes(KeyProperties.BLOCK_MODE_GCM)
            .setEncryptionPaddings(KeyProperties.ENCRYPTION_PADDING_NONE)
            .setKeySize(256)
            .build()
        generator.init(specification)
        return generator.generateKey()
    }
}
