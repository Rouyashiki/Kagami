package com.anatdx.kagami

import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import org.json.JSONArray
import org.json.JSONObject
import java.io.BufferedReader
import java.io.InputStreamReader

data class CommandResult(
    val exitCode: Int,
    val stdout: String,
    val stderr: String,
)

data class KasumiStatus(
    val kernel: String = "Unknown",
    val selinux: String = "Unknown",
    val mountBase: String = "",
    val available: Boolean = false,
    val status: Int = 1,
    val featureNames: List<String> = emptyList(),
    val hooks: String = "",
)

data class LkmStatus(
    val loaded: Boolean = false,
    val autoload: Boolean = false,
    val kmiOverride: String = "",
)

data class ModuleInfo(
    val id: String,
    val name: String,
    val version: String,
    val author: String,
    val description: String,
    val mode: String,
    val strategy: String,
)

data class KagamiSnapshot(
    val status: KasumiStatus = KasumiStatus(),
    val lkm: LkmStatus = LkmStatus(),
    val modules: List<ModuleInfo> = emptyList(),
    val rawSystemJson: String = "",
)

class KagamidBridge(
    private val binary: String = "/data/adb/modules/kagami/kagamid",
) {
    suspend fun snapshot(): KagamiSnapshot {
        val system = run("api system")
        val lkm = run("api lkm")
        val modules = run("module list")
        return KagamiSnapshot(
            status = parseStatus(system.stdout),
            lkm = parseLkm(lkm.stdout),
            modules = parseModules(modules.stdout),
            rawSystemJson = system.stdout.ifBlank { system.stderr },
        )
    }

    suspend fun run(args: String): CommandResult = withContext(Dispatchers.IO) {
        val command = buildString {
            append("for p in ")
            append(binary)
            append(" /data/local/tmp/kagamid; do ")
            append("if [ -x \"\$p\" ]; then exec \"\$p\" ")
            append(args)
            append("; fi; done; echo kagamid not found >&2; exit 127")
        }
        val process = ProcessBuilder("su", "-c", command).start()
        val stdout = process.inputStream.bufferedReader().use(BufferedReader::readText)
        val stderr = process.errorStream.bufferedReader().use(BufferedReader::readText)
        val exitCode = process.waitFor()
        CommandResult(exitCode, stdout.trim(), stderr.trim())
    }

    suspend fun startDaemon(): CommandResult = run("daemon start")

    suspend fun daemonCall(args: String): CommandResult = run("daemon call $args")

    private fun parseStatus(json: String): KasumiStatus {
        if (json.isBlank()) return KasumiStatus()
        val root = JSONObject(json)
        val features = root.optJSONObject("features")
        return KasumiStatus(
            kernel = root.optString("kernel", "Unknown"),
            selinux = root.optString("selinux", "Unknown"),
            mountBase = root.optString("mount_base", ""),
            available = root.optBoolean("kasumi_available", false),
            status = root.optInt("kasumi_status", 1),
            featureNames = features?.optJSONArray("names").orEmptyStrings(),
            hooks = root.optString("hooks", ""),
        )
    }

    private fun parseLkm(json: String): LkmStatus {
        if (json.isBlank()) return LkmStatus()
        val root = JSONObject(json)
        return LkmStatus(
            loaded = root.optBoolean("loaded", false),
            autoload = root.optBoolean("autoload", false),
            kmiOverride = root.optString("kmi_override", ""),
        )
    }

    private fun parseModules(json: String): List<ModuleInfo> {
        if (json.isBlank()) return emptyList()
        val root = JSONObject(json)
        val modules = root.optJSONArray("modules") ?: JSONArray()
        return buildList {
            for (index in 0 until modules.length()) {
                val item = modules.optJSONObject(index) ?: continue
                add(
                    ModuleInfo(
                        id = item.optString("id"),
                        name = item.optString("name", item.optString("id")),
                        version = item.optString("version"),
                        author = item.optString("author"),
                        description = item.optString("description"),
                        mode = item.optString("mode", "auto"),
                        strategy = item.optString("strategy", "auto"),
                    ),
                )
            }
        }
    }
}

fun JSONArray?.orEmptyStrings(): List<String> {
    if (this == null) return emptyList()
    return buildList {
        for (index in 0 until length()) {
            add(optString(index))
        }
    }
}

fun JSONArray?.orEmptyInts(): List<Int> {
    if (this == null) return emptyList()
    return buildList {
        for (index in 0 until length()) {
            add(optInt(index))
        }
    }
}
