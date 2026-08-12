package com.anatdx.kagami

import android.os.Build
import android.os.Bundle
import android.widget.ImageView
import androidx.activity.ComponentActivity
import androidx.activity.compose.setContent
import androidx.activity.enableEdgeToEdge
import androidx.compose.foundation.background
import androidx.compose.foundation.layout.Arrangement
import androidx.compose.foundation.layout.Box
import androidx.compose.foundation.layout.Column
import androidx.compose.foundation.layout.ColumnScope
import androidx.compose.foundation.layout.ExperimentalLayoutApi
import androidx.compose.foundation.layout.FlowRow
import androidx.compose.foundation.layout.PaddingValues
import androidx.compose.foundation.layout.Row
import androidx.compose.foundation.layout.Spacer
import androidx.compose.foundation.layout.fillMaxSize
import androidx.compose.foundation.layout.fillMaxWidth
import androidx.compose.foundation.layout.height
import androidx.compose.foundation.layout.padding
import androidx.compose.foundation.layout.size
import androidx.compose.foundation.layout.width
import androidx.compose.foundation.lazy.LazyColumn
import androidx.compose.foundation.lazy.items
import androidx.compose.foundation.shape.CircleShape
import androidx.compose.foundation.shape.RoundedCornerShape
import androidx.compose.material.icons.Icons
import androidx.compose.material.icons.filled.Apps
import androidx.compose.material.icons.filled.BugReport
import androidx.compose.material.icons.filled.CheckCircle
import androidx.compose.material.icons.filled.Code
import androidx.compose.material.icons.filled.Delete
import androidx.compose.material.icons.filled.Error
import androidx.compose.material.icons.filled.Extension
import androidx.compose.material.icons.filled.ExpandLess
import androidx.compose.material.icons.filled.ExpandMore
import androidx.compose.material.icons.filled.FolderOpen
import androidx.compose.material.icons.filled.Memory
import androidx.compose.material.icons.filled.PlayArrow
import androidx.compose.material.icons.filled.PowerSettingsNew
import androidx.compose.material.icons.filled.Refresh
import androidx.compose.material.icons.filled.Security
import androidx.compose.material.icons.filled.Search
import androidx.compose.material.icons.filled.Terminal
import androidx.compose.material.icons.filled.Tune
import androidx.compose.material.icons.filled.VisibilityOff
import androidx.compose.material3.AssistChip
import androidx.compose.material3.Button
import androidx.compose.material3.CardDefaults
import androidx.compose.material3.CenterAlignedTopAppBar
import androidx.compose.material3.CircularProgressIndicator
import androidx.compose.material3.ElevatedCard
import androidx.compose.material3.ExperimentalMaterial3Api
import androidx.compose.material3.FilledTonalButton
import androidx.compose.material3.Icon
import androidx.compose.material3.IconButton
import androidx.compose.material3.LinearProgressIndicator
import androidx.compose.material3.MaterialTheme
import androidx.compose.material3.ModalBottomSheet
import androidx.compose.material3.NavigationBar
import androidx.compose.material3.NavigationBarItem
import androidx.compose.material3.OutlinedButton
import androidx.compose.material3.OutlinedTextField
import androidx.compose.material3.Scaffold
import androidx.compose.material3.SegmentedButton
import androidx.compose.material3.SegmentedButtonDefaults
import androidx.compose.material3.SingleChoiceSegmentedButtonRow
import androidx.compose.material3.Surface
import androidx.compose.material3.Switch
import androidx.compose.material3.Text
import androidx.compose.material3.TextButton
import androidx.compose.material3.TopAppBarDefaults
import androidx.compose.material3.dynamicLightColorScheme
import androidx.compose.material3.lightColorScheme
import androidx.compose.material3.rememberModalBottomSheetState
import androidx.compose.runtime.Composable
import androidx.compose.runtime.collectAsState
import androidx.compose.runtime.getValue
import androidx.compose.runtime.mutableIntStateOf
import androidx.compose.runtime.mutableStateOf
import androidx.compose.runtime.remember
import androidx.compose.runtime.saveable.rememberSaveable
import androidx.compose.runtime.setValue
import androidx.compose.ui.Alignment
import androidx.compose.ui.Modifier
import androidx.compose.ui.graphics.Color
import androidx.compose.ui.graphics.vector.ImageVector
import androidx.compose.ui.platform.LocalContext
import androidx.compose.ui.res.stringResource
import androidx.compose.ui.viewinterop.AndroidView
import androidx.compose.ui.text.font.FontFamily
import androidx.compose.ui.text.font.FontWeight
import androidx.compose.ui.text.style.TextOverflow
import androidx.compose.ui.unit.dp
import androidx.lifecycle.viewmodel.compose.viewModel

class MainActivity : ComponentActivity() {
    override fun onCreate(savedInstanceState: Bundle?) {
        super.onCreate(savedInstanceState)
        enableEdgeToEdge()
        setContent {
            KagamiTheme {
                KagamiApp()
            }
        }
    }
}

@Composable
private fun KagamiTheme(content: @Composable () -> Unit) {
    val colorScheme = if (Build.VERSION.SDK_INT >= Build.VERSION_CODES.S) {
        dynamicLightColorScheme(LocalContext.current)
    } else {
        lightColorScheme()
    }
    MaterialTheme(
        colorScheme = colorScheme,
        content = content,
    )
}

private data class AppTab(
    val titleRes: Int,
    val icon: ImageVector,
)

