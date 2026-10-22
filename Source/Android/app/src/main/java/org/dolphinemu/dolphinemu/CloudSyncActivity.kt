// SPDX-License-Identifier: GPL-2.0-or-later

package org.dolphinemu.dolphinemu

import android.content.ActivityNotFoundException
import android.content.Intent
import android.net.Uri
import android.os.Bundle
import android.view.View
import android.widget.Toast
import androidx.appcompat.app.AppCompatActivity
import androidx.lifecycle.lifecycleScope
import com.google.android.material.dialog.MaterialAlertDialogBuilder
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.launch
import kotlinx.coroutines.withContext
import org.dolphinemu.dolphinemu.databinding.ActivityCloudSyncBinding
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader
import java.io.OutputStream
import java.net.InetAddress
import java.net.ServerSocket
import java.net.Socket
import java.net.SocketTimeoutException

class CloudSyncActivity : AppCompatActivity() {
    private lateinit var binding: ActivityCloudSyncBinding
    private var oauthJob: Job? = null
    private var oauthServer: ServerSocket? = null
    private var connected = false

    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        binding = ActivityCloudSyncBinding.inflate(layoutInflater)
        setContentView(binding.root)

        binding.toolbarCloudSync.setTitle(R.string.cloud_sync)
        binding.toolbarCloudSync.setNavigationOnClickListener { finish() }

        binding.connectButton.setOnClickListener { connectDropbox() }
        binding.disconnectButton.setOnClickListener { confirmDisconnect() }
        binding.testButton.setOnClickListener { testConnection() }
        binding.saveButton.setOnClickListener { saveSettings() }
        binding.syncLastButton.setOnClickListener { syncLastGame() }

