package com.anatdx.kagami

import android.app.Application
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.viewModelScope
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.StateFlow
import kotlinx.coroutines.flow.asStateFlow
import kotlinx.coroutines.launch

enum class AppPolicyChoice { OFF, ALLOW, DENY }

data class AppPickerState(
    val visible: Boolean = false,
    val loading: Boolean = false,
    val apps: List<AppEntry> = emptyList(),
    val query: String = "",
    val includeSystem: Boolean = false,
    val choices: Map<String, AppPolicyChoice> = emptyMap(),
    val error: String? = null,
)

data class MapsResolveState(
    val message: String? = null,
    val isError: Boolean = false,
    val resolving: Boolean = false,
    val pathInput: String = "",
)

data class ManagerUiState(
    val loading: Boolean = false,
    val snapshot: KagamiSnapshot = KagamiSnapshot(),
    val nativeSnapshot: NativeKasumiSnapshot = NativeKasumiSnapshot(),
    val nativePath: String = "",
    val kernelDebugEnabled: Boolean = false,
    val mapsTargetIno: String = "",
    val mapsTargetDev: String = "0",
    val mapsSpoofedIno: String = "",
    val mapsSpoofedDev: String = "",
    val mapsSpoofedPath: String = "",
    val mapsResolve: MapsResolveState = MapsResolveState(),
    val policyOwner: String = "auto",
    val policyAllowUids: String = "",
    val policyDenyUids: String = "",
    val policyIncludeIsolated: Boolean = false,
    val appPicker: AppPickerState = AppPickerState(),
    val kernelLog: String = "",
    val command: String = "api system",
    val commandResult: CommandResult? = null,
    val nativeActionResult: NativeActionResult? = null,
    val error: String? = null,
)

class MainViewModel(application: Application) : AndroidViewModel(application) {
    private val bridge = KagamidBridge()
    private val nativeClient = NativeKasumiClient(application)
    private val appRepository = AppRepository(application)
    private val mutableState = MutableStateFlow(ManagerUiState())
    val state: StateFlow<ManagerUiState> = mutableState.asStateFlow()

    init {
        refresh()
    }

    fun refresh() {
        viewModelScope.launch {
            mutableState.value = mutableState.value.copy(loading = true, error = null)
            val nativeSnapshot = nativeClient.snapshot()
            mutableState.value = syncPolicyEditor(mutableState.value.copy(
                loading = false,
                error = nativeSnapshot.error,
            ), nativeSnapshot)
        }
    }

    fun refreshDaemon() {
        viewModelScope.launch {
            mutableState.value = mutableState.value.copy(loading = true, error = null)
            runCatching { bridge.snapshot() }
                .onSuccess { snapshot ->
                    mutableState.value = mutableState.value.copy(
                        loading = false,
                        snapshot = snapshot,
                        error = null,
                    )
                }
                .onFailure { error ->
                    mutableState.value = mutableState.value.copy(
                        loading = false,
                        error = error.message ?: error::class.java.simpleName,
                    )
                }
        }
    }

    fun startDaemon() {
        viewModelScope.launch {
            mutableState.value = mutableState.value.copy(loading = true, error = null)
            runCatching { bridge.startDaemon() }
                .onSuccess { result ->
                    val snapshot = runCatching { bridge.snapshot() }.getOrDefault(state.value.snapshot)
                    mutableState.value = mutableState.value.copy(
                        loading = false,
                        snapshot = snapshot,
                        commandResult = result,
                        error = result.stderr.ifBlank { null },
                    )
                }
                .onFailure { error ->
                    mutableState.value = mutableState.value.copy(
                        loading = false,
                        error = error.message ?: error::class.java.simpleName,
                    )
                }
        }
    }

    fun setCommand(command: String) {
        mutableState.value = mutableState.value.copy(command = command)
    }

    fun runCommand() {
        val command = state.value.command.trim()
        if (command.isEmpty()) return
        viewModelScope.launch {
            mutableState.value = mutableState.value.copy(loading = true, error = null)
            runCatching { bridge.run(command) }
                .onSuccess { result ->
                    mutableState.value = mutableState.value.copy(
                        loading = false,
                        commandResult = result,
                        error = null,
                    )
                }
                .onFailure { error ->
                    mutableState.value = mutableState.value.copy(
                        loading = false,
                        error = error.message ?: error::class.java.simpleName,
                    )
                }
        }
    }