private const val FEATURE_MOUNT_HIDE = 1 shl 6
private const val FEATURE_MAPS_SPOOF = 1 shl 7
private const val FEATURE_STATFS_SPOOF = 1 shl 8
private const val FEATURE_SELINUX_GUARD = 1 shl 10

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun KagamiApp(viewModel: MainViewModel = viewModel()) {
    val state by viewModel.state.collectAsState()
    var selectedTab by remember { mutableIntStateOf(0) }
    val tabs = listOf(
        AppTab(R.string.tab_status, Icons.Default.Tune),
        AppTab(R.string.tab_control, Icons.Default.Security),
        AppTab(R.string.tab_modules, Icons.Default.Extension),
        AppTab(R.string.tab_logs, Icons.Default.Code),
        AppTab(R.string.tab_console, Icons.Default.Terminal),
    )

    if (state.appPicker.visible) {
        AppPickerSheet(
            picker = state.appPicker,
            onDismiss = viewModel::closeAppPicker,
            onQueryChange = viewModel::setAppPickerQuery,
            onIncludeSystemChange = viewModel::setAppPickerIncludeSystem,
            onChoiceChange = viewModel::setAppChoice,
            onApply = viewModel::applyAppPickerSelection,
            onReload = viewModel::loadApps,
        )
    }

    Scaffold(
        topBar = {
            CenterAlignedTopAppBar(
                title = {
                    Column(horizontalAlignment = Alignment.CenterHorizontally) {
                        Text(stringResource(R.string.app_name), fontWeight = FontWeight.SemiBold)
                        Text(
                            stringResource(R.string.app_subtitle),
                            style = MaterialTheme.typography.labelSmall,
                            color = MaterialTheme.colorScheme.onSurfaceVariant,
                        )
                    }
                },
                actions = {
                    IconButton(onClick = viewModel::refresh, enabled = !state.loading) {
                        Icon(Icons.Default.Refresh, contentDescription = stringResource(R.string.action_refresh))
                    }
                },
                colors = TopAppBarDefaults.topAppBarColors(
                    containerColor = MaterialTheme.colorScheme.surface,
                ),
            )
        },
        bottomBar = {
            NavigationBar {
                tabs.forEachIndexed { index, tab ->
                    NavigationBarItem(
                        selected = selectedTab == index,
                        onClick = { selectedTab = index },
                        icon = { Icon(tab.icon, contentDescription = stringResource(tab.titleRes)) },
                        label = { Text(stringResource(tab.titleRes)) },
                    )
                }
            }
        },
    ) { padding ->
        Surface(
            modifier = Modifier
                .fillMaxSize()
                .padding(padding),
            color = MaterialTheme.colorScheme.background,
        ) {
            Column {
                if (state.loading) {
                    LinearProgressIndicator(Modifier.fillMaxWidth())
                }
                when (selectedTab) {
                    0 -> StatusScreen(
                        state = state,
                        onRefreshDaemon = viewModel::refreshDaemon,
                        onStartDaemon = viewModel::startDaemon,
                    )
                    1 -> ControlScreen(
                        state = state,
                        onSetKasumiEnabled = viewModel::setKasumiEnabled,
                        onSetDebug = viewModel::setDebug,
                        onSetMountHide = viewModel::setMountHide,
                        onSetMapsSpoof = viewModel::setMapsSpoof,
                        onSetStatfsSpoof = viewModel::setStatfsSpoof,
                        onSetSelinuxGuard = viewModel::setSelinuxGuard,
                        onNativePathChange = viewModel::setNativePath,
                        onHideNativePath = viewModel::hideNativePath,
                        onDeleteNativeRule = viewModel::deleteNativeRule,
                        onMapsTargetInoChange = viewModel::setMapsTargetIno,
                        onMapsTargetDevChange = viewModel::setMapsTargetDev,
                        onMapsSpoofedInoChange = viewModel::setMapsSpoofedIno,
                        onMapsSpoofedDevChange = viewModel::setMapsSpoofedDev,
                        onMapsSpoofedPathChange = viewModel::setMapsSpoofedPath,
                        onMapsResolvePathChange = viewModel::setMapsResolvePath,
                        onMapsResolveByPath = viewModel::resolveMapsTargetByPath,
                        onMapsPresetLib = viewModel::applyMapsPresetLib,
                        onMapsPresetApk = viewModel::applyMapsPresetApk,
                        onAddMapsRule = viewModel::addMapsRule,
                        onClearMapsRules = viewModel::clearMapsRules,
                        onPolicyOwnerChange = viewModel::setPolicyOwner,
                        onPolicyAllowUidsChange = viewModel::setPolicyAllowUids,
                        onPolicyDenyUidsChange = viewModel::setPolicyDenyUids,
                        onPolicyIncludeIsolatedChange = viewModel::setPolicyIncludeIsolated,
                        onApplyManualPolicy = viewModel::applyManualPolicy,
                        onClearManualPolicy = viewModel::clearManualPolicy,
                        onOpenAppPicker = viewModel::openAppPicker,
                    )
                    2 -> ModulesScreen(state.snapshot.modules)
                    3 -> LogsScreen(state, viewModel::refreshKernelLog)
                    else -> ConsoleScreen(state, viewModel::setCommand, viewModel::runCommand)
                }
            }
        }
    }
}

@Composable
private fun StatusScreen(
    state: ManagerUiState,
    onRefreshDaemon: () -> Unit,
    onStartDaemon: () -> Unit,
) {
    val native = state.nativeSnapshot
    val snapshot = state.snapshot
    LazyColumn(
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            OverviewCard(
                native = native,
                daemon = snapshot.status,
                lkm = snapshot.lkm,
                onRefreshDaemon = onRefreshDaemon,
                onStartDaemon = onStartDaemon,
            )
        }
        item {
            FeatureCard(
                featureBitmask = native.featureBitmask,
                features = snapshot.status.featureNames.ifEmpty { native.featureNames },
                hooks = snapshot.status.hooks.ifBlank { native.hooks },
            )
        }
        item { PolicySummaryCard(policy = native.policy) }
        item { DiagnosticsCard(native = native, snapshot = snapshot) }
        item {
            ExpandableRawCard(
                title = stringResource(R.string.card_native_json),
                body = native.rawJson,
            )
        }
        item {
            ExpandableRawCard(
                title = stringResource(R.string.card_system_json),
                body = snapshot.rawSystemJson,
            )
        }
    }
}

@Composable
private fun PolicySummaryCard(policy: NativeKasumiPolicy) {
    ManagerCard {
        SectionHeader(
            icon = Icons.Default.Tune,
            title = stringResource(R.string.card_policy_summary),
            subtitle = stringResource(R.string.card_policy_summary_desc),
        )
        Spacer(Modifier.height(12.dp))
        if (!policy.ok) {
            Text(
                stringResource(R.string.policy_summary_unavailable),
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                style = MaterialTheme.typography.bodySmall,
            )
            return@ManagerCard
        }
        KeyValueGrid(
            items = listOf(
                stringResource(R.string.label_policy_owner) to policy.owner,
                stringResource(R.string.label_policy_effective_owner) to policy.effectiveOwner,
                stringResource(R.string.label_policy_allow_count) to "${policy.allowCount}/${policy.maxUidCount}",
                stringResource(R.string.label_policy_deny_count) to "${policy.denyCount}/${policy.maxUidCount}",
                stringResource(R.string.label_policy_flags) to "0x${policy.flags.toString(16)}",
                stringResource(R.string.label_policy_roots) to "0x${policy.detectedRoots.toString(16)}",
            ),
        )
        if (policy.allowUids.isNotEmpty() || policy.denyUids.isNotEmpty()) {
            Spacer(Modifier.height(10.dp))
            if (policy.allowUids.isNotEmpty()) {
                ChipRow(policy.allowUids.take(12).map { "allow $it" })
            }
            if (policy.denyUids.isNotEmpty()) {
                Spacer(Modifier.height(6.dp))
                ChipRow(policy.denyUids.take(12).map { "deny $it" })
            }
        }
    }
}

