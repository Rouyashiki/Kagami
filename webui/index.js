// SPDX-License-Identifier: MIT
// Kagami interface, adapted from YukiZygisk and KOWX712/ksu-webui-demo.
// Authors: KOWX712 and Anatdx.
import './assets/material.js';
import { api, BUILTIN_PARTITIONS, getRuntimeMode } from './api.js';
import { enableEdgeToEdge, hasAsyncExec } from './assets/kernelsu.js';
import { LANGUAGE_OPTIONS, getNavigatorLanguage } from './i18n.js';
import { createStore } from './state.js';
import { createNavigation } from './navigation.js';
import { TABS, FIELDS, configReady, configValue, renderPage, pageTitle, pageResources, moduleResults, logResults } from './pages.js';
import { e, t, setLanguage, icon, iconButton, button, patchHtml, radioOptions, textField, modeLabel } from './ui.js';

function preference(key, fallback) {
  try { return localStorage.getItem(key) || fallback; } catch { return fallback; }
}
function storePreference(key, value) {
  try { localStorage.setItem(key, value); } catch { /* Host storage may be disabled. */ }
}
const store = createStore(api, () => render());
const state = store.state;
state.language = preference('kagami_static_language', getNavigatorLanguage());
if (!LANGUAGE_OPTIONS.some(option => option.value === state.language)) state.language = 'en';
state.theme = preference('kagami_static_theme', 'system');
if (!['system', 'light', 'dark'].includes(state.theme)) state.theme = 'system';
state.preview = getRuntimeMode() === 'mock';
state.moduleSearch = '';
state.moduleFilter = 'all';
state.logSearch = '';
state.route = { tab: 'status', view: '', id: '' };
const dialog = document.getElementById('editor');
let dialogModel = null;
let mounted = false;
let toastTimer;
let pollTimer;
let refreshPromise;
let activeMutation = null;
const reducedMotion = matchMedia('(prefers-reduced-motion: reduce)');

function applyPreferences() {
  setLanguage(state.language);
  if (state.theme === 'system') delete document.documentElement.dataset.theme;
  else document.documentElement.dataset.theme = state.theme;
}
function navButtons() {
  return TABS.map(tab => `<button type="button" class="nav-item" data-tab="${tab.id}" ${state.route.tab === tab.id ? 'aria-current="page"' : ''}><span class="nav-icon">${icon(tab.icon)}</span><span>${e(t(tab.label))}</span></button>`).join('');
}
function mountApp() {
  document.getElementById('app').className = '';
  document.getElementById('app').innerHTML = `<nav class="side-nav" aria-label="Kagami"><div class="side-brand"><span class="brand-mark" aria-hidden="true"></span>Kagami</div><div id="side-links"></div></nav><div class="workspace"><div class="page"><header class="page-heading" id="page-heading"></header><main id="page-content" tabindex="-1"></main></div></div><nav class="bottom-nav" id="bottom-links" aria-label="Kagami"></nav>`;
  mounted = true;
  render();
}
function render() {
  if (!mounted) return;
  const title = pageTitle(state);
  document.title = `${title} · Kagami`;
  for (const id of ['side-links', 'bottom-links']) patchHtml(document.getElementById(id), navButtons());
  const loading = pageResources(state.route).some(name => state.resources[name].loading);
  patchHtml(document.getElementById('page-heading'), `${state.route.view ? iconButton('back', 'arrow-left', t('ui.back')) : ''}<h1>${e(title)}</h1>${state.preview ? `<span class="preview-badge">${e(t('ui.preview'))}</span>` : ''}${iconButton('refresh', 'refresh-cw', t('ui.refresh'), { disabled: loading, 'aria-busy': loading })}`);
  patchHtml(document.getElementById('page-content'), renderPage(state));
}
function animatePage() {
  if (reducedMotion.matches) return;
  document.getElementById('page-content').animate([{ opacity: 0, transform: 'translateY(10px)' }, { opacity: 1, transform: 'none' }], { duration: 280, easing: 'cubic-bezier(.2,0,0,1)' });
}
const navigation = createNavigation({
  onRoute(route, changed) {
    state.route = route;
    if (!mounted) return;
    if (changed) document.getElementById('page-content').replaceChildren();
    render();
    if (changed) {
      animatePage();
      document.getElementById('page-content').focus({ preventScroll: true });
      void refreshCurrent();
    }
    schedulePoll();
  },
  onDialogBack() {
    if (dialog.open && (dialogModel?.requiredAck || dialogModel?.busy)) return false;
    if (dialog.open) dialog.close();
    dialogModel = null;
    render();
  },
});
state.route = navigation.route;

