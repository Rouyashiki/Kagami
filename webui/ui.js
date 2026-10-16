// SPDX-License-Identifier: MIT
// Material layout primitives adapted from YukiZygisk / KOWX712/ksu-webui-demo.
import { translations, isRtlLanguage } from './i18n.js';
import { messages } from './messages.js';

let language = 'en';
export function setLanguage(value) {
  language = value;
  document.documentElement.lang = value;
  document.documentElement.dir = isRtlLanguage(value) ? 'rtl' : 'ltr';
}
const lookup = (dictionary, key) => key.split('.').reduce((value, part) => value?.[part], dictionary);
export function t(key, values = {}) {
  const result = messages[language]?.[key] ?? lookup(translations[language], key)
    ?? messages.en[key] ?? lookup(translations.en, key) ?? key;
  return String(result).replace(/\{(\w+)\}/g, (_, name) => String(values[name] ?? `{${name}}`));
}
export function escapeHtml(value) {
  return String(value ?? '').replace(/[&<>"']/g, char => ({ '&': '&amp;', '<': '&lt;', '>': '&gt;', '"': '&quot;', "'": '&#39;' })[char]);
}
export const e = escapeHtml;
export function icon(name, className = '', slot = '') {
  return `<svg class="icon ${e(className)}" viewBox="0 0 24 24" aria-hidden="true"${slot ? ` slot="${e(slot)}"` : ''}><use href="./assets/icons.svg#${e(name)}"></use></svg>`;
}
export function attrs(values = {}) {
  return Object.entries(values).filter(([, value]) => value !== undefined && value !== false)
    .map(([key, value]) => `${key}="${e(value === true ? '' : value)}"`).join(' ');
}
export function iconButton(action, symbol, label, data = {}) {
  return `<md-icon-button ${attrs({ 'data-action': action, 'aria-label': label, title: label, ...data })}>${icon(symbol)}</md-icon-button>`;
}
export function button(action, label, data = {}, variant = 'tonal', symbol = '') {
  const tag = variant === 'filled' ? 'md-filled-button' : variant === 'text' ? 'md-text-button' : 'md-filled-tonal-button';
  return `<${tag} ${attrs({ 'data-action': action, ...data })}>${symbol ? icon(symbol, '', 'icon') : ''}${e(label)}</${tag}>`;
}
export function row({ title, subtitle = '', symbol = '', action = '', data = {}, trailing = '', key = '', disabled = false }) {
  const tag = action ? 'button' : 'div';
  return `<${tag} class="list-row" ${attrs({ type: action ? 'button' : undefined, 'data-key': key || undefined, 'data-action': action || undefined, disabled, ...data })}>
    ${symbol ? `<span class="row-symbol">${icon(symbol)}</span>` : ''}
    <span class="row-copy"><strong>${e(title)}</strong>${subtitle ? `<small>${e(subtitle)}</small>` : ''}</span>
    ${trailing || (action ? icon('chevron-right', 'chevron') : '')}${action ? '<md-ripple></md-ripple>' : ''}</${tag}>`;
}
export function section(title, body, { action = '', key = '', className = '' } = {}) {
  return `<section class="section ${e(className)}" ${attrs({ 'data-key': key || undefined })}><div class="section-heading"><h2>${e(title)}</h2>${action}</div>${body}</section>`;
}
export function info(items) {
  return `<dl class="info-list">${items.map(([label, value, code]) => `<div data-key="${e(label)}"><dt>${e(label)}</dt><dd>${code ? `<code dir="ltr">${e(value ?? '—')}</code>` : e(value ?? '—')}</dd></div>`).join('')}</dl>`;
}
export function badge(label, tone = 'muted') { return `<span class="badge ${e(tone)}">${e(label)}</span>`; }
export function empty(label = t('ui.empty'), symbol = 'boxes') { return `<div class="empty-state">${icon(symbol)}<p>${e(label)}</p></div>`; }
export function notice(title, body = '', tone = '', extra = '') {
  return `<div class="notice ${e(tone)}" role="${tone === 'danger' ? 'alert' : 'status'}">${icon(tone === 'danger' ? 'circle-alert' : 'info')}<div><strong>${e(title)}</strong>${body ? `<p>${e(body)}</p>` : ''}${extra}</div></div>`;
}
export function resourceState(resource, key) {
  if (resource.error) return `<div data-key="resource-${e(key)}">${notice(t('ui.readFailed'), `${resource.error}${resource.ready ? ` · ${t('ui.stale')}` : ''}`, 'danger', button('retry-resource', t('ui.retry'), { 'data-resource': key }, 'text'))}</div>`;
  if (!resource.ready) return `<div class="loading-state" data-key="resource-${e(key)}" role="status">${icon('loader-circle', 'spinning')} ${e(t('ui.loading'))}</div>`;
  return '';
}
export function modeLabel(mode) {
  return ({ auto: t('modules.modeAuto'), overlay: 'OverlayFS', overlayfs: 'OverlayFS', magic: 'Magic Mount', kasumi: 'Kasumi', none: t('modules.modeNone'), hide: t('ui.hide') })[mode] || mode || t('ui.unknown');
}
export function search(field, value, placeholder) {
  return `<label class="search-field" data-key="search-${e(field)}">${icon('search')}<input type="search" data-field="${e(field)}" value="${e(value)}" placeholder="${e(placeholder)}" aria-label="${e(placeholder)}" autocomplete="off" spellcheck="false"></label>`;
}
export function radioOptions(options, selected, name = 'value') {
  return `<div class="radio-list">${options.map(option => `<label class="radio-option"><span class="row-copy"><strong>${e(option.label)}</strong>${option.description ? `<small>${e(option.description)}</small>` : ''}</span><input type="radio" name="${e(name)}" value="${e(option.value)}" ${String(option.value) === String(selected) ? 'checked' : ''} ${option.disabled ? 'disabled' : ''}></label>`).join('')}</div>`;
}
export function textField(label, value, { name = 'value', type = 'text', hint = '', required = true, placeholder = '' } = {}) {
  return `<label class="text-field"><span>${e(label)}</span><input ${attrs({ name, value, type, required, placeholder, autocomplete: 'off', spellcheck: 'false', dir: 'auto' })}>${hint ? `<small>${e(hint)}</small>` : ''}</label>`;
}

// Patch light DOM in place. Search inputs, disclosure state and custom-element
// instances survive telemetry updates; dialog drafts live outside this tree.
function nodeKey(node) { return node.nodeType === 1 ? node.getAttribute('data-key') || node.id : ''; }
function sameKind(a, b) { return a.nodeType === b.nodeType && a.nodeName === b.nodeName && nodeKey(a) === nodeKey(b); }
function reconcile(oldNode, newNode) {
  if (oldNode.nodeType !== 1) {
    if (oldNode.nodeValue !== newNode.nodeValue) oldNode.nodeValue = newNode.nodeValue;
    return;
  }
  const activeInput = oldNode === document.activeElement && /^(INPUT|TEXTAREA|SELECT)$/.test(oldNode.tagName);
  for (const { name } of [...oldNode.attributes]) {
    if (!newNode.hasAttribute(name) && !(name === 'open' && oldNode.tagName === 'DETAILS')) oldNode.removeAttribute(name);
  }
  for (const { name, value } of [...newNode.attributes]) {
    if (activeInput && name === 'value') continue;
    if (oldNode.getAttribute(name) !== value) oldNode.setAttribute(name, value);
  }
  if (oldNode.tagName === 'INPUT' && !activeInput && oldNode.value !== newNode.value) oldNode.value = newNode.value;
  if (oldNode.tagName === 'MD-SWITCH') oldNode.selected = newNode.hasAttribute('selected');
  morphChildren(oldNode, newNode);
}
function morphChildren(parent, desired) {
  let cursor = parent.firstChild;
  for (const next of [...desired.childNodes]) {
    let match = cursor;
    if (!match || !sameKind(match, next)) {
      match = nodeKey(next) ? [...parent.childNodes].find(child => sameKind(child, next)) : null;
      if (match) parent.insertBefore(match, cursor);
      else { match = next.cloneNode(true); parent.insertBefore(match, cursor); }
    }
    reconcile(match, next);
    cursor = match.nextSibling;
  }
  while (cursor) { const next = cursor.nextSibling; cursor.remove(); cursor = next; }
}
export function patchHtml(element, markup) {
  const template = document.createElement('template');
  template.innerHTML = markup;
  morphChildren(element, template.content);
}