@Composable
private fun DiagnosticsCard(native: NativeKasumiSnapshot, snapshot: KagamiSnapshot) {
    ManagerCard {
        SectionHeader(
            icon = Icons.Default.BugReport,
            title = stringResource(R.string.card_diagnostics),
            subtitle = stringResource(R.string.card_diagnostics_desc),
        )
        Spacer(Modifier.height(12.dp))
        KeyValueGrid(
            items = listOf(
                stringResource(R.string.label_kernel) to snapshot.status.kernel.ifBlank { stringResource(R.string.value_unknown) },
                stringResource(R.string.label_selinux) to snapshot.status.selinux.ifBlank { stringResource(R.string.value_unknown) },
                stringResource(R.string.label_modules_visible) to if (native.modulesVisible) stringResource(R.string.value_yes) else stringResource(R.string.value_no),
                stringResource(R.string.label_lkm_autoload) to if (snapshot.lkm.autoload) stringResource(R.string.value_on) else stringResource(R.string.value_off),
                stringResource(R.string.label_lkm_kmi) to snapshot.lkm.kmiOverride.ifBlank { stringResource(R.string.value_unknown) },
                stringResource(R.string.label_module_count) to snapshot.modules.size.toString(),
            ),
        )
        if (native.lastErrno != 0 || native.lastError.isNotBlank()) {
            Spacer(Modifier.height(10.dp))
            Text(
                stringResource(R.string.diagnostics_last_error, native.lastError.ifBlank { native.lastErrno.toString() }),
                color = MaterialTheme.colorScheme.error,
                style = MaterialTheme.typography.bodySmall,
            )
        }
    }
}

@Composable
private fun ExpandableRawCard(title: String, body: String) {
    var expanded by rememberSaveable(title) { mutableStateOf(false) }
    ManagerCard {
        Row(
            modifier = Modifier.fillMaxWidth(),
            verticalAlignment = Alignment.CenterVertically,
        ) {
            Text(
                title,
                modifier = Modifier.weight(1f),
                style = MaterialTheme.typography.titleSmall,
                fontWeight = FontWeight.SemiBold,
            )
            IconButton(onClick = { expanded = !expanded }) {
                Icon(
                    if (expanded) Icons.Default.ExpandLess else Icons.Default.ExpandMore,
                    contentDescription = title,
                )
            }
        }
        if (expanded) {
            Spacer(Modifier.height(6.dp))
            CodeBlock(body.ifBlank { "-" })
        }
    }
}

@OptIn(ExperimentalLayoutApi::class)
@Composable
private fun OverviewCard(
    native: NativeKasumiSnapshot,
    daemon: KasumiStatus,
    lkm: LkmStatus,
    onRefreshDaemon: () -> Unit,
    onStartDaemon: () -> Unit,
) {
    ManagerCard {
        Row(verticalAlignment = Alignment.CenterVertically) {
            StatusOrb(native.available)
            Spacer(Modifier.width(12.dp))
            Column(Modifier.weight(1f)) {
                Text(stringResource(R.string.card_overview), style = MaterialTheme.typography.titleLarge, fontWeight = FontWeight.SemiBold)
                Text(
                    stringResource(R.string.overview_caption),
                    style = MaterialTheme.typography.bodySmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                FilledTonalButton(onClick = onStartDaemon) {
                    Icon(Icons.Default.PlayArrow, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.action_start_daemon))
                }
                OutlinedButton(onClick = onRefreshDaemon) {
                    Icon(Icons.Default.Refresh, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.action_probe))
                }
            }
        }
        Spacer(Modifier.height(14.dp))
        FlowRow(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalArrangement = Arrangement.spacedBy(8.dp),
            modifier = Modifier.fillMaxWidth(),
        ) {
            StatusPill(
                label = if (native.available) stringResource(R.string.status_native_ready) else stringResource(R.string.status_native_unavailable),
                good = native.available,
            )
            StatusPill(
                label = if (daemon.available) stringResource(R.string.status_daemon_ready) else stringResource(R.string.status_daemon_unknown),
                good = daemon.available,
            )
            StatusPill(
                label = if (lkm.loaded) stringResource(R.string.status_lkm_loaded) else stringResource(R.string.status_lkm_missing),
                good = lkm.loaded,
            )
        }
        Spacer(Modifier.height(14.dp))
        KeyValueGrid(
            items = listOf(
                stringResource(R.string.label_kernel) to daemon.kernel,
                stringResource(R.string.label_selinux) to daemon.selinux,
                stringResource(R.string.label_mirror) to native.mountBase.ifBlank { daemon.mountBase.ifBlank { stringResource(R.string.value_unknown) } },
                stringResource(R.string.label_protocol) to "${native.kernelProtocol}/${native.expectedProtocol}",
                stringResource(R.string.label_uid) to "${native.uid} / ${native.euid}",
                stringResource(R.string.label_autoload) to if (lkm.autoload) stringResource(R.string.value_on) else stringResource(R.string.value_off),
            ),
        )
    }
}