function schedulePoll() {
  clearTimeout(pollTimer);
  if ((!state.preview && !hasAsyncExec()) || document.hidden || state.route.view || !['status', 'modules'].includes(state.route.tab)) return;
  pollTimer = setTimeout(async () => {
    if (!dialog.open && !document.activeElement?.matches('input,textarea,select')) await refreshCurrent();
    schedulePoll();
  }, 15000);
}
async function refreshCurrent() {
  const names = pageResources(state.route);
  // Route-specific queued refreshes retain the destination that requested them.
  if (refreshPromise && names.every(name => state.resources[name].loading)) return refreshPromise;
  const promise = store.refresh(names);
  refreshPromise = promise;
  try { return await promise; } finally { if (refreshPromise === promise) refreshPromise = null; }
}
function showToast(message) {
  clearTimeout(toastTimer);
  const stack = document.getElementById('toast-stack');
  stack.innerHTML = `<div class="toast"><span>${e(message)}</span><button class="icon-button" data-action="dismiss-toast" aria-label="${e(t('ui.close'))}">${icon('x')}</button></div>`;
  toastTimer = setTimeout(() => stack.replaceChildren(), 5000);
}
function showDialog({ title, body, submit, confirm = t('common.save'), requiredAck = false }) {
  if (dialog.open) return;
  dialogModel = { submit, requiredAck, busy: false, resume: null };
  document.getElementById('dialog-title').textContent = title;
  document.getElementById('dialog-body').innerHTML = body;
  document.getElementById('dialog-error').hidden = true;
  const close = dialog.querySelector('[data-action="close-dialog"]');
  close.innerHTML = icon('x');
  close.setAttribute('aria-label', t('ui.close'));
  close.hidden = requiredAck;
  document.getElementById('dialog-actions').innerHTML = `${requiredAck ? '' : button('dialog-cancel', t('common.cancel'), { type: 'button' }, 'text')}${button('dialog-save', confirm, { type: 'submit' }, 'filled')}`;
  navigation.openDialog();
  dialog.showModal();
  requestAnimationFrame(() => (dialog.querySelector('input:not([type=radio])') || dialog.querySelector('input:checked') || dialog.querySelector('[data-action="dialog-save"]'))?.focus());
}
function closeDialog() {
  if (!dialogModel || dialogModel.requiredAck || dialogModel.busy) return;
  navigation.back();
}
function setDialogBusy(busy) {
  if (!dialogModel) return;
  dialogModel.busy = busy;
  for (const control of dialog.querySelectorAll('input,button,md-filled-button,md-text-button')) {
    if (busy) {
      control.dataset.wasDisabled = String(Boolean(control.disabled));
      control.disabled = true;
    } else {
      control.disabled = control.dataset.wasDisabled === 'true';
      delete control.dataset.wasDisabled;
    }
  }
  dialog.setAttribute('aria-busy', String(busy));
}
function dialogError(message) {
  const element = document.getElementById('dialog-error');
  element.textContent = message;
  element.hidden = false;
}
async function runMutation(key, operation, options) {
  activeMutation = key;
  return store.mutate(key, operation, options);
}
async function saveConfig(key, value) {
  if (!configReady(state)) throw new Error(t('ui.saveBlocked'));
  const nextBoot = ['moduledir', 'work_dir', 'mirror_dir', 'mirror_img', 'mirror_img_size_mb', 'mountsource', 'fs_type', 'mount_backend', 'overlay_writable', 'overlayfs_enabled', 'magic_mount_enabled', 'partitions', 'enable_overlay_xattr_hide'].includes(key);
  return runMutation(`config:${key}`, () => api.saveConfig({ [key]: value }), { reload: ['config', 'system'], success: nextBoot ? 'ui.nextBoot' : result => result?.runtimeApplied ? 'ui.runtimeApplied' : 'ui.saved', pendingConfig: { [key]: value } });
}
function requirePath(value, optional = false) {
  const path = String(value || '').trim();
  if ((!path && optional)) return path;
  if (!path.startsWith('/') || /[\0\r\n]/.test(path)) throw new Error(t('ui.absolutePath'));
  return path;
}
function modes({ includeAuto = true, includeHide = false } = {}) {
  return [...(includeAuto ? ['auto'] : []), 'kasumi', 'overlay', 'magic', 'none', ...(includeHide ? ['hide'] : [])].map(value => ({ value, label: modeLabel(value) }));
}
function editPreference(key) {
  const options = key === 'theme' ? ['system', 'light', 'dark'].map(value => ({ value, label: t(`ui.theme.${value}`) })) : LANGUAGE_OPTIONS;
  showDialog({ title: t(key === 'theme' ? 'ui.theme' : 'ui.language'), body: radioOptions(options, state[key]), submit(form) {
    const value = form.get('value');
    if (!options.some(option => option.value === value)) throw new Error(t('ui.required'));
    state[key] = value;
    storePreference(`kagami_static_${key}`, value);
    applyPreferences(); render(); return true;
  } });
}
function editConfig(key) {
  const field = FIELDS[key];
  if (!field || !configReady(state)) return;
  const value = configValue(state, key);
  let options;
  if (field.type === 'mode') options = modes().filter(option => option.value !== 'none');
  if (field.choices) options = field.choices.map(([value, label]) => ({ value, label: t(label) }));
  if (key === 'mount_hide_mode') {
    const resource = state.resources.system;
    const supported = resource.ready && !resource.error && resource.data.features?.names?.includes('mount_hide_aggressive');
    options = options.map(option => ({ ...option, disabled: option.value === 'aggressive' && !supported, description: option.value === 'aggressive' && !supported ? t('ui.unsupported') : t(option.value === 'aggressive' ? 'config.mountHideModeAggressiveDesc' : 'config.mountHideModeNormalDesc') }));
  }
  showDialog({ title: t(field.title), body: options ? radioOptions(options, value) : textField(t(field.title), value, { type: field.type === 'number' ? 'number' : 'text', required: !field.optional }), submit(form) {
    let next = String(form.get('value') ?? '').trim();
    if (options && !options.some(option => String(option.value) === next && !option.disabled)) throw new Error(t('ui.required'));
    if (field.type === 'path') next = requirePath(next, field.optional);
    if (field.type === 'number') {
      if (!/^\d+$/.test(next) || !Number.isSafeInteger(Number(next)) || Number(next) <= 0 || Number(next) > 2147483647) throw new Error(t('ui.imageSizeRange'));
      next = Number(next);
    }
    return saveConfig(key, next);
  } });
}
function getModule(id) {
  const resource = state.resources.modules;
  if (!resource.ready || resource.error) throw new Error(t('ui.readFailed'));
  const module = resource.data.find(item => item.id === id);
  if (!module) throw new Error(t('ui.moduleMissing'));
  return module;
}
function editModuleMode(id) {
  const module = getModule(id);
  showDialog({ title: t('ui.configMode'), body: radioOptions(modes(), module.mode) + `<p>${e(t('ui.nextBoot'))}</p>`, submit(form) {
    const mode = form.get('value');
    if (!modes().some(option => option.value === mode)) throw new Error(t('ui.required'));
    return runMutation(`module:${id}:mode`, () => api.setModuleMode(id, mode), { reload: ['modules'], success: 'ui.nextBoot' });
  } });
}
function editRule(id, originalPath = '') {
  const module = getModule(id);
  const existing = module.rules.find(rule => rule.path === originalPath);
  if (originalPath && !existing) throw new Error(t('ui.readFailed'));
  showDialog({ title: t('ui.ruleEditor'), body: textField(t('modules.path'), originalPath, { name: 'path', placeholder: '/system/…' }) + radioOptions(modes({ includeAuto: false, includeHide: true }), existing?.mode || 'kasumi', 'mode'), submit(form) {
    const path = requirePath(form.get('path'));
    const mode = String(form.get('mode') || '');
    if (!modes({ includeAuto: false, includeHide: true }).some(option => option.value === mode)) throw new Error(t('ui.required'));
    if (path !== originalPath && getModule(id).rules.some(rule => rule.path === path)) throw new Error(t('ui.duplicatePath'));
    let added = false;
    const operation = async () => {
      if (!added) { await api.addModuleRule(id, path, mode); added = true; }
      if (originalPath && path !== originalPath) {
        const current = (await api.scanModules()).find(item => item.id === id);
        if (!current?.rules.some(rule => rule.path === path && rule.mode === mode)) throw new Error(t('ui.ruleReplacementMissing'));
        try { await api.removeModuleRule(id, originalPath); }
        catch (error) { error.message = `${t('ui.ruleRenamePartial')} ${error.message}`; throw error; }
      }
    };
    return runMutation(`module:${id}:rule:${originalPath || path}`, operation, { reload: ['modules'], success: 'ui.nextBoot' });
  } });
}
function confirmAction(title, body, key, operation, options = {}) {
  showDialog({ title, body: `<p>${e(body)}</p>`, confirm: t('common.confirm'), submit: () => runMutation(key, operation, options) });
}
function addHideRule(initial = '') {
  if (!state.resources.userRules.ready || state.resources.userRules.error) return;
  showDialog({ title: t('ui.hiddenPaths'), body: textField(t('modules.path'), initial), submit(form) {
    const path = requirePath(form.get('value'));
    if (state.resources.userRules.data.includes(path)) throw new Error(t('ui.duplicatePath'));
    return runMutation(`hide:${path}`, () => api.addUserHideRule(path), { reload: ['userRules', 'rules'] });
  } });
}
function addPartition() {
  if (!configReady(state)) return;
  showDialog({ title: t('config.addPartition'), body: textField(t('config.addPartition'), '', { placeholder: 'vendor_dlkm' }), submit(form) {
    const name = String(form.get('value') || '').trim();
    if (!/^[A-Za-z0-9_][A-Za-z0-9_.-]*$/.test(name) || name === '.' || name === '..') throw new Error(t('ui.partitionName'));
    return saveConfig('partitions', [...new Set([...effectivePartitions(), name])]);
  } });
}
function effectivePartitions() {
  const configured = state.resources.config.data?.partitions;
  return configured?.length ? configured : BUILTIN_PARTITIONS;
}
function editKmi() {
  const lkm = state.resources.lkm;
  if (!lkm.ready || lkm.error || lkm.data.builtin) return;
  showDialog({ title: t('kasumi.lkm.kmiOverride'), body: textField(t('kasumi.lkm.kmiOverride'), lkm.data.kmi_override || '', { required: false, hint: t('ui.kmiHint'), placeholder: 'android14-6.1' }), submit(form) {
    const value = String(form.get('value') || '').trim();
    return runMutation('lkm:kmi', () => value ? api.lkmSetKmi(value) : api.lkmClearKmi(), { reload: ['lkm'] });
  } });
}
async function copyLogs() {
  if (!state.resources.logs.ready || state.resources.logs.error) return;
  const text = state.resources.logs.data;
  if (navigator.clipboard?.writeText) await navigator.clipboard.writeText(text);
  else {
    const field = document.createElement('textarea');
    field.value = text; field.style.position = 'fixed'; field.style.opacity = '0';
    document.body.appendChild(field); field.select();
    const copied = document.execCommand('copy'); field.remove();
    if (!copied) throw new Error(t('ui.copy'));
  }
  showToast(t('ui.copied'));
}

