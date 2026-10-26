// SPDX-License-Identifier: MIT
import { LANGUAGE_OPTIONS } from './i18n.js';
import { e, t, icon, iconButton, button, row, section, info, badge, empty, notice, resourceState, modeLabel, search } from './ui.js';

export const TABS = [
  { id: 'status', label: 'nav.status', icon: 'activity' },
  { id: 'modules', label: 'nav.modules', icon: 'layers' },
  { id: 'kasumi', label: 'nav.kasumi', icon: 'cpu' },
  { id: 'settings', label: 'nav.settings', icon: 'settings-2' },
];
export const FIELDS = {
  kasumi_enabled: { title: 'config.enableKasumi', description: 'ui.kasumiEnableDesc', type: 'switch', icon: 'power' },
  enable_stealth: { title: 'config.enableStealth', type: 'switch', icon: 'shield' },
  enable_overlay_xattr_hide: { title: 'config.enableOverlayXattrHide', description: 'config.enableOverlayXattrHideDesc', type: 'switch', icon: 'layers' },
  enable_mount_hide: { title: 'config.enableMountHide', description: 'config.enableMountHideDesc', type: 'switch', icon: 'eye-off' },
  enable_maps_spoof: { title: 'config.enableMapsSpoof', description: 'config.enableMapsSpoofDesc', type: 'switch', icon: 'file-text' },
  enable_statfs_spoof: { title: 'config.enableStatfsSpoof', description: 'config.enableStatfsSpoofDesc', type: 'switch', icon: 'hard-drive' },
  enable_selinux_fix: { title: 'config.enableSelinuxFix', description: 'config.enableSelinuxFixDesc', type: 'switch', icon: 'shield-check' },
  enable_kernel_debug: { title: 'config.enableKernelDebug', type: 'switch', icon: 'terminal' },
  debug: { title: 'config.debug', type: 'switch', icon: 'logs' },
  verbose: { title: 'config.verbose', type: 'switch', icon: 'list' },
  moduledir: { title: 'config.moduleDir', type: 'path', icon: 'folder' },
  work_dir: { title: 'ui.workDir', type: 'path', icon: 'folder' },
  mirror_dir: { title: 'ui.mirrorDir', type: 'path', optional: true, icon: 'folder' },
  mirror_img: { title: 'ui.mirrorImage', type: 'path', icon: 'hard-drive' },
  mirror_img_size_mb: { title: 'ui.mirrorSize', type: 'number', icon: 'hard-drive' },
  mountsource: { title: 'config.mountSource', type: 'choice', icon: 'layers', choices: [['KSU', 'KSU'], ['APatch', 'APatch'], ['Magisk', 'Magisk']] },
  fs_type: { title: 'config.fsType', type: 'choice', icon: 'hard-drive', choices: [['auto', 'config.fsAuto'], ['tmpfs', 'tmpfs'], ['erofs', 'erofs'], ['ext4', 'ext4']] },
  mount_backend: { title: 'ui.mountBackend', type: 'mode', icon: 'git-branch' },
  overlay_writable: { title: 'ui.overlayWritable', type: 'switch', icon: 'pencil' },
  overlayfs_enabled: { title: 'ui.overlayEnabled', type: 'switch', icon: 'layers' },
  magic_mount_enabled: { title: 'ui.magicEnabled', type: 'switch', icon: 'layers' },
};
export const VIEW_LABELS = { module: 'ui.moduleInfo', logs: 'nav.logs', lkm: 'ui.lkmManagement', hide: 'ui.hiddenPaths', rules: 'ui.effectiveRules', diagnostics: 'ui.diagnostics', storage: 'ui.storage', partitions: 'config.partitions', about: 'info.title', conflicts: 'ui.conflicts' };
export const VIEW_PARENTS = { module: 'modules', conflicts: 'modules', logs: 'status', lkm: 'kasumi', hide: 'kasumi', rules: 'kasumi', diagnostics: 'kasumi', storage: 'settings', partitions: 'settings', about: 'settings' };
const list = content => `<div class="row-list">${content}</div>`;
const data = (s, name, fallback = {}) => s.resources[name].data ?? fallback;
const error = (s, name) => resourceState(s.resources[name], name);
export const configValue = (s, key) => s.pendingConfig[key] ?? data(s, 'config')[key];
export const configReady = s => s.resources.config.ready && !s.resources.config.error;
export function mutation(s, key) {
  const record = s.mutations[key];
  if (!record) return '';
  const failed = ['error', 'applyFailed', 'unconfirmed', 'unknown'].includes(record.phase);
  return `<div class="save-status ${failed ? 'error' : ''}" data-key="save-${e(key)}" role="status">${icon(record.phase === 'saving' ? 'loader-circle' : failed ? 'circle-alert' : 'check', record.phase === 'saving' ? 'spinning' : '')}<span>${e(t(record.label))}${record.error ? `<small>${e(record.error)}</small>` : ''}</span>${failed ? button('retry-mutation', t('ui.retry'), { 'data-key': key }, 'text') : ''}</div>`;
}
export function setting(s, key) {
  const field = FIELDS[key];
  const saving = s.mutations[`config:${key}`]?.phase === 'saving';
  const disabled = !configReady(s) || saving;
  const value = configValue(s, key);
  const summary = field.type === 'mode' ? modeLabel(value) : field.choices ? t(field.choices.find(([choice]) => choice === value)?.[1] ?? String(value ?? 'ui.unknown')) : String(value ?? '');
  const description = field.description ? t(field.description) : '';
  return `<div class="setting-item" data-key="field-${e(key)}">${field.type === 'switch'
    ? `<label class="setting-row ${disabled ? 'disabled' : ''}"><span class="row-symbol">${icon(field.icon)}</span><span class="row-copy"><strong id="label-${e(key)}">${e(t(field.title))}</strong>${description ? `<small>${e(description)}</small>` : ''}</span><md-switch data-field="${e(key)}" aria-labelledby="label-${e(key)}" ${value ? 'selected' : ''} ${disabled ? 'disabled' : ''}></md-switch></label>`
    : row({ title: t(field.title), subtitle: summary || t('ui.default'), symbol: field.icon, action: 'config-edit', data: { 'data-field': key }, disabled })}${mutation(s, `config:${key}`)}</div>`;
}
function settings(s, keys) { return `<div class="setting-list">${keys.map(key => setting(s, key)).join('')}</div>`; }
function viewRow(view, title, subtitle, symbol) { return row({ title: title || t(VIEW_LABELS[view]), subtitle, symbol, action: 'open-view', data: { 'data-view': view }, key: `link-${view}` }); }
const isMounted = module => !module.runtime?.error && ['mounted', 'partial'].includes(module.runtime?.state);
function runtimeBadge(module, readFailed = false) {
  const runtime = module.runtime;
  const failed = readFailed || Boolean(runtime?.error);
  const state = failed ? 'unknown' : runtime?.state || 'unknown';
  const label = failed ? t('ui.mountReadFailed') : state === 'unknown' ? t('ui.mountUnavailable')
    : `${runtime.backend && runtime.backend !== 'none' ? `${modeLabel(runtime.backend)} ` : ''}${t(state === 'mounted' ? 'ui.mounted' : state === 'partial' ? 'ui.partialMount' : 'ui.unmounted')}`;
  return `<span data-key="runtime-status" data-runtime-state="${e(state)}">${badge(label, failed ? 'danger' : state === 'mounted' ? 'success' : state === 'partial' ? 'warning' : 'muted')}</span>`;
}
function status(s) {
  const system = data(s, 'system'), storage = data(s, 'storage'), lkm = data(s, 'lkm');
  const ready = s.resources.system.ready && !s.resources.system.error;
  const tone = s.resources.system.error || system.kasumiMismatch ? 'danger' : '';
  const title = s.resources.system.error ? t('ui.offline') : !ready ? t('ui.loading') : system.kasumiMismatch ? t('status.kasumiMismatch') : t('ui.ready');
  const count = name => s.resources[name].ready && !s.resources[name].error ? data(s, name, []).length : '—';
  const mounted = s.resources.modules.ready && !s.resources.modules.error ? data(s, 'modules', []).filter(isMounted).length : '—';
  return `${s.preview ? notice(t('ui.preview'), t('ui.previewNote')) : ''}
    <div class="runtime-overview ${tone}" data-key="overview"><span class="runtime-symbol">${icon(ready ? 'shield-check' : 'circle-alert')}</span><div><h2>${e(title)}</h2><p>${e(ready ? t('ui.connectedDesc') : '')}</p>${s.updated ? `<small>${e(s.updated)}</small>` : ''}</div></div>
    ${error(s, 'system')}${error(s, 'modules')}
    <div class="metrics" data-key="metrics"><div class="metric" data-key="mounted-count"><strong>${mounted}</strong><span>${e(t('ui.moduleCount'))}</span></div><div class="metric" data-key="module-count"><strong>${count('modules')}</strong><span>${e(t('ui.moduleTotal'))}</span></div></div>
    ${section(t('ui.mountDetails'), info([[t('status.totalMounts'), ready ? system.mountStats?.total_mounts : null], ['OverlayFS', ready ? system.mountStats?.overlayfs_mounts : null], ['Kasumi', ready ? t(system.kasumiAvailable ? 'ui.available' : 'ui.unavailable') : null], [t('ui.lkmManagement'), s.resources.lkm.error ? t('ui.unknown') : lkm.builtin ? t('ui.builtin') : lkm.loaded === true ? t('kasumi.lkm.loaded') : lkm.loaded === false ? t('kasumi.lkm.notLoaded') : null]]), { key: 'mounts' })}
    ${error(s, 'lkm')}${section(t('status.storage'), `${error(s, 'storage')}<div class="storage-panel"><div><strong>${storage.percent == null ? '—' : `${Number(storage.percent).toFixed(0)}%`}</strong><span>${e(storage.used || '—')} / ${e(storage.size || '—')} · ${e(storage.mode || t('ui.unknown'))}</span></div><progress max="100" value="${Math.max(0, Math.min(100, storage.percent ?? 0))}" aria-label="${e(t('status.storage'))}"></progress></div>`, { key: 'storage' })}
    ${section(t('ui.diagnostics'), list(viewRow('logs', '', t('logs.systemLog'), 'logs')), { key: 'diagnostics' })}
    <details class="section disclosure" data-key="system-details"><summary>${e(t('ui.statusDetails'))}${icon('chevron-down')}</summary>${info([[t('status.kernel'), system.kernel, true], [t('status.selinux'), system.selinux], [t('status.mountBase'), system.mountBase, true]])}</details>
    <details class="section disclosure" data-key="partition-details"><summary>${e(t('status.partitions'))}${icon('chevron-down')}</summary>${info((system.detectedPartitions || []).map(part => [part.name, `${part.mount_point} · ${part.fs_type}`, true]))}</details>`;
}
export function moduleResults(s) {
  if (s.moduleFilter === 'active' && s.resources.modules.error) return '';
  const needle = s.moduleSearch.toLocaleLowerCase();
  const modules = data(s, 'modules', []).filter(module => `${module.name} ${module.id} ${module.description}`.toLocaleLowerCase().includes(needle))
    .filter(module => s.moduleFilter === 'all' || (s.moduleFilter === 'active' ? isMounted(module) : module.mode === s.moduleFilter));
  if (!s.resources.modules.ready) return '';
  if (!modules.length) return empty(t(s.moduleSearch || s.moduleFilter !== 'all' ? 'ui.noResults' : 'modules.noModules'));
  return list(modules.map(module => `<button type="button" class="list-row module-row" data-action="module-detail" data-id="${e(module.id)}" data-key="${e(module.id)}"><span class="row-symbol">${icon('layers')}</span><span class="row-copy"><strong title="${e(module.name)}">${e(module.name)}</strong><small>${e(module.id)}${module.version ? ` · ${e(module.version)}` : ''}</small><span class="module-meta">${runtimeBadge(module, Boolean(s.resources.modules.error))}</span></span>${icon('chevron-right', 'chevron')}<md-ripple></md-ripple></button>`).join(''));
}
function modules(s) {
  return `<div class="toolbar" data-key="module-toolbar">${search('module-search', s.moduleSearch, t('modules.search'))}${iconButton('module-filter', 'filter', t('ui.filter'))}</div>
    <div class="list-heading" data-key="module-heading"><span>${e(s.moduleFilter === 'all' ? t('ui.all') : s.moduleFilter === 'active' ? t('ui.mounted') : modeLabel(s.moduleFilter))}</span>${button('open-view', t('modules.checkConflicts'), { 'data-view': 'conflicts' }, 'text')}</div>
    ${error(s, 'modules')}<div id="module-results" data-key="module-results">${moduleResults(s)}</div>`;
}
function bypassSwitch(s, checked, field, attributes, disabled, compact = false) {
  const system = s.resources.system;
  const supported = system.ready && !system.error && system.data.features?.names?.includes('rule_bypass');
  return `<label class="setting-row"><span class="row-copy"><strong>${e(t('bypass.title'))}</strong>${compact ? '' : `<small>${e(t(supported ? 'bypass.description' : 'bypass.unsupported'))}</small>`}</span><md-switch data-field="${e(field)}" ${attributes} aria-label="${e(t('bypass.title'))}" ${checked ? 'selected' : ''} ${disabled || (!supported && !checked) ? 'disabled' : ''}></md-switch></label>`;
}
function moduleDetail(s) {
  const module = data(s, 'modules', []).find(item => item.id === s.route.id);
  if (!module) return `${error(s, 'modules')}${s.resources.modules.ready ? empty(t('ui.moduleMissing')) : ''}`;
  const disabled = Boolean(s.resources.modules.error) || Object.entries(s.mutations).some(([key, value]) => key.startsWith(`module:${module.id}:`) && value.phase === 'saving');
  const runtime = module.runtime;
  const backend = s.resources.modules.error || runtime?.error ? t('ui.mountReadFailed') : runtime?.backend === 'none' ? t('ui.none') : modeLabel(runtime?.backend);
  return `${error(s, 'modules')}<div class="module-intro" data-key="module-intro"><span class="large-symbol">${icon('layers')}</span><h2>${e(module.name)}</h2><p>${e(module.description)}</p><div class="badges">${badge(module.version || '—')}${runtimeBadge(module, Boolean(s.resources.modules.error))}</div></div>
    ${section(t('ui.mounting'), `${list(row({ title: t('ui.configMode'), subtitle: modeLabel(module.mode), symbol: 'git-branch', action: 'module-mode', data: { 'data-id': module.id }, disabled }))}${mutation(s, `module:${module.id}:mode`)}${bypassSwitch(s, module.bypass, 'module_bypass', `data-id="${e(module.id)}"`, disabled)}${mutation(s, `module:${module.id}:bypass`)}<p class="supporting">${e(t('ui.mountBackendNextBoot'))}</p>${info([[t('ui.runtimeMode'), backend], ...(runtime?.error ? [[t('ui.lastError'), runtime.error.message]] : [])])}`, { key: 'module-mode' })}
    ${section(t('modules.rules'), `${module.rules.length ? `<div class="row-list">${module.rules.map(rule => `<div class="rule-row" data-key="${e(rule.path)}"><button class="rule-content" data-action="rule-edit" data-id="${e(module.id)}" data-path="${e(rule.path)}" ${disabled ? 'disabled' : ''}><code dir="ltr">${e(rule.path)}</code><span>${e(modeLabel(rule.mode))}${module.bypass || rule.bypass ? ` · ${e(t('bypass.title'))}` : ''}</span></button>${iconButton('rule-remove', 'trash-2', t('ui.remove'), { 'data-id': module.id, 'data-path': rule.path, disabled })}</div>`).join('')}</div>` : empty(t('ui.empty'), 'list')}${Object.keys(s.mutations).filter(key => key.startsWith(`module:${module.id}:rule`)).map(key => mutation(s, key)).join('')}`, { key: 'module-rules', action: button('rule-add', t('modules.addRule'), { 'data-id': module.id, disabled }, 'tonal', 'plus') })}
    ${section(t('ui.moduleInfo'), info([[t('ui.id'), module.id, true], [t('ui.author'), module.author || '—'], [t('modules.path'), module.path, true]]), { key: 'module-info' })}`;
}
function kasumi(s) {
  const system = data(s, 'system'), lkm = data(s, 'lkm');
  const available = s.resources.system.ready && !s.resources.system.error && system.kasumiAvailable;
  return `${error(s, 'system')}${error(s, 'config')}
    ${system.kasumiMismatch ? notice(t('status.kasumiMismatch'), system.mismatchMessage, 'danger') : ''}
    <div class="runtime-overview ${system.kasumiMismatch ? 'danger' : ''}" data-key="kasumi-status"><span class="runtime-symbol">${icon('cpu')}</span><div><h2>Kasumi</h2><p>${e(s.resources.system.error || !s.resources.system.ready ? t('ui.unknown') : available ? t('ui.available') : t('ui.unavailable'))}${lkm.builtin ? ` · ${e(t('ui.builtin'))}` : ''}</p></div></div>
    ${section(t('ui.behavior'), settings(s, ['kasumi_enabled', 'enable_stealth']), { key: 'kasumi-behavior' })}
    ${section(t('ui.protection'), settings(s, ['enable_mount_hide', 'enable_overlay_xattr_hide', 'enable_maps_spoof', 'enable_statfs_spoof', 'enable_selinux_fix']), { key: 'kasumi-visibility' })}
    ${section(t('ui.features'), list(viewRow('lkm', '', lkm.builtin ? t('ui.builtin') : t('ui.external'), 'cpu') + viewRow('hide', '', t('hideRules.subtitle'), 'eye-off') + viewRow('rules', '', '', 'list') + viewRow('diagnostics', '', t('kasumi.hooks.title'), 'terminal')), { key: 'kasumi-links' })}`;
}
function settingsPage(s) {
  return `${section(t('ui.preferences'), list(row({ title: t('ui.theme'), subtitle: t(`ui.theme.${s.theme}`), symbol: 'moon', action: 'preference', data: { 'data-key': 'theme' } }) + row({ title: t('ui.language'), subtitle: LANGUAGE_OPTIONS.find(option => option.value === s.language)?.label, symbol: 'languages', action: 'preference', data: { 'data-key': 'language' } })), { key: 'preferences' })}
    ${error(s, 'config')}${section(t('ui.mounting'), `${settings(s, ['mount_backend', 'overlayfs_enabled', 'magic_mount_enabled'])}${list(viewRow('storage', '', t('config.fsType'), 'hard-drive') + viewRow('partitions', '', `${data(s, 'config').partitions?.length ?? '—'}`, 'folder'))}<p class="supporting">${e(t('ui.nextBoot'))}</p>`, { key: 'mount-settings' })}
    ${section(t('ui.diagnostics'), settings(s, ['debug', 'verbose', 'enable_kernel_debug']), { key: 'logging-settings' })}
    ${section(t('info.title'), list(viewRow('about', 'Kagami', data(s, 'meta').version || '—', 'info')), { key: 'about-link' })}`;
}
function lkmPage(s) {
  const lkm = data(s, 'lkm');
  const ready = s.resources.lkm.ready && !s.resources.lkm.error;
  const busy = Object.entries(s.mutations).some(([key, value]) => key.startsWith('lkm:') && value.phase === 'saving');
  const controls = ready && !lkm.builtin && lkm.external_management !== false;
  return `${error(s, 'lkm')}${lkm.builtin ? notice(t('ui.builtin'), t('ui.builtinDesc')) : ''}
    ${info([[t('ui.lkmManagement'), !ready ? t('ui.unknown') : lkm.builtin ? t('ui.builtin') : t(lkm.loaded ? 'kasumi.lkm.loaded' : 'kasumi.lkm.notLoaded')], [t('ui.detectedKmi'), lkm.detected_kmi || '—', true], [t('ui.asset'), lkm.asset || '—', true]])}
    ${controls ? `<div class="actions" data-key="lkm-actions">${button(lkm.loaded ? 'lkm-unload' : 'lkm-load', t(lkm.loaded ? 'kasumi.lkm.unload' : 'kasumi.lkm.load'), { disabled: busy }, 'filled', 'power')}</div>${section(t('ui.behavior'), `<div class="setting-list"><label class="setting-row"><span class="row-symbol">${icon('rotate-ccw')}</span><span class="row-copy"><strong id="label-autoload">${e(t('kasumi.lkm.autoload'))}</strong><small>${e(t('ui.autoloadDesc'))}</small></span><md-switch data-field="lkm_autoload" aria-labelledby="label-autoload" ${lkm.autoload ? 'selected' : ''} ${busy ? 'disabled' : ''}></md-switch></label>${row({ title: t('kasumi.lkm.kmiOverride'), subtitle: lkm.kmi_override || t('ui.default'), symbol: 'cpu', action: 'kmi-edit', disabled: busy })}</div>`, { key: 'lkm-settings' })}` : ''}
    ${['lkm:load', 'lkm:unload', 'lkm:autoload', 'lkm:kmi'].map(key => mutation(s, key)).join('')}
    ${lkm.last_error ? notice(t('ui.lastError'), lkm.last_error, 'danger') : ''}
    ${lkm.unload?.attempted ? section(t('kasumi.lkm.unload'), info(Object.entries(lkm.unload).filter(([, value]) => typeof value !== 'object').map(([key, value]) => [key, String(value)])), { key: 'unload-details' }) : ''}`;
}
function hidePage(s) {
  const saved = data(s, 'userRules', []).map(rule => typeof rule === 'string' ? { path: rule, bypass: false } : rule), active = data(s, 'rules', []);
  const disabled = !s.resources.userRules.ready || Boolean(s.resources.userRules.error);
  const busy = Object.entries(s.mutations).some(([key, value]) => key.startsWith('hide:') && value.phase === 'saving');
  return `${error(s, 'userRules')}${error(s, 'rules')}${section(t('hideRules.userRules'), saved.length ? list(saved.map(({ path, bypass }) => `<div class="rule-row" data-key="${e(path)}"><details class="disclosure hide-options"><summary><span class="hide-summary"><code dir="ltr">${e(path)}</code><span class="badges">${bypass ? badge(t('bypass.global')) : ''}${badge(t(!s.resources.rules.error && s.resources.rules.ready && active.some(rule => rule.path === path && rule.type === 'HIDE') ? 'ui.hideActive' : 'ui.hidePending'))}</span></span>${icon('chevron-down')}</summary>${bypassSwitch(s, bypass, 'hide_bypass', `data-path="${e(path)}"`, disabled || busy, true)}</details>${iconButton('hide-remove', 'trash-2', t('ui.remove'), { 'data-path': path, disabled: disabled || busy })}</div>`).join('')) : s.resources.userRules.ready ? empty(t('hideRules.noUserRules'), 'eye-off') : '', { key: 'hide-paths', action: button('hide-add', t('ui.add'), { disabled: disabled || busy }, 'tonal', 'plus') })}
    ${Object.keys(s.mutations).filter(key => key.startsWith('hide:')).map(key => mutation(s, key)).join('')}
    ${section(t('ui.quickPaths'), `<div class="chips">${['/dev/scene', '/dev/cpuset/scene-daemon', '/sdcard/Download/advanced', '/sdcard/MT2', '/data/adb/magisk'].map(path => `<button class="chip" data-action="hide-add" data-path="${e(path)}" ${disabled || busy || saved.some(rule => rule.path === path) ? 'disabled' : ''}><code dir="ltr">${e(path)}</code></button>`).join('')}</div>`, { key: 'quick-paths' })}`;
}
function rulesPage(s) {
  const rules = data(s, 'rules', []);
  return `${error(s, 'rules')}${rules.length ? `<div class="rule-details">${rules.map((rule, index) => `<details class="disclosure" data-key="rule-${index}-${e(rule.path)}"><summary><span><code dir="ltr">${e(rule.path)}</code><small>${e(rule.type)} · ${e(t(rule.isUserDefined ? 'ui.userRule' : 'ui.moduleRule'))}</small></span>${icon('chevron-down')}</summary>${info([[t('ui.type'), rule.type], [t('ui.target'), rule.target || '—', true], [t('ui.source'), rule.source || '—', true]])}</details>`).join('')}</div>` : s.resources.rules.ready ? empty(t('ui.noActiveRules'), 'list') : ''}`;
}
function diagnostics(s) {
  const system = data(s, 'system');
  return `${error(s, 'system')}${info([[t('ui.logPath'), data(s, 'meta').log_file || data(s, 'config').logfile || '—', true]])}${section(t('ui.features'), `<div class="chips">${(system.features?.names || []).map(name => badge(name)).join('') || e(t('ui.empty'))}</div>`, { key: 'feature-list' })}${section(t('kasumi.hooks.title'), `<pre class="terminal" dir="ltr">${e(system.hooks || t('ui.noHooks'))}</pre>`, { key: 'hooks' })}`;
}
function storagePage(s) {
  return `${error(s, 'config')}${notice(t('ui.nextBoot'))}${section(t('config.fsType'), settings(s, ['fs_type', 'overlay_writable']), { key: 'filesystems' })}${section(t('ui.storage'), settings(s, ['moduledir', 'work_dir', 'mirror_dir', 'mirror_img', 'mirror_img_size_mb', 'mountsource']), { key: 'paths' })}`;
}
function partitionsPage(s) {
  const partitions = data(s, 'config').partitions || [];
  const disabled = !configReady(s) || s.mutations['config:partitions']?.phase === 'saving';
  return `${error(s, 'config')}${notice(t('ui.nextBoot'))}<div class="actions">${button('partition-add', t('ui.add'), { disabled }, 'filled', 'plus')}${button('partition-scan', t('config.scanPartitions'), { disabled }, 'tonal', 'search')}</div>
    ${partitions.length ? list(partitions.map(name => `<div class="rule-row" data-key="${e(name)}"><code class="rule-content" dir="ltr">${e(name)}</code>${iconButton('partition-remove', 'trash-2', t('ui.remove'), { 'data-name': name, disabled })}</div>`).join('')) : empty()}
    ${mutation(s, 'config:partitions')}`;
}
export function logResults(s) {
  if (!s.resources.logs.ready) return '';
  const text = data(s, 'logs', '');
  const lines = text.split('\n').filter(line => line.toLocaleLowerCase().includes(s.logSearch.toLocaleLowerCase()));
  return lines.some(Boolean) ? `<pre class="terminal" dir="ltr">${e(lines.join('\n'))}</pre>` : empty(t(s.logSearch ? 'ui.noResults' : 'logs.noLogs'), 'logs');
}
function logsPage(s) {
  return `<div class="segmented" data-key="log-type" role="group" aria-label="${e(t('nav.logs'))}">${['system', 'kernel'].map(type => `<button data-action="log-type" data-type="${type}" aria-pressed="${type === s.logType}">${e(t(`logs.${type}Log`))}</button>`).join('')}</div>
    <div class="toolbar" data-key="log-toolbar">${search('log-search', s.logSearch, t('ui.searchLogs'))}${iconButton('copy-logs', 'copy', t('ui.copy'), { disabled: !s.resources.logs.ready || Boolean(s.resources.logs.error) })}${s.logType === 'system' ? iconButton('clear-logs', 'trash-2', t('logs.clear'), { disabled: s.mutations['logs:clear']?.phase === 'saving' }) : ''}</div>
    ${error(s, 'logs')}${mutation(s, 'logs:clear')}<div id="log-results" data-key="log-results">${logResults(s)}</div>`;
}
function conflictsPage(s) {
  const conflicts = data(s, 'conflicts', []);
  return `${error(s, 'conflicts')}${conflicts.length ? list(conflicts.map(item => row({ title: item.file, subtitle: item.modules.join(' · '), symbol: 'circle-alert', key: item.file })).join('')) : s.resources.conflicts.ready ? empty(t('modules.noConflicts'), 'shield-check') : ''}`;
}
function aboutPage(s) {
  const meta = data(s, 'meta');
  const links = [['Kagami', 'https://github.com/Rouyashiki/Kagami'], ['Kasumi', 'https://github.com/Rouyashiki/Kasumi'], ['YukiSU', 'https://github.com/Rouyashiki/YukiSU'], ['GitLab', 'https://gitlab.com/Anatdx/kagami'], ['Anatdx', 'https://git.anatdx.com/Anatdx/kagami']];
  return `<div class="module-intro" data-key="brand"><span class="brand-mark" aria-hidden="true"></span><h2>Kagami</h2><p>${e(t('ui.aboutDescription'))}</p></div>${error(s, 'meta')}${info([[t('ui.version'), meta.version], [t('ui.author'), meta.author || 'Anatdx']])}
    ${section(t('info.links'), list(links.map(([label, url]) => `<a class="list-row" href="${url}" target="_blank" rel="noopener noreferrer"><span class="row-symbol">${icon('link')}</span><span class="row-copy"><strong>${e(label)}</strong></span>${icon('external-link')}</a>`).join('')), { key: 'project-links' })}
    ${section(t('info.acknowledgments'), `<p class="supporting">KernelSU · WebUI X · Kasumi · YukiSU · Magisk · KernelPatch · susfs4ksu · meta-hybrid_mount · meta-magic_mount</p><p class="supporting">Material Web · Lucide · KOWX712 · Anatdx</p>`, { key: 'credits' })}`;
}
export function renderPage(s) {
  const pages = { status, modules, kasumi, settings: settingsPage, module: moduleDetail, lkm: lkmPage, hide: hidePage, rules: rulesPage, diagnostics, storage: storagePage, partitions: partitionsPage, logs: logsPage, conflicts: conflictsPage, about: aboutPage };
  return pages[s.route.view || s.route.tab](s);
}
export function pageTitle(s) {
  if (s.route.view === 'module') return data(s, 'modules', []).find(module => module.id === s.route.id)?.name || t('ui.moduleInfo');
  return t(s.route.view ? VIEW_LABELS[s.route.view] : TABS.find(tab => tab.id === s.route.tab).label);
}
export function pageResources(route) {
  const byView = { module: ['modules'], lkm: ['lkm'], hide: ['userRules', 'rules'], rules: ['rules'], diagnostics: ['system'], storage: ['config', 'storage'], partitions: ['config', 'system'], logs: ['logs'], conflicts: ['conflicts'], about: ['meta'] };
  return byView[route.view] || ({ status: ['system', 'modules', 'storage', 'lkm'], modules: ['modules'], kasumi: ['config', 'system', 'lkm'], settings: ['config', 'meta'] })[route.tab];
}