    fun setKasumiEnabled(enabled: Boolean) {
        runNativeAction { nativeClient.setEnabled(enabled) }
    }

    fun setDebug(enabled: Boolean) {
        mutableState.value = mutableState.value.copy(kernelDebugEnabled = enabled)
        runNativeAction { nativeClient.setDebug(enabled) }
    }

    fun setMountHide(enabled: Boolean) {
        runNativeAction { nativeClient.setMountHide(enabled) }
    }

    fun setMapsSpoof(enabled: Boolean) {
        runNativeAction { nativeClient.setMapsSpoof(enabled) }
    }

    fun setStatfsSpoof(enabled: Boolean) {
        runNativeAction { nativeClient.setStatfsSpoof(enabled) }
    }

    fun setSelinuxGuard(enabled: Boolean) {
        runNativeAction { nativeClient.setSelinuxGuard(enabled) }
    }

    fun setNativePath(path: String) {
        mutableState.value = mutableState.value.copy(nativePath = path)
    }

    fun setMapsTargetIno(value: String) {
        mutableState.value = mutableState.value.copy(mapsTargetIno = value)
    }

    fun setMapsTargetDev(value: String) {
        mutableState.value = mutableState.value.copy(mapsTargetDev = value)
    }

    fun setMapsSpoofedIno(value: String) {
        mutableState.value = mutableState.value.copy(mapsSpoofedIno = value)
    }

    fun setMapsSpoofedDev(value: String) {
        mutableState.value = mutableState.value.copy(mapsSpoofedDev = value)
    }

    fun setMapsSpoofedPath(value: String) {
        mutableState.value = mutableState.value.copy(mapsSpoofedPath = value)
    }

    fun setMapsResolvePath(value: String) {
        mutableState.value = mutableState.value.copy(
            mapsResolve = mutableState.value.mapsResolve.copy(pathInput = value, message = null, isError = false),
        )
    }

    fun resolveMapsTargetByPath() {
        val current = state.value
        val path = current.mapsResolve.pathInput.trim()
        if (path.isEmpty()) {
            mutableState.value = current.copy(
                mapsResolve = current.mapsResolve.copy(message = null, isError = true),
            )
            return
        }
        viewModelScope.launch {
            mutableState.value = current.copy(mapsResolve = current.mapsResolve.copy(resolving = true))
            val stat = nativeClient.statPath(path)
            val now = state.value
            if (stat.ok) {
                mutableState.value = now.copy(
                    mapsTargetIno = stat.ino.toString(),
                    mapsTargetDev = stat.dev.toString(),
                    mapsResolve = now.mapsResolve.copy(
                        resolving = false,
                        isError = false,
                        message = formatResolveOk(stat),
                    ),
                )
            } else {
                mutableState.value = now.copy(
                    mapsResolve = now.mapsResolve.copy(
                        resolving = false,
                        isError = true,
                        message = stat.error.ifBlank { "errno=${stat.errno}" },
                    ),
                )
            }
        }
    }

    fun applyMapsPresetLib() {
        applyMapsPreset("/system/lib64/libc.so")
    }

    fun applyMapsPresetApk() {
        applyMapsPreset("/system/framework/services.jar")
    }

    private fun applyMapsPreset(path: String) {
        viewModelScope.launch {
            val stat = nativeClient.statPath(path)
            val current = state.value
            mutableState.value = if (stat.ok) {
                current.copy(
                    mapsSpoofedIno = stat.ino.toString(),
                    mapsSpoofedDev = stat.dev.toString(),
                    mapsSpoofedPath = path,
                    mapsResolve = current.mapsResolve.copy(message = formatResolveOk(stat), isError = false),
                )
            } else {
                current.copy(
                    mapsResolve = current.mapsResolve.copy(
                        message = stat.error.ifBlank { "errno=${stat.errno}" },
                        isError = true,
                    ),
                )
            }
        }
    }

    private fun formatResolveOk(stat: NativeStatResult): String {
        return "ino=${stat.ino} dev=${stat.dev} (${stat.devMajor}:${stat.devMinor})"
    }

    fun setPolicyOwner(value: String) {
        mutableState.value = mutableState.value.copy(policyOwner = value)
    }

    fun setPolicyAllowUids(value: String) {
        mutableState.value = mutableState.value.copy(policyAllowUids = value)
    }

