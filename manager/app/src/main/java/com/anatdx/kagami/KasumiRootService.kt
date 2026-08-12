package com.anatdx.kagami

import android.content.Intent
import android.os.IBinder
import com.topjohnwu.superuser.ipc.RootService
import java.io.BufferedReader
import java.io.File
import java.io.InputStreamReader
import org.json.JSONObject

class KasumiRootService : RootService() {
    private val binder = object : IKasumiRootService.Stub() {
        override fun snapshotJson(): String =
            daemonPayload("api", "kasumi")

        override fun setEnabled(enabled: Boolean): String =
            daemonAction("kasumi", if (enabled) "enable" else "disable")

        override fun setDebug(enabled: Boolean): String =
            daemonAction("debug", if (enabled) "enable" else "disable")

        override fun setMountHide(enabled: Boolean): String =
            daemonAction("kasumi", "mount-hide", if (enabled) "on" else "off")

        override fun setMapsSpoof(enabled: Boolean): String =
            daemonAction("kasumi", "maps-spoof", if (enabled) "on" else "off")

        override fun setStatfsSpoof(enabled: Boolean): String =
            daemonAction("kasumi", "statfs-spoof", if (enabled) "on" else "off")

        override fun setSelinuxGuard(enabled: Boolean): String =
            daemonAction("kasumi", "selinux-fix", if (enabled) "on" else "off")

        override fun hidePath(path: String): String =
            daemonAction("kasumi", "hide-path", path)

        override fun deleteRule(path: String): String =
            daemonAction("kasumi", "delete-rule", path)

        override fun addMapsRule(targetIno: String, targetDev: String, spoofedIno: String, spoofedDev: String, spoofedPath: String): String =
            daemonAction("kasumi", "maps", "add", targetIno, targetDev, spoofedIno, spoofedDev, spoofedPath)

        override fun clearMapsRules(): String =
            daemonAction("kasumi", "maps", "clear")

        override fun setPolicy(owner: String, flags: Int): String =
            daemonAction("kasumi", "policy", "owner", owner, "0x${Integer.toUnsignedString(flags, 16)}")

        override fun setPolicyUids(list: String, uids: IntArray): String = daemonAction(
            *buildList {
                add("kasumi")
                add("policy")
                add(list)
                uids.forEach { add(Integer.toUnsignedString(it)) }
            }.toTypedArray(),
        )

        override fun clearPolicyUids(list: String): String =
            daemonAction("kasumi", "policy", "clear", list)

        override fun kernelLog(): String =
            runCommand("dmesg | tail -n 240")

        override fun statPath(path: String): String =
            KasumiNative.statPath(path)
    }

    override fun onBind(intent: Intent): IBinder = binder

    private data class CommandOutput(
        val exitCode: Int,
        val stdout: String,
        val stderr: String,
    )

    private fun kagamid(vararg args: String): CommandOutput {
        val binary = KAGAMID_CANDIDATES.firstOrNull { File(it).canExecute() }
            ?: throw IllegalStateException("kagamid not found")
        val process = ProcessBuilder(listOf(binary, *args))
            .redirectErrorStream(false)
            .start()
        val stdout = process.inputStream.bufferedReader().use { it.readText() }
        val stderr = process.errorStream.bufferedReader().use { it.readText() }
        return CommandOutput(process.waitFor(), stdout.trim(), stderr.trim())
    }

    private fun daemonEnvelope(vararg args: String): JSONObject {
        val result = kagamid("daemon", "call", *args)
        if (result.exitCode != 0) {
            throw IllegalStateException(
                result.stderr.ifBlank { "kagamid daemon is not running" },
            )
        }
        return JSONObject(result.stdout)
    }

    private fun daemonPayload(vararg args: String): String {
        val envelope = daemonEnvelope(*args)
        if (!envelope.optBoolean("ok", false)) {
            throw IllegalStateException(
                envelope.optString("stderr").ifBlank { envelope.optString("stdout") },
            )
        }
        return envelope.optString("stdout").trim()
    }

    private fun daemonAction(vararg args: String): String {
        val envelope = daemonEnvelope(*args)
        val ok = envelope.optBoolean("ok", false)
        val errno = envelope.optInt("errno", if (ok) 0 else 1)
        val error = envelope.optString("stderr").trim().ifBlank {
            if (ok) "" else envelope.optString("stdout").trim()
        }.ifBlank {
            if (ok) "" else "errno=$errno"
        }
        return JSONObject()
            .put("ok", ok)
            .put("errno", errno)
            .put("error", error)
            .toString()
    }

    private fun runCommand(command: String): String {
        return runCatching {
            val process = ProcessBuilder("sh", "-c", command)
                .redirectErrorStream(true)
                .start()
            val output = BufferedReader(InputStreamReader(process.inputStream)).use { it.readText() }
            val code = process.waitFor()
            if (code == 0) output.trimEnd() else "exit=$code\n${output.trimEnd()}"
        }.getOrElse { it.message ?: it::class.java.simpleName }
    }

    private companion object {
        val KAGAMID_CANDIDATES = listOf(
            "/data/adb/modules/kagami/kagamid",
            "/data/local/tmp/kagamid",
        )
    }
}