        refreshState()
        showLastResult()
    }

    override fun onDestroy() {
        oauthJob?.cancel()
        runCatching { oauthServer?.close() }
        oauthServer = null
        super.onDestroy()
    }

    private fun refreshState() {
        runCatching {
            val state = CloudSyncBridge.getState()
            connected = state.optBoolean("connected")
            binding.enabledSwitch.isChecked = state.optBoolean("enabled")
            binding.autoSyncSwitch.isChecked = state.optBoolean("auto_sync")
            binding.syncStatesSwitch.isChecked = state.optBoolean("sync_states")
            binding.rootPath.setText(state.optString("root_path", "/DolphinFork"))
            binding.accountStatus.setText(
                if (connected) R.string.cloud_sync_connected else R.string.cloud_sync_not_connected
            )
            binding.disconnectButton.isEnabled = connected
            binding.testButton.isEnabled = connected
            binding.syncLastButton.isEnabled = connected
        }.onFailure {
            binding.accountStatus.setText(R.string.cloud_sync_not_connected)
            showError(it.message ?: getString(R.string.cloud_sync_error))
        }
    }

    private fun showLastResult() {
        runCatching {
            val result = CloudSyncBridge.getLastResult()
            if (result.has("status")) {
                binding.resultText.text = formatResult(result)
            }
        }
    }

    private fun saveSettings() {
        val rootPath = binding.rootPath.text?.toString().orEmpty().trim()
        val enabled = binding.enabledSwitch.isChecked
        val autoSync = binding.autoSyncSwitch.isChecked
        val syncStates = binding.syncStatesSwitch.isChecked
        setBusy(true)
        lifecycleScope.launch {
            val error = withContext(Dispatchers.IO) {
                CloudSyncBridge.updateSettings(enabled, autoSync, syncStates, rootPath)
            }
            setBusy(false)
            if (error.isEmpty()) {
                Toast.makeText(this@CloudSyncActivity, R.string.cloud_sync_saved, Toast.LENGTH_SHORT)
                    .show()
                refreshState()
            } else {
                showError(error)
            }
        }
    }

    private fun testConnection() {
        setBusy(true)
        lifecycleScope.launch {
            val error = withContext(Dispatchers.IO) { CloudSyncBridge.testConnection() }
            setBusy(false)
            if (error.isEmpty()) {
                Toast.makeText(
                    this@CloudSyncActivity,
                    R.string.cloud_sync_connection_ok,
                    Toast.LENGTH_SHORT
                ).show()
            } else {
                showError(error)
            }
        }
    }

    private fun syncLastGame() {
        setBusy(true)
        lifecycleScope.launch {
            val result = withContext(Dispatchers.IO) { CloudSyncBridge.syncLastGame() }
            setBusy(false)
            binding.resultText.text = formatResult(result)
        }
    }

    private fun confirmDisconnect() {
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.cloud_sync_disconnect)
            .setMessage(R.string.cloud_sync_disconnect_confirm)
            .setNegativeButton(android.R.string.cancel, null)
            .setPositiveButton(R.string.cloud_sync_disconnect) { _, _ ->
                CloudSyncBridge.disconnect()
                refreshState()
            }
            .show()
    }

    private fun connectDropbox() {
        oauthJob?.cancel()
        oauthJob = lifecycleScope.launch {
            setBusy(true)
            var server: ServerSocket? = null
            try {
                val authorization = withContext(Dispatchers.IO) {
                    CloudSyncBridge.createAuthorization()
                }
                val state = authorization.getString("state")
                val verifier = authorization.getString("verifier")
                val url = authorization.getString("url")

                val activeServer = withContext(Dispatchers.IO) {
                    ServerSocket(53682, 1, InetAddress.getByName("127.0.0.1")).also {
                        it.soTimeout = OAUTH_TIMEOUT_MS
                        oauthServer = it
                    }
                }
                server = activeServer

                openAuthorizationUrl(url)
                val response = withContext(Dispatchers.IO) {
                    receiveAuthorization(activeServer, state)
                }
                val oauthError = response.error
                if (oauthError != null) {
                    showError(oauthError)
                    return@launch
                }

                val error = withContext(Dispatchers.IO) {
                    CloudSyncBridge.completeAuthorization(response.code.orEmpty(), verifier)
                }
                if (error.isEmpty()) {
                    Toast.makeText(
                        this@CloudSyncActivity,
                        R.string.cloud_sync_connected_toast,
                        Toast.LENGTH_SHORT
                    ).show()
                    refreshState()
                } else {
                    showError(error)
                }
            } catch (e: ActivityNotFoundException) {
                showError(getString(R.string.cloud_sync_no_browser))
            } catch (e: Exception) {
                showError(e.message ?: getString(R.string.cloud_sync_oauth_failed))
            } finally {
                runCatching { server?.close() }
                oauthServer = null
                setBusy(false)
            }
        }
    }

    private fun openAuthorizationUrl(url: String) {
        startActivity(Intent(Intent.ACTION_VIEW, Uri.parse(url)))
    }

    private fun receiveAuthorization(server: ServerSocket, expectedState: String): OAuthResponse {
        try {
            while (true) {
                val socket = server.accept()
                try {
                    val reader = BufferedReader(InputStreamReader(socket.getInputStream()))
                    val requestLine = reader.readLine()
                    while (true) {
                        val header = reader.readLine()
                        if (header.isNullOrEmpty()) {
                            break
                        }
                    }

                    if (requestLine == null) {
                        return OAuthResponse(error = getString(R.string.cloud_sync_oauth_failed))
                    }

                    val target = requestLine.split(' ').getOrNull(1).orEmpty()
                    val uri = Uri.parse("http://127.0.0.1$target")
                    val error = uri.getQueryParameter("error_description")
                        ?: uri.getQueryParameter("error")
                    val code = uri.getQueryParameter("code")
                    val state = uri.getQueryParameter("state")

                    if (error != null) {
                        writeResponse(socket, 200, false)
                        return OAuthResponse(error = error)
                    }
                    if (code.isNullOrEmpty()) {
                        writeResponse(socket, 404, false)
                        continue
                    }
                    if (state != expectedState) {
                        writeResponse(socket, 400, false)
                        return OAuthResponse(error = getString(R.string.cloud_sync_oauth_state_mismatch))
                    }

                    writeResponse(socket, 200, true)
                    return OAuthResponse(code = code)
                } finally {
                    socket.close()
                }
            }
        } catch (e: SocketTimeoutException) {
            return OAuthResponse(error = getString(R.string.cloud_sync_oauth_timeout))
        } catch (e: Exception) {
            return OAuthResponse(error = e.message ?: getString(R.string.cloud_sync_oauth_failed))
        }
    }

    private fun writeResponse(socket: Socket, status: Int, success: Boolean) {
        val message = getString(
            if (success) R.string.cloud_sync_oauth_success else R.string.cloud_sync_oauth_failed
        )
        val body = "<!doctype html><meta name=\"viewport\" content=\"width=device-width\">" +
            "<title>Dolphin</title><body style=\"font-family:sans-serif;padding:2em\">" +
            "<h1>Dolphin</h1><p>$message</p></body>"
        val response = "HTTP/1.1 $status ${if (status == 200) "OK" else "Error"}\r\n" +
            "Content-Type: text/html; charset=utf-8\r\n" +
            "Content-Length: ${body.toByteArray(Charsets.UTF_8).size}\r\n" +
            "Connection: close\r\n\r\n$body"
        val output: OutputStream = socket.getOutputStream()
        output.write(response.toByteArray(Charsets.UTF_8))
        output.flush()
    }

    private fun formatResult(result: JSONObject): String {
        val status = result.optString("status")
        val message = result.optString("message")
        if (status == "success") {
            return getString(
                R.string.cloud_sync_result_counts,
                result.optJSONArray("uploaded")?.length() ?: 0,
                result.optJSONArray("downloaded")?.length() ?: 0
            )
        }
        return if (message.isEmpty()) status else "$status: $message"
    }

    private fun setBusy(busy: Boolean) {
        binding.progress.visibility = if (busy) View.VISIBLE else View.GONE
        binding.connectButton.isEnabled = !busy
        binding.saveButton.isEnabled = !busy
        binding.disconnectButton.isEnabled = !busy && connected
        binding.testButton.isEnabled = !busy && connected
        binding.syncLastButton.isEnabled = !busy && connected
    }

    private fun showError(error: String) {
        if (isFinishing || isDestroyed) {
            return
        }
        MaterialAlertDialogBuilder(this)
            .setTitle(R.string.cloud_sync_error)
            .setMessage(error)
            .setPositiveButton(android.R.string.ok, null)
            .show()
    }

    private data class OAuthResponse(val code: String? = null, val error: String? = null)

    companion object {
        private const val OAUTH_TIMEOUT_MS = 5 * 60 * 1000
    }
}
