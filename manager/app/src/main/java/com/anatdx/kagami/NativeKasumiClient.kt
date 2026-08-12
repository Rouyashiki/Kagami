package com.anatdx.kagami

import android.content.ComponentName
import android.content.Context
import android.content.Intent
import android.content.ServiceConnection
import android.os.IBinder
import android.os.RemoteException
import com.topjohnwu.superuser.ipc.RootService
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.suspendCancellableCoroutine
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import org.json.JSONObject
import kotlin.coroutines.resume

data class NativeKasumiSnapshot(
    val available: Boolean = false,
    val status: Int = 1,
    val expectedProtocol: Int = 0,
    val kernelProtocol: Int = 0,
    val uid: Int = -1,
    val euid: Int = -1,
    val lastErrno: Int = 0,
    val lastError: String = "",
    val modulesVisible: Boolean = false,
    val mountBase: String = "",
    val featureBitmask: Int = 0,
    val featureNames: List<String> = emptyList(),
    val hooks: String = "",
    val policy: NativeKasumiPolicy = NativeKasumiPolicy(),
    val rawJson: String = "",
    val error: String? = null,
)

data class NativeKasumiPolicy(
    val ok: Boolean = false,
    val errno: Int = 0,
    val err: Int = 0,
    val apiVersion: Int = 0,
    val generation: Long = 0,
    val owner: String = "auto",
    val effectiveOwner: String = "auto",
    val flags: Int = 0,
    val detectedRoots: Int = 0,
    val allowCount: Int = 0,
    val denyCount: Int = 0,
    val maxUidCount: Int = 0,
    val allowUids: List<Int> = emptyList(),
    val denyUids: List<Int> = emptyList(),
)

data class NativeActionResult(
    val ok: Boolean,
    val errno: Int,
    val error: String,
)

data class NativeStatResult(
    val ok: Boolean,
    val errno: Int,
    val error: String,
    val ino: Long,
    val dev: Long,
    val devMajor: Int,
    val devMinor: Int,
    val mode: Long,
)