async function handleAction(target) {
  const action = target.dataset.action;
  if (action === 'dismiss-toast') { document.getElementById('toast-stack').replaceChildren(); return; }
  if (action === 'dialog-save') return; // The form submit event owns the transaction.
  if (action === 'dialog-cancel' || action === 'close-dialog') { closeDialog(); return; }
  if (action === 'back') { navigation.back(); return; }
  if (action === 'open-view') { navigation.open(target.dataset.view); return; }
  if (action === 'module-detail') { navigation.open('module', target.dataset.id); return; }
  if (action === 'refresh') { await refreshCurrent(); return; }
  if (action === 'retry-resource') { await store.refresh([target.dataset.resource]); return; }
  if (action === 'retry-mutation') { await state.mutations[target.dataset.key]?.retry(); return; }
  if (action === 'preference') { editPreference(target.dataset.key); return; }
  if (action === 'config-edit') { editConfig(target.dataset.field); return; }
  if (action === 'module-mode') { editModuleMode(target.dataset.id); return; }
  if (action === 'rule-add' || action === 'rule-edit') { editRule(target.dataset.id, target.dataset.path); return; }
  if (action === 'rule-remove') {
    const { id, path } = target.dataset;
    confirmAction(t('ui.removeRule'), path, `module:${id}:rule:${path}`, () => api.removeModuleRule(id, path), { reload: ['modules'], success: 'ui.nextBoot' }); return;
  }
  if (action === 'module-filter') {
    const options = [{ value: 'all', label: t('ui.all') }, ...modes(), { value: 'active', label: t('ui.mounted') }];
    showDialog({ title: t('ui.filter'), body: radioOptions(options, state.moduleFilter), submit(form) { state.moduleFilter = form.get('value'); render(); return true; } }); return;
  }
  if (action === 'log-type') {
    if (state.logType === target.dataset.type) return;
    state.logType = target.dataset.type;
    Object.assign(state.resources.logs, { ready: false, data: null, error: '' });
    render(); await store.refresh(['logs']); return;
  }
  if (action === 'copy-logs') { await copyLogs(); return; }
  if (action === 'clear-logs') {
    if (state.logType !== 'system') return;
    confirmAction(t('ui.clearLogs'), t('ui.clearLogsDesc'), 'logs:clear', () => api.clearLogs(), { reload: ['logs'] }); return;
  }
  if (action === 'hide-add') { addHideRule(target.dataset.path); return; }
  if (action === 'hide-remove') {
    const path = target.dataset.path;
    confirmAction(t('ui.removeRule'), path, `hide:${path}`, () => api.removeUserHideRule(path), { reload: ['userRules', 'rules'] }); return;
  }
  if (action === 'partition-add') { addPartition(); return; }
  if (action === 'partition-remove') {
    const name = target.dataset.name;
    showDialog({ title: t('ui.deletePartition'), body: `<p><code>${e(name)}</code></p>`, confirm: t('ui.remove'), submit: () => saveConfig('partitions', state.resources.config.data.partitions.filter(item => item !== name)) }); return;
  }
  if (action === 'partition-scan') {
    if (!configReady(state)) return;
    await runMutation('config:partitions', async () => {
      const scan = await api.getPartitionScan();
      const invalid = new Set(scan.invalid);
      const partitions = [...new Set([...effectivePartitions(), ...scan.partitions])].filter(name => !invalid.has(name));
      if (JSON.stringify(partitions) !== JSON.stringify(state.resources.config.data.partitions)) await api.saveConfig({ partitions });
    }, { reload: ['config', 'system'], success: 'ui.nextBoot' }); return;
  }
  if (action === 'kmi-edit') { editKmi(); return; }
  if (action === 'lkm-load' || action === 'lkm-unload') {
    const lkm = state.resources.lkm;
    if (!lkm.ready || lkm.error || lkm.data.builtin || lkm.data.external_management === false) return;
    const load = action === 'lkm-load';
    confirmAction(t(load ? 'ui.loadConfirm' : 'ui.unloadConfirm'), t(load ? 'ui.loadConfirmDesc' : 'ui.unloadConfirmDesc'), load ? 'lkm:load' : 'lkm:unload', () => load ? api.lkmLoad() : api.lkmUnload(), { reload: ['lkm', 'system', 'modules'] });
  }
}
document.addEventListener('click', event => {
  const target = event.target.closest?.('[data-tab],[data-action]');
  if (!target || target.disabled) return;
  if (target.dataset.tab) { if (!dialog.open) navigation.tab(target.dataset.tab); return; }
  void handleAction(target).catch(error => showToast(error.message || String(error)));
});
document.addEventListener('input', event => {
  const field = event.target.dataset.field;
  if (field === 'module-search') {
    state.moduleSearch = event.target.value;
    patchHtml(document.getElementById('module-results'), moduleResults(state));
  } else if (field === 'log-search') {
    state.logSearch = event.target.value;
    patchHtml(document.getElementById('log-results'), logResults(state));
  }
  if (dialog.contains(event.target) && dialogModel && !dialogModel.busy) {
    // Editing after an error starts a fresh, explicitly confirmed transaction.
    dialogModel.resume = null;
    document.getElementById('dialog-error').hidden = true;
  }
});
document.addEventListener('change', event => {
  const target = event.target.closest?.('md-switch[data-field]');
  if (!target || target.disabled) return;
  const key = target.dataset.field;
  const value = target.selected;
  const task = key === 'lkm_autoload'
    ? runMutation('lkm:autoload', () => api.lkmSetAutoload(value), { reload: ['lkm', 'config'] })
    : saveConfig(key, value);
  void task.catch(() => {}).finally(() => render());
});
document.getElementById('editor-form').addEventListener('submit', async event => {
  event.preventDefault();
  const model = dialogModel;
  if (!model || model.busy) return;
  const form = new FormData(event.currentTarget);
  activeMutation = null;
  document.getElementById('dialog-error').hidden = true;
  setDialogBusy(true);
  try {
    const verified = await (model.resume ? model.resume() : model.submit(form));
    if (dialogModel !== model) return;
    if (verified === false) {
      model.resume = activeMutation ? state.mutations[activeMutation]?.retry : model.resume;
      if (activeMutation) model.mutationKey = activeMutation;
      dialogError(t(state.mutations[model.mutationKey]?.label || 'ui.unconfirmed'));
    } else {
      model.requiredAck = false;
      setDialogBusy(false);
      navigation.back();
    }
  } catch (error) {
    if (dialogModel !== model) return;
    if (activeMutation) { model.resume = state.mutations[activeMutation]?.retry; model.mutationKey = activeMutation; }
    const phase = state.mutations[model.mutationKey]?.label || (error.outcomeUnknown ? 'ui.outcomeUnknown' : 'ui.applyFailed');
    dialogError(`${error.configPersisted || error.outcomeUnknown ? `${t(phase)}: ` : ''}${error.message || error}`);
  } finally {
    if (dialogModel === model) setDialogBusy(false);
  }
});
dialog.addEventListener('cancel', event => { event.preventDefault(); closeDialog(); });
dialog.addEventListener('click', event => {
  if (event.target !== dialog) return;
  const rect = dialog.getBoundingClientRect();
  if (event.clientX < rect.left || event.clientX > rect.right || event.clientY < rect.top || event.clientY > rect.bottom) closeDialog();
});
document.addEventListener('visibilitychange', () => {
  if (document.hidden) clearTimeout(pollTimer);
  else { if (!dialog.open) void refreshCurrent(); schedulePoll(); }
});
function updateViewport() { document.documentElement.style.setProperty('--viewport-height', `${window.visualViewport?.height || innerHeight}px`); }
window.visualViewport?.addEventListener('resize', updateViewport);
window.addEventListener('resize', updateViewport);
updateViewport();
applyPreferences();
mountApp();
navigation.recoverDialog();
try { void enableEdgeToEdge(true).catch(() => {}); } catch { /* Older hosts omit window controls. */ }
if (!state.preview) {
  for (const name of ['insets', 'colors']) {
    const link = document.createElement('link');
    link.rel = 'stylesheet'; link.href = `https://mui.kernelsu.org/internal/${name}.css`; link.media = 'print';
    link.onload = () => { link.media = 'all'; }; document.head.appendChild(link);
  }
}
void store.refresh([...new Set(['config', 'system', 'modules', 'storage', 'lkm', 'meta', ...pageResources(state.route)])]);
if (!state.preview && preference('kagami_experimental_ack', '') !== 'true') {
  showDialog({ title: t('ui.experimental'), body: `<p>${e(t('ui.experimentalBody'))}</p>`, confirm: t('ui.understand'), requiredAck: true, submit() { storePreference('kagami_experimental_ack', 'true'); return true; } });
}
schedulePoll();