    fun setPolicyDenyUids(value: String) {
        mutableState.value = mutableState.value.copy(policyDenyUids = value)
    }

    fun setPolicyIncludeIsolated(value: Boolean) {
        mutableState.value = mutableState.value.copy(policyIncludeIsolated = value)
    }

    fun openAppPicker() {
        val current = state.value
        val seeded = seedChoicesFromPolicy(current.appPicker.choices, current.nativeSnapshot.policy)
        mutableState.value = current.copy(
            appPicker = current.appPicker.copy(visible = true, choices = seeded, error = null),
        )
        if (current.appPicker.apps.isEmpty()) {
            loadApps()
        }
    }

    fun closeAppPicker() {
        mutableState.value = mutableState.value.copy(
            appPicker = mutableState.value.appPicker.copy(visible = false),
        )
    }

    fun setAppPickerQuery(value: String) {
        mutableState.value = mutableState.value.copy(
            appPicker = mutableState.value.appPicker.copy(query = value),
        )
    }

    fun setAppPickerIncludeSystem(value: Boolean) {
        mutableState.value = mutableState.value.copy(
            appPicker = mutableState.value.appPicker.copy(includeSystem = value),
        )
    }

    fun setAppChoice(packageName: String, choice: AppPolicyChoice) {
        val current = state.value.appPicker
        val next = current.choices.toMutableMap()
        if (choice == AppPolicyChoice.OFF) {
            next.remove(packageName)
        } else {
            next[packageName] = choice
        }
        mutableState.value = state.value.copy(appPicker = current.copy(choices = next))
    }

    fun loadApps() {
        viewModelScope.launch {
            mutableState.value = mutableState.value.copy(
                appPicker = mutableState.value.appPicker.copy(loading = true, error = null),
            )
            val result = runCatching { appRepository.loadInstalledApps() }
            mutableState.value = mutableState.value.copy(
                appPicker = mutableState.value.appPicker.copy(
                    loading = false,
                    apps = result.getOrDefault(emptyList()),
                    error = result.exceptionOrNull()?.message,
                ),
            )
        }
    }

    fun applyAppPickerSelection() {
        val current = state.value
        val picker = current.appPicker
        val allowUids = sortedDistinctUids(picker, AppPolicyChoice.ALLOW)
        val denyUids = sortedDistinctUids(picker, AppPolicyChoice.DENY)
        mutableState.value = current.copy(
            policyOwner = "manual",
            policyAllowUids = allowUids.joinToString(","),
            policyDenyUids = denyUids.joinToString(","),
            appPicker = picker.copy(visible = false),
        )
        applyManualPolicy()
    }

    private fun sortedDistinctUids(picker: AppPickerState, target: AppPolicyChoice): List<Int> {
        val packageToUid = picker.apps.associate { it.packageName to it.uid }
        return picker.choices.entries
            .filter { it.value == target }
            .mapNotNull { packageToUid[it.key] }
            .filter { it > 0 }
            .distinct()
            .sorted()
    }

    private fun seedChoicesFromPolicy(existing: Map<String, AppPolicyChoice>, policy: NativeKasumiPolicy): Map<String, AppPolicyChoice> {
        if (existing.isNotEmpty()) return existing
        val apps = state.value.appPicker.apps
        if (apps.isEmpty()) return existing
        val allow = policy.allowUids.toSet()
        val deny = policy.denyUids.toSet()
        val seeded = mutableMapOf<String, AppPolicyChoice>()
        apps.forEach { app ->
            when {
                deny.contains(app.uid) -> seeded[app.packageName] = AppPolicyChoice.DENY
                allow.contains(app.uid) -> seeded[app.packageName] = AppPolicyChoice.ALLOW
            }
        }
        return seeded
    }

    fun hideNativePath() {
        val path = state.value.nativePath.trim()
        if (path.isEmpty()) return
        runNativeAction { nativeClient.hidePath(path) }
    }

    fun deleteNativeRule() {
        val path = state.value.nativePath.trim()
        if (path.isEmpty()) return
        runNativeAction { nativeClient.deleteRule(path) }
    }