@Composable
private fun ControlScreen(
    state: ManagerUiState,
    onSetKasumiEnabled: (Boolean) -> Unit,
    onSetDebug: (Boolean) -> Unit,
    onSetMountHide: (Boolean) -> Unit,
    onSetMapsSpoof: (Boolean) -> Unit,
    onSetStatfsSpoof: (Boolean) -> Unit,
    onSetSelinuxGuard: (Boolean) -> Unit,
    onNativePathChange: (String) -> Unit,
    onHideNativePath: () -> Unit,
    onDeleteNativeRule: () -> Unit,
    onMapsTargetInoChange: (String) -> Unit,
    onMapsTargetDevChange: (String) -> Unit,
    onMapsSpoofedInoChange: (String) -> Unit,
    onMapsSpoofedDevChange: (String) -> Unit,
    onMapsSpoofedPathChange: (String) -> Unit,
    onMapsResolvePathChange: (String) -> Unit,
    onMapsResolveByPath: () -> Unit,
    onMapsPresetLib: () -> Unit,
    onMapsPresetApk: () -> Unit,
    onAddMapsRule: () -> Unit,
    onClearMapsRules: () -> Unit,
    onPolicyOwnerChange: (String) -> Unit,
    onPolicyAllowUidsChange: (String) -> Unit,
    onPolicyDenyUidsChange: (String) -> Unit,
    onPolicyIncludeIsolatedChange: (Boolean) -> Unit,
    onApplyManualPolicy: () -> Unit,
    onClearManualPolicy: () -> Unit,
    onOpenAppPicker: () -> Unit,
) {
    LazyColumn(
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            DirectKasumiCard(
                native = state.nativeSnapshot,
                actionResult = state.nativeActionResult,
                error = state.error,
                kernelDebugEnabled = state.kernelDebugEnabled,
                nativePath = state.nativePath,
                onSetKasumiEnabled = onSetKasumiEnabled,
                onSetDebug = onSetDebug,
                onSetMountHide = onSetMountHide,
                onSetMapsSpoof = onSetMapsSpoof,
                onSetStatfsSpoof = onSetStatfsSpoof,
                onSetSelinuxGuard = onSetSelinuxGuard,
                onNativePathChange = onNativePathChange,
                onHideNativePath = onHideNativePath,
                onDeleteNativeRule = onDeleteNativeRule,
            )
        }
        item {
            ManualPolicyCard(
                state = state,
                onPolicyOwnerChange = onPolicyOwnerChange,
                onPolicyAllowUidsChange = onPolicyAllowUidsChange,
                onPolicyDenyUidsChange = onPolicyDenyUidsChange,
                onPolicyIncludeIsolatedChange = onPolicyIncludeIsolatedChange,
                onApplyManualPolicy = onApplyManualPolicy,
                onClearManualPolicy = onClearManualPolicy,
                onOpenAppPicker = onOpenAppPicker,
            )
        }
        item {
            MapsSpoofCard(
                state = state,
                onMapsTargetInoChange = onMapsTargetInoChange,
                onMapsTargetDevChange = onMapsTargetDevChange,
                onMapsSpoofedInoChange = onMapsSpoofedInoChange,
                onMapsSpoofedDevChange = onMapsSpoofedDevChange,
                onMapsSpoofedPathChange = onMapsSpoofedPathChange,
                onMapsResolvePathChange = onMapsResolvePathChange,
                onMapsResolveByPath = onMapsResolveByPath,
                onMapsPresetLib = onMapsPresetLib,
                onMapsPresetApk = onMapsPresetApk,
                onAddMapsRule = onAddMapsRule,
                onClearMapsRules = onClearMapsRules,
            )
        }
    }
}

@Composable
private fun ManualPolicyCard(
    state: ManagerUiState,
    onPolicyOwnerChange: (String) -> Unit,
    onPolicyAllowUidsChange: (String) -> Unit,
    onPolicyDenyUidsChange: (String) -> Unit,
    onPolicyIncludeIsolatedChange: (Boolean) -> Unit,
    onApplyManualPolicy: () -> Unit,
    onClearManualPolicy: () -> Unit,
    onOpenAppPicker: () -> Unit,
) {
    val policy = state.nativeSnapshot.policy
    val pickerSummary = appPickerSummary(state.appPicker.choices)
    ManagerCard {
        SectionHeader(
            icon = Icons.Default.Tune,
            title = stringResource(R.string.card_policy),
            subtitle = stringResource(R.string.card_policy_desc),
        )
        Spacer(Modifier.height(12.dp))
        KeyValueGrid(
            items = listOf(
                stringResource(R.string.label_policy_owner) to policy.owner,
                stringResource(R.string.label_policy_effective_owner) to policy.effectiveOwner,
                stringResource(R.string.label_policy_flags) to "0x${policy.flags.toString(16)}",
                stringResource(R.string.label_policy_roots) to "0x${policy.detectedRoots.toString(16)}",
            ),
        )
        Spacer(Modifier.height(10.dp))
        FilledTonalButton(onClick = onOpenAppPicker, modifier = Modifier.fillMaxWidth()) {
            Icon(Icons.Default.Apps, contentDescription = null, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(8.dp))
            Column(Modifier.weight(1f), horizontalAlignment = Alignment.Start) {
                Text(stringResource(R.string.action_pick_apps), fontWeight = FontWeight.SemiBold)
                Text(
                    pickerSummary,
                    style = MaterialTheme.typography.labelSmall,
                    color = MaterialTheme.colorScheme.onSurfaceVariant,
                )
            }
        }
        Spacer(Modifier.height(10.dp))
        CompactTextField(state.policyOwner, onPolicyOwnerChange, stringResource(R.string.label_policy_owner_input), Modifier.fillMaxWidth())
        CompactTextField(state.policyAllowUids, onPolicyAllowUidsChange, stringResource(R.string.label_policy_allow_uids), Modifier.fillMaxWidth())
        CompactTextField(state.policyDenyUids, onPolicyDenyUidsChange, stringResource(R.string.label_policy_deny_uids), Modifier.fillMaxWidth())
        SettingsSwitchRow(stringResource(R.string.control_policy_isolated), state.policyIncludeIsolated, onPolicyIncludeIsolatedChange)
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            Button(onClick = onApplyManualPolicy, modifier = Modifier.weight(1f)) {
                Icon(Icons.Default.CheckCircle, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.action_apply_policy))
            }
            OutlinedButton(onClick = onClearManualPolicy, modifier = Modifier.weight(1f)) {
                Icon(Icons.Default.Delete, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.action_reset_policy))
            }
        }
        Spacer(Modifier.height(8.dp))
        ChipRow(
            listOf(
                "allow=${policy.allowUids.joinToString(",")}",
                "deny=${policy.denyUids.joinToString(",")}",
            ),
        )
    }
}

@Composable
private fun appPickerSummary(choices: Map<String, AppPolicyChoice>): String {
    val allow = choices.count { it.value == AppPolicyChoice.ALLOW }
    val deny = choices.count { it.value == AppPolicyChoice.DENY }
    return stringResource(R.string.apps_picked_summary, allow, deny)
}