class NativeKasumiClient(
    context: Context,
) {
    private val appContext = context.applicationContext
    private var service: IKasumiRootService? = null
    private var connection: ServiceConnection? = null

    suspend fun snapshot(): NativeKasumiSnapshot = withContext(Dispatchers.IO) {
        runCatching { parseSnapshot(rootService().snapshotJson()) }
            .getOrElse { NativeKasumiSnapshot(error = it.message ?: it::class.java.simpleName) }
    }

    suspend fun setEnabled(enabled: Boolean): NativeActionResult =
        callAction { setEnabled(enabled) }

    suspend fun setDebug(enabled: Boolean): NativeActionResult =
        callAction { setDebug(enabled) }

    suspend fun setMountHide(enabled: Boolean): NativeActionResult =
        callAction { setMountHide(enabled) }

    suspend fun setMapsSpoof(enabled: Boolean): NativeActionResult =
        callAction { setMapsSpoof(enabled) }

    suspend fun setStatfsSpoof(enabled: Boolean): NativeActionResult =
        callAction { setStatfsSpoof(enabled) }

    suspend fun setSelinuxGuard(enabled: Boolean): NativeActionResult =
        callAction { setSelinuxGuard(enabled) }

    suspend fun hidePath(path: String): NativeActionResult =
        callAction { hidePath(path) }

    suspend fun deleteRule(path: String): NativeActionResult =
        callAction { deleteRule(path) }

    suspend fun addMapsRule(targetIno: String, targetDev: String, spoofedIno: String, spoofedDev: String, spoofedPath: String): NativeActionResult =
        callAction { addMapsRule(targetIno, targetDev, spoofedIno, spoofedDev, spoofedPath) }

    suspend fun clearMapsRules(): NativeActionResult =
        callAction { clearMapsRules() }

    suspend fun setPolicy(owner: String, flags: Int): NativeActionResult =
        callAction { setPolicy(owner, flags) }

    suspend fun setPolicyUids(list: String, uids: IntArray): NativeActionResult =
        callAction { setPolicyUids(list, uids) }

    suspend fun clearPolicyUids(list: String): NativeActionResult =
        callAction { clearPolicyUids(list) }

    suspend fun kernelLog(): String = withContext(Dispatchers.IO) {
        runCatching { rootService().kernelLog() }
            .getOrElse { it.message ?: it::class.java.simpleName }
    }

    suspend fun statPath(path: String): NativeStatResult = withContext(Dispatchers.IO) {
        runCatching {
            val root = JSONObject(rootService().statPath(path))
            NativeStatResult(
                ok = root.optBoolean("ok", false),
                errno = root.optInt("errno", 0),
                error = root.optString("error"),
                ino = root.optLong("ino", 0L),
                dev = root.optLong("dev", 0L),
                devMajor = root.optInt("dev_major", 0),
                devMinor = root.optInt("dev_minor", 0),
                mode = root.optLong("mode", 0L),
            )
        }.getOrElse {
            NativeStatResult(false, 0, it.message ?: it::class.java.simpleName, 0L, 0L, 0, 0, 0L)
        }
    }

    private suspend fun callAction(block: IKasumiRootService.() -> String): NativeActionResult = withContext(Dispatchers.IO) {
        runCatching {
            val root = JSONObject(rootService().block())
            NativeActionResult(
                ok = root.optBoolean("ok", false),
                errno = root.optInt("errno", 0),
                error = root.optString("error"),
            )
        }.getOrElse {
            NativeActionResult(ok = false, errno = 0, error = it.message ?: it::class.java.simpleName)
        }
    }

    private suspend fun rootService(): IKasumiRootService {
        service?.let { return it }
        return withTimeout(ROOT_SERVICE_TIMEOUT_MS) {
            withContext(Dispatchers.Main.immediate) {
                suspendCancellableCoroutine { continuation ->
                    val intent = Intent(appContext, KasumiRootService::class.java)
                    val conn = object : ServiceConnection {
                        override fun onServiceConnected(name: ComponentName, binder: IBinder) {
                            val remote = IKasumiRootService.Stub.asInterface(binder)
                            service = remote
                            if (continuation.isActive) {
                                continuation.resume(remote)
                            }
                        }

                        override fun onServiceDisconnected(name: ComponentName) {
                            service = null
                            connection = null
                        }

                        override fun onBindingDied(name: ComponentName) {
                            service = null
                            connection = null
                        }

                        override fun onNullBinding(name: ComponentName) {
                            service = null
                            connection = null
                            if (continuation.isActive) {
                                continuation.resumeWith(Result.failure(RemoteException("RootService returned null binding")))
                            }
                        }
                    }
                    connection = conn
                    continuation.invokeOnCancellation {
                        if (service == null && connection === conn) {
                            runCatching { RootService.unbind(conn) }
                            connection = null
                        }
                    }
                    RootService.bind(intent, conn)
                }
            }
        }
    }

    private fun parseSnapshot(json: String): NativeKasumiSnapshot {
        val root = JSONObject(json)
        val features = root.optJSONObject("features")
        val policy = root.optJSONObject("policy")
        return NativeKasumiSnapshot(
            available = root.optBoolean("available", false),
            status = root.optInt("status", 1),
            expectedProtocol = root.optInt("expected_protocol", 0),
            kernelProtocol = root.optInt("kernel_protocol", 0),
            uid = root.optInt("uid", -1),
            euid = root.optInt("euid", -1),
            lastErrno = root.optInt("last_errno", 0),
            lastError = root.optString("last_error"),
            modulesVisible = root.optBoolean("modules_visible", false),
            mountBase = root.optString("mount_base", ""),
            featureBitmask = features?.optInt("bitmask", 0) ?: 0,
            featureNames = features?.optJSONArray("names").orEmptyStrings(),
            hooks = root.optString("hooks", ""),
            policy = NativeKasumiPolicy(
                ok = policy?.optBoolean("ok", false) ?: false,
                errno = policy?.optInt("errno", 0) ?: 0,
                err = policy?.optInt("err", 0) ?: 0,
                apiVersion = policy?.optInt("api_version", 0) ?: 0,
                generation = policy?.optLong("generation", 0L) ?: 0L,
                owner = policy?.optString("owner", "auto") ?: "auto",
                effectiveOwner = policy?.optString("effective_owner", "auto") ?: "auto",
                flags = policy?.optInt("flags", 0) ?: 0,
                detectedRoots = policy?.optInt("detected_roots", 0) ?: 0,
                allowCount = policy?.optInt("allow_count", 0) ?: 0,
                denyCount = policy?.optInt("deny_count", 0) ?: 0,
                maxUidCount = policy?.optInt("max_uid_count", 0) ?: 0,
                allowUids = policy?.optJSONArray("allow_uids").orEmptyInts(),
                denyUids = policy?.optJSONArray("deny_uids").orEmptyInts(),
            ),
            rawJson = json,
        )
    }

    private companion object {
        const val ROOT_SERVICE_TIMEOUT_MS = 10_000L
    }
}