    fun addMapsRule() {
        val current = state.value
        if (current.mapsTargetIno.isBlank() || current.mapsSpoofedIno.isBlank() ||
            current.mapsSpoofedDev.isBlank() || current.mapsSpoofedPath.isBlank()
        ) {
            mutableState.value = current.copy(error = "Maps rule is incomplete")
            return
        }
        runNativeAction {
            nativeClient.addMapsRule(
                current.mapsTargetIno.trim(),
                current.mapsTargetDev.trim().ifBlank { "0" },
                current.mapsSpoofedIno.trim(),
                current.mapsSpoofedDev.trim(),
                current.mapsSpoofedPath.trim(),
            )
        }
    }

    fun clearMapsRules() {
        runNativeAction { nativeClient.clearMapsRules() }
    }

    fun applyManualPolicy() {
        val current = state.value
        val allow = parseUidList(current.policyAllowUids)
        val deny = parseUidList(current.policyDenyUids)
        if (allow == null || deny == null) {
            mutableState.value = current.copy(error = "Policy UID list contains invalid values")
            return
        }
        val owner = current.policyOwner.trim().lowercase()
        if (owner !in setOf("auto", "kernelsu", "ksu", "apatch", "manual", "disabled", "off")) {
            mutableState.value = current.copy(error = "Policy owner is invalid")
            return
        }
        persistAndApplyPolicy(owner, allow, deny, current.policyIncludeIsolated)
    }

    private fun persistAndApplyPolicy(owner: String, allow: List<Int>, deny: List<Int>, includeIsolated: Boolean) {
        val allowCsv = allow.joinToString(",").ifBlank { "-" }
        val denyCsv = deny.joinToString(",").ifBlank { "-" }
        val isolated = if (includeIsolated) "on" else "off"
        runNativeAction {
            val persisted = bridge.run("config policy set $owner $allowCsv $denyCsv $isolated")
            if (persisted.exitCode != 0) {
                return@runNativeAction NativeActionResult(
                    ok = false,
                    errno = persisted.exitCode,
                    error = persisted.stderr.ifBlank { persisted.stdout },
                )
            }
            val applied = bridge.run("config apply")
            NativeActionResult(
                ok = applied.exitCode == 0,
                errno = applied.exitCode,
                error = applied.stderr.ifBlank { if (applied.exitCode == 0) "" else applied.stdout },
            )
        }
    }

    fun clearManualPolicy() {
        persistAndApplyPolicy("auto", emptyList(), emptyList(), false)
    }

    fun refreshKernelLog() {
        viewModelScope.launch {
            mutableState.value = mutableState.value.copy(loading = true, error = null)
            val log = nativeClient.kernelLog()
            mutableState.value = mutableState.value.copy(loading = false, kernelLog = log)
        }
    }

    private fun runNativeAction(action: suspend () -> NativeActionResult) {
        viewModelScope.launch {
            mutableState.value = mutableState.value.copy(loading = true, error = null)
            val result = action()
            val nativeSnapshot = nativeClient.snapshot()
            mutableState.value = syncPolicyEditor(mutableState.value.copy(
                loading = false,
                nativeActionResult = result,
                error = when {
                    result.ok -> nativeSnapshot.error
                    result.error.isNotBlank() -> result.error
                    else -> "Native action failed"
                },
            ), nativeSnapshot)
        }
    }

    private fun syncPolicyEditor(state: ManagerUiState, snapshot: NativeKasumiSnapshot): ManagerUiState {
        if (!snapshot.policy.ok) {
            return state.copy(nativeSnapshot = snapshot)
        }
        val policy = snapshot.policy
        return state.copy(
            nativeSnapshot = snapshot,
            policyOwner = policy.owner,
            policyAllowUids = policy.allowUids.joinToString(","),
            policyDenyUids = policy.denyUids.joinToString(","),
            policyIncludeIsolated = (policy.flags and POLICY_FLAG_INCLUDE_ISOLATED_UIDS) != 0,
        )
    }

    private fun parseUidList(value: String): List<Int>? {
        if (value.isBlank()) return emptyList()
        return value.split(',', ' ', '\n', '\t')
            .filter { it.isNotBlank() }
            .map {
                val parsed = it.toLongOrNull() ?: return null
                if (parsed <= 0 || parsed > UInt.MAX_VALUE.toLong()) return null
                parsed.toInt()
            }
    }

    private companion object {
        const val POLICY_FLAG_USE_ALLOW_UIDS = 1 shl 0
        const val POLICY_FLAG_USE_DENY_UIDS = 1 shl 1
        const val POLICY_FLAG_INCLUDE_ISOLATED_UIDS = 1 shl 2
    }
}