@Composable
private fun DirectKasumiCard(
    native: NativeKasumiSnapshot,
    actionResult: NativeActionResult?,
    error: String?,
    kernelDebugEnabled: Boolean,
    nativePath: String,
    onSetKasumiEnabled: (Boolean) -> Unit,
    onSetDebug: (Boolean) -> Unit,
    onSetMountHide: (Boolean) -> Unit,
    onSetMapsSpoof: (Boolean) -> Unit,
    onSetStatfsSpoof: (Boolean) -> Unit,
    onSetSelinuxGuard: (Boolean) -> Unit,
    onNativePathChange: (String) -> Unit,
    onHideNativePath: () -> Unit,
    onDeleteNativeRule: () -> Unit,
) {
    ManagerCard {
        SectionHeader(
            icon = Icons.Default.Security,
            title = stringResource(R.string.card_direct_kasumi),
            subtitle = stringResource(R.string.card_direct_kasumi_desc),
        )
        Spacer(Modifier.height(12.dp))
        InfoRow(stringResource(R.string.label_status), nativeStatusLabel(native.status))
        InfoRow(stringResource(R.string.label_get_fd), nativeGetFdLabel(native))
        Spacer(Modifier.height(8.dp))
        NativeControlRow(stringResource(R.string.control_kasumi_runtime), onEnable = { onSetKasumiEnabled(true) }, onDisable = { onSetKasumiEnabled(false) })
        SettingsSwitchRow(stringResource(R.string.control_kernel_log), kernelDebugEnabled, onSetDebug)
        SettingsSwitchRow(stringResource(R.string.control_mount_hide), native.featureBitmask hasFeature FEATURE_MOUNT_HIDE, onSetMountHide)
        SettingsSwitchRow(stringResource(R.string.control_maps_spoof), native.featureBitmask hasFeature FEATURE_MAPS_SPOOF, onSetMapsSpoof)
        SettingsSwitchRow(stringResource(R.string.control_statfs_spoof), native.featureBitmask hasFeature FEATURE_STATFS_SPOOF, onSetStatfsSpoof)
        SettingsSwitchRow(stringResource(R.string.control_selinux_guard), native.featureBitmask hasFeature FEATURE_SELINUX_GUARD, onSetSelinuxGuard)
        Spacer(Modifier.height(8.dp))
        OutlinedTextField(
            value = nativePath,
            onValueChange = onNativePathChange,
            modifier = Modifier.fillMaxWidth(),
            singleLine = true,
            leadingIcon = { Icon(Icons.Default.Code, contentDescription = null) },
            label = { Text(stringResource(R.string.label_kasumi_path_rule)) },
        )
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            Button(onClick = onHideNativePath, modifier = Modifier.weight(1f)) {
                Icon(Icons.Default.VisibilityOff, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.action_hide))
            }
            OutlinedButton(onClick = onDeleteNativeRule, modifier = Modifier.weight(1f)) {
                Icon(Icons.Default.Delete, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.action_delete))
            }
        }
        ActionMessage(actionResult, error)
    }
}

@Composable
private fun MapsSpoofCard(
    state: ManagerUiState,
    onMapsTargetInoChange: (String) -> Unit,
    onMapsTargetDevChange: (String) -> Unit,
    onMapsSpoofedInoChange: (String) -> Unit,
    onMapsSpoofedDevChange: (String) -> Unit,
    onMapsSpoofedPathChange: (String) -> Unit,
    onMapsResolvePathChange: (String) -> Unit,
    onMapsResolveByPath: () -> Unit,
    onMapsPresetLib: () -> Unit,
    onMapsPresetApk: () -> Unit,
    onAddMapsRule: () -> Unit,
    onClearMapsRules: () -> Unit,
) {
    ManagerCard {
        SectionHeader(
            icon = Icons.Default.Memory,
            title = stringResource(R.string.card_maps_spoof),
            subtitle = stringResource(R.string.card_maps_spoof_desc),
        )
        Spacer(Modifier.height(12.dp))
        Text(
            stringResource(R.string.maps_picker_title),
            style = MaterialTheme.typography.titleSmall,
            fontWeight = FontWeight.SemiBold,
        )
        Text(
            stringResource(R.string.maps_picker_desc),
            style = MaterialTheme.typography.bodySmall,
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Spacer(Modifier.height(8.dp))
        Row(
            horizontalArrangement = Arrangement.spacedBy(8.dp),
            verticalAlignment = Alignment.CenterVertically,
            modifier = Modifier.fillMaxWidth(),
        ) {
            CompactTextField(
                value = state.mapsResolve.pathInput,
                onValueChange = onMapsResolvePathChange,
                label = stringResource(R.string.maps_picker_path_hint),
                modifier = Modifier.weight(1f),
            )
            Button(onClick = onMapsResolveByPath, enabled = !state.mapsResolve.resolving) {
                if (state.mapsResolve.resolving) {
                    CircularProgressIndicator(modifier = Modifier.size(16.dp), strokeWidth = 2.dp)
                } else {
                    Icon(Icons.Default.FolderOpen, contentDescription = null, modifier = Modifier.size(18.dp))
                }
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.action_resolve_path))
            }
        }
        state.mapsResolve.message?.let { msg ->
            Spacer(Modifier.height(4.dp))
            Text(
                msg,
                style = MaterialTheme.typography.bodySmall,
                color = if (state.mapsResolve.isError) MaterialTheme.colorScheme.error else MaterialTheme.colorScheme.primary,
            )
        }
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            CompactTextField(state.mapsTargetIno, onMapsTargetInoChange, stringResource(R.string.label_target_ino), Modifier.weight(1f))
            CompactTextField(state.mapsTargetDev, onMapsTargetDevChange, stringResource(R.string.label_target_dev), Modifier.weight(1f))
        }
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            CompactTextField(state.mapsSpoofedIno, onMapsSpoofedInoChange, stringResource(R.string.label_spoofed_ino), Modifier.weight(1f))
            CompactTextField(state.mapsSpoofedDev, onMapsSpoofedDevChange, stringResource(R.string.label_spoofed_dev), Modifier.weight(1f))
        }
        CompactTextField(state.mapsSpoofedPath, onMapsSpoofedPathChange, stringResource(R.string.label_spoofed_path), Modifier.fillMaxWidth())
        Spacer(Modifier.height(8.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            OutlinedButton(onClick = onMapsPresetLib, modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.action_maps_preset_lib), maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
            OutlinedButton(onClick = onMapsPresetApk, modifier = Modifier.weight(1f)) {
                Text(stringResource(R.string.action_maps_preset_apk), maxLines = 1, overflow = TextOverflow.Ellipsis)
            }
        }
        Spacer(Modifier.height(10.dp))
        Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
            Button(onClick = onAddMapsRule, modifier = Modifier.weight(1f)) {
                Icon(Icons.Default.CheckCircle, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.action_add_rule))
            }
            OutlinedButton(onClick = onClearMapsRules, modifier = Modifier.weight(1f)) {
                Icon(Icons.Default.Delete, contentDescription = null, modifier = Modifier.size(18.dp))
                Spacer(Modifier.width(8.dp))
                Text(stringResource(R.string.action_clear))
            }
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AppPickerSheet(
    picker: AppPickerState,
    onDismiss: () -> Unit,
    onQueryChange: (String) -> Unit,
    onIncludeSystemChange: (Boolean) -> Unit,
    onChoiceChange: (String, AppPolicyChoice) -> Unit,
    onApply: () -> Unit,
    onReload: () -> Unit,
) {
    val sheetState = rememberModalBottomSheetState(skipPartiallyExpanded = true)
    val filtered = remember(picker.apps, picker.query, picker.includeSystem) {
        val q = picker.query.trim().lowercase()
        picker.apps.asSequence()
            .filter { picker.includeSystem || !it.isSystem }
            .filter { q.isEmpty() || it.label.lowercase().contains(q) || it.packageName.lowercase().contains(q) }
            .toList()
    }
    ModalBottomSheet(onDismissRequest = onDismiss, sheetState = sheetState) {
        Column(modifier = Modifier.padding(horizontal = 16.dp)) {
            Row(verticalAlignment = Alignment.CenterVertically) {
                Text(
                    stringResource(R.string.apps_select_label),
                    style = MaterialTheme.typography.titleMedium,
                    fontWeight = FontWeight.SemiBold,
                    modifier = Modifier.weight(1f),
                )
                IconButton(onClick = onReload, enabled = !picker.loading) {
                    Icon(Icons.Default.Refresh, contentDescription = stringResource(R.string.action_refresh))
                }
            }
            Spacer(Modifier.height(8.dp))
            OutlinedTextField(
                value = picker.query,
                onValueChange = onQueryChange,
                modifier = Modifier.fillMaxWidth(),
                singleLine = true,
                leadingIcon = { Icon(Icons.Default.Search, contentDescription = null) },
                label = { Text(stringResource(R.string.apps_search_hint)) },
            )
            Spacer(Modifier.height(8.dp))
            SettingsSwitchRow(
                label = stringResource(R.string.apps_filter_system),
                checked = picker.includeSystem,
                onCheckedChange = onIncludeSystemChange,
            )
            Text(
                appPickerSummary(picker.choices),
                style = MaterialTheme.typography.labelMedium,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
            )
            Spacer(Modifier.height(8.dp))
            when {
                picker.loading && picker.apps.isEmpty() -> {
                    Box(modifier = Modifier.fillMaxWidth().padding(vertical = 24.dp), contentAlignment = Alignment.Center) {
                        Column(horizontalAlignment = Alignment.CenterHorizontally) {
                            CircularProgressIndicator()
                            Spacer(Modifier.height(8.dp))
                            Text(stringResource(R.string.apps_loading), color = MaterialTheme.colorScheme.onSurfaceVariant)
                        }
                    }
                }
                filtered.isEmpty() -> {
                    Text(
                        stringResource(R.string.apps_empty),
                        modifier = Modifier.padding(vertical = 16.dp),
                        color = MaterialTheme.colorScheme.onSurfaceVariant,
                    )
                }
                else -> {
                    LazyColumn(
                        modifier = Modifier.weight(1f, fill = false).fillMaxWidth(),
                        verticalArrangement = Arrangement.spacedBy(4.dp),
                    ) {
                        items(filtered, key = { it.packageName }) { app ->
                            val choice = picker.choices[app.packageName] ?: AppPolicyChoice.OFF
                            AppPickerRow(
                                app = app,
                                choice = choice,
                                onChoiceChange = { onChoiceChange(app.packageName, it) },
                            )
                        }
                    }
                }
            }
            Spacer(Modifier.height(12.dp))
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                OutlinedButton(onClick = onDismiss, modifier = Modifier.weight(1f)) {
                    Text(stringResource(R.string.apps_select_done))
                }
                Button(onClick = onApply, modifier = Modifier.weight(1f)) {
                    Icon(Icons.Default.CheckCircle, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(6.dp))
                    Text(stringResource(R.string.action_apply_app_picker))
                }
            }
            Spacer(Modifier.height(16.dp))
        }
    }
}

@OptIn(ExperimentalMaterial3Api::class)
@Composable
private fun AppPickerRow(app: AppEntry, choice: AppPolicyChoice, onChoiceChange: (AppPolicyChoice) -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 4.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Box(
            modifier = Modifier
                .size(36.dp)
                .background(MaterialTheme.colorScheme.surfaceContainerHigh, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            if (app.icon != null) {
                AndroidView(
                    factory = { ctx ->
                        ImageView(ctx).apply {
                            scaleType = ImageView.ScaleType.FIT_CENTER
                            setImageDrawable(app.icon)
                        }
                    },
                    modifier = Modifier.size(28.dp),
                )
            } else {
                Icon(Icons.Default.Apps, contentDescription = null, tint = MaterialTheme.colorScheme.onSurfaceVariant)
            }
        }
        Spacer(Modifier.width(10.dp))
        Column(Modifier.weight(1f)) {
            Text(app.label, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
            Text(
                "${app.packageName}  ·  uid ${app.uid}",
                style = MaterialTheme.typography.labelSmall,
                color = MaterialTheme.colorScheme.onSurfaceVariant,
                maxLines = 1,
                overflow = TextOverflow.Ellipsis,
            )
        }
        Spacer(Modifier.width(8.dp))
        val options = listOf(
            AppPolicyChoice.OFF to stringResource(R.string.apps_state_off),
            AppPolicyChoice.ALLOW to stringResource(R.string.apps_state_allow),
            AppPolicyChoice.DENY to stringResource(R.string.apps_state_deny),
        )
        SingleChoiceSegmentedButtonRow {
            options.forEachIndexed { index, (value, label) ->
                SegmentedButton(
                    selected = choice == value,
                    onClick = { onChoiceChange(value) },
                    shape = SegmentedButtonDefaults.itemShape(index = index, count = options.size),
                ) {
                    Text(label, style = MaterialTheme.typography.labelSmall)
                }
            }
        }
    }
}

@Composable
private fun NativeControlRow(label: String, onEnable: () -> Unit, onDisable: () -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 3.dp),
        horizontalArrangement = Arrangement.spacedBy(8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            modifier = Modifier.weight(1.25f),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        FilledTonalButton(onClick = onEnable, modifier = Modifier.weight(1f)) {
            Icon(Icons.Default.CheckCircle, contentDescription = null, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(6.dp))
            Text(stringResource(R.string.value_on))
        }
        OutlinedButton(onClick = onDisable, modifier = Modifier.weight(1f)) {
            Icon(Icons.Default.PowerSettingsNew, contentDescription = null, modifier = Modifier.size(18.dp))
            Spacer(Modifier.width(6.dp))
            Text(stringResource(R.string.value_off))
        }
    }
}

@Composable
private fun SettingsSwitchRow(label: String, checked: Boolean, onCheckedChange: (Boolean) -> Unit) {
    Row(
        modifier = Modifier
            .fillMaxWidth()
            .padding(vertical = 5.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(
            text = label,
            modifier = Modifier.weight(1f),
            color = MaterialTheme.colorScheme.onSurfaceVariant,
        )
        Switch(checked = checked, onCheckedChange = onCheckedChange)
    }
}

@Composable
private fun FeatureCard(featureBitmask: Int, features: List<String>, hooks: String) {
    ManagerCard {
        SectionHeader(
            icon = Icons.Default.Memory,
            title = stringResource(R.string.card_features),
            subtitle = stringResource(R.string.card_features_desc),
        )
        Spacer(Modifier.height(12.dp))
        InfoRow(stringResource(R.string.label_feature_mask), "0x${featureBitmask.toString(16)}")
        if (features.isEmpty()) {
            Text(stringResource(R.string.empty_features), color = MaterialTheme.colorScheme.onSurfaceVariant)
        } else {
            ChipRow(features.take(10))
        }
        Spacer(Modifier.height(12.dp))
        Text(stringResource(R.string.label_hooks), style = MaterialTheme.typography.titleSmall, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(6.dp))
        CodeBlock(hooks.ifBlank { stringResource(R.string.empty_hooks) })
    }
}

@Composable
private fun LogsScreen(state: ManagerUiState, onRefreshKernelLog: () -> Unit) {
    LazyColumn(
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            ManagerCard {
                SectionHeader(
                    icon = Icons.Default.Code,
                    title = stringResource(R.string.card_kernel_log),
                    subtitle = stringResource(R.string.card_kernel_log_desc),
                )
                Spacer(Modifier.height(12.dp))
                Button(onClick = onRefreshKernelLog, enabled = !state.loading) {
                    Icon(Icons.Default.Refresh, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.action_refresh))
                }
            }
        }
        item {
            RawCard(stringResource(R.string.card_kernel_log_output), state.kernelLog)
        }
    }
}

@Composable
private fun ModulesScreen(modules: List<ModuleInfo>) {
    LazyColumn(
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        if (modules.isEmpty()) {
            item {
                EmptyState(
                    title = stringResource(R.string.empty_modules_title),
                    body = stringResource(R.string.empty_modules_body),
                )
            }
        } else {
            items(modules, key = { it.id }) { module ->
                ManagerCard {
                    Row(verticalAlignment = Alignment.CenterVertically) {
                        Box(
                            modifier = Modifier
                                .size(40.dp)
                                .background(MaterialTheme.colorScheme.primaryContainer, CircleShape),
                            contentAlignment = Alignment.Center,
                        ) {
                            Icon(Icons.Default.Extension, contentDescription = null, tint = MaterialTheme.colorScheme.onPrimaryContainer)
                        }
                        Spacer(Modifier.width(12.dp))
                        Column(Modifier.weight(1f)) {
                            Text(module.name, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
                            Text(module.id, fontFamily = FontFamily.Monospace, style = MaterialTheme.typography.bodySmall)
                        }
                    }
                    if (module.description.isNotBlank()) {
                        Spacer(Modifier.height(10.dp))
                        Text(module.description, maxLines = 2, overflow = TextOverflow.Ellipsis)
                    }
                    Spacer(Modifier.height(10.dp))
                    ChipRow(listOf(module.mode, module.strategy, module.version).filter { it.isNotBlank() })
                }
            }
        }
    }
}

@Composable
private fun ConsoleScreen(
    state: ManagerUiState,
    onCommandChange: (String) -> Unit,
    onRunCommand: () -> Unit,
) {
    LazyColumn(
        contentPadding = PaddingValues(16.dp),
        verticalArrangement = Arrangement.spacedBy(12.dp),
    ) {
        item {
            ManagerCard {
                SectionHeader(
                    icon = Icons.Default.Terminal,
                    title = stringResource(R.string.card_console),
                    subtitle = stringResource(R.string.card_console_desc),
                )
                Spacer(Modifier.height(12.dp))
                OutlinedTextField(
                    value = state.command,
                    onValueChange = onCommandChange,
                    modifier = Modifier.fillMaxWidth(),
                    singleLine = true,
                    leadingIcon = { Icon(Icons.Default.Code, contentDescription = null) },
                    label = { Text(stringResource(R.string.label_arguments)) },
                )
                Spacer(Modifier.height(10.dp))
                Button(onClick = onRunCommand, enabled = !state.loading) {
                    Icon(Icons.Default.PlayArrow, contentDescription = null, modifier = Modifier.size(18.dp))
                    Spacer(Modifier.width(8.dp))
                    Text(stringResource(R.string.action_run))
                }
            }
        }
        item {
            val result = state.commandResult
            RawCard(
                title = if (result == null) stringResource(R.string.card_output) else stringResource(R.string.card_output_code, result.exitCode),
                body = result?.stdout?.ifBlank { result.stderr }.orEmpty(),
            )
        }
    }
}

@Composable
private fun RawCard(title: String, body: String) {
    ManagerCard {
        Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
        Spacer(Modifier.height(8.dp))
        CodeBlock(body.ifBlank { "-" })
    }
}

@Composable
private fun EmptyState(title: String, body: String) {
    ManagerCard {
        SectionHeader(icon = Icons.Default.Extension, title = title, subtitle = body)
    }
}

@Composable
private fun ManagerCard(content: @Composable ColumnScope.() -> Unit) {
    ElevatedCard(
        modifier = Modifier.fillMaxWidth(),
        shape = RoundedCornerShape(8.dp),
        colors = CardDefaults.elevatedCardColors(containerColor = MaterialTheme.colorScheme.surfaceContainerLow),
        content = {
            Column(
                modifier = Modifier.padding(16.dp),
                verticalArrangement = Arrangement.spacedBy(2.dp),
                content = content,
            )
        },
    )
}

@Composable
private fun SectionHeader(icon: ImageVector, title: String, subtitle: String) {
    Row(verticalAlignment = Alignment.CenterVertically) {
        Box(
            modifier = Modifier
                .size(36.dp)
                .background(MaterialTheme.colorScheme.secondaryContainer, CircleShape),
            contentAlignment = Alignment.Center,
        ) {
            Icon(icon, contentDescription = null, tint = MaterialTheme.colorScheme.onSecondaryContainer)
        }
        Spacer(Modifier.width(12.dp))
        Column {
            Text(title, style = MaterialTheme.typography.titleMedium, fontWeight = FontWeight.SemiBold)
            Text(subtitle, style = MaterialTheme.typography.bodySmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        }
    }
}

@Composable
private fun InfoRow(label: String, value: String) {
    Row(
        Modifier
            .fillMaxWidth()
            .padding(vertical = 3.dp),
        horizontalArrangement = Arrangement.SpaceBetween,
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Text(label, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value.ifBlank { stringResource(R.string.value_unknown) }, fontWeight = FontWeight.Medium, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun CompactTextField(
    value: String,
    onValueChange: (String) -> Unit,
    label: String,
    modifier: Modifier = Modifier,
) {
    OutlinedTextField(
        value = value,
        onValueChange = onValueChange,
        modifier = modifier,
        singleLine = true,
        label = { Text(label, maxLines = 1, overflow = TextOverflow.Ellipsis) },
    )
}

@Composable
private fun KeyValueGrid(items: List<Pair<String, String>>) {
    Column(verticalArrangement = Arrangement.spacedBy(4.dp)) {
        items.chunked(2).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp), modifier = Modifier.fillMaxWidth()) {
                row.forEach { item ->
                    KeyValueTile(item.first, item.second, Modifier.weight(1f))
                }
                if (row.size == 1) {
                    Spacer(Modifier.weight(1f))
                }
            }
        }
    }
}

@Composable
private fun KeyValueTile(label: String, value: String, modifier: Modifier = Modifier) {
    Column(
        modifier = modifier
            .background(MaterialTheme.colorScheme.surfaceContainerHigh, RoundedCornerShape(8.dp))
            .padding(horizontal = 12.dp, vertical = 10.dp),
    ) {
        Text(label, style = MaterialTheme.typography.labelSmall, color = MaterialTheme.colorScheme.onSurfaceVariant)
        Text(value.ifBlank { stringResource(R.string.value_unknown) }, style = MaterialTheme.typography.bodyMedium, maxLines = 1, overflow = TextOverflow.Ellipsis)
    }
}

@Composable
private fun StatusPill(label: String, good: Boolean, modifier: Modifier = Modifier) {
    Row(
        modifier = modifier
            .background(
                if (good) MaterialTheme.colorScheme.primaryContainer else MaterialTheme.colorScheme.errorContainer,
                RoundedCornerShape(8.dp),
            )
            .padding(horizontal = 10.dp, vertical = 8.dp),
        verticalAlignment = Alignment.CenterVertically,
    ) {
        Icon(
            if (good) Icons.Default.CheckCircle else Icons.Default.Error,
            contentDescription = null,
            modifier = Modifier.size(16.dp),
            tint = if (good) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onErrorContainer,
        )
        Spacer(Modifier.width(6.dp))
        Text(
            label,
            style = MaterialTheme.typography.labelMedium,
            color = if (good) MaterialTheme.colorScheme.onPrimaryContainer else MaterialTheme.colorScheme.onErrorContainer,
            maxLines = 2,
        )
    }
}

@Composable
private fun StatusOrb(ready: Boolean) {
    Box(
        modifier = Modifier
            .size(44.dp)
            .background(if (ready) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error, CircleShape),
        contentAlignment = Alignment.Center,
    ) {
        Icon(
            if (ready) Icons.Default.CheckCircle else Icons.Default.Error,
            contentDescription = null,
            tint = Color.White,
        )
    }
}

@Composable
private fun ChipRow(items: List<String>) {
    Column(verticalArrangement = Arrangement.spacedBy(6.dp)) {
        items.chunked(3).forEach { row ->
            Row(horizontalArrangement = Arrangement.spacedBy(8.dp)) {
                row.forEach { item ->
                    AssistChip(onClick = {}, label = { Text(item) })
                }
            }
        }
    }
}

@Composable
private fun CodeBlock(text: String) {
    Text(
        modifier = Modifier
            .fillMaxWidth()
            .background(MaterialTheme.colorScheme.surfaceContainerHigh, RoundedCornerShape(8.dp))
            .padding(12.dp),
        text = text.ifBlank { "-" },
        fontFamily = FontFamily.Monospace,
        style = MaterialTheme.typography.bodySmall,
    )
}

@Composable
private fun ActionMessage(actionResult: NativeActionResult?, error: String?) {
    if (actionResult != null) {
        Text(
            text = if (actionResult.ok) {
                stringResource(R.string.message_native_action_applied)
            } else {
                stringResource(R.string.message_native_action_failed, actionResult.error.ifBlank { actionResult.errno.toString() })
            },
            color = if (actionResult.ok) MaterialTheme.colorScheme.primary else MaterialTheme.colorScheme.error,
        )
    }
    if (error != null) {
        Text(error, color = MaterialTheme.colorScheme.error)
    }
}

@Composable
private fun nativeStatusLabel(status: Int): String = when (status) {
    0 -> stringResource(R.string.native_status_available)
    1 -> stringResource(R.string.native_status_not_present)
    2 -> stringResource(R.string.native_status_kernel_too_old)
    3 -> stringResource(R.string.native_status_client_too_old)
    else -> stringResource(R.string.native_status_unknown, status)
}

@Composable
private fun nativeGetFdLabel(native: NativeKasumiSnapshot): String = when {
    native.available -> stringResource(R.string.native_get_fd_ok)
    native.lastErrno != 0 && native.lastError.isNotBlank() -> "${native.lastError} (${native.lastErrno})"
    native.uid > 0 || native.euid > 0 -> stringResource(R.string.native_get_fd_app_uid)
    else -> stringResource(R.string.native_get_fd_missing)
}

private infix fun Int.hasFeature(feature: Int): Boolean = (this and feature) != 0
