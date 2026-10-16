// SPDX-License-Identifier: MIT
import { TABS, VIEW_PARENTS } from './pages.js';

export function createNavigation({ onRoute, onDialogBack }) {
  let pendingTab = null;
  let backing = false;
  const scroll = new Map();
  const routeKey = route => `${route.tab}/${route.view || ''}/${route.id || ''}`;
  const legacyTabs = { overview: 'status', config: 'settings', info: 'settings', logs: 'status' };
  const params = new URLSearchParams(location.search);
  let tab = legacyTabs[params.get('tab')] || params.get('tab') || 'status';
  if (!TABS.some(item => item.id === tab)) tab = 'status';
  let view = params.get('view') || ({ info: 'about', logs: 'logs' })[params.get('tab')] || '';
  if (!VIEW_PARENTS[view]) view = '';
  if (view) tab = VIEW_PARENTS[view];
  let current = { tab, view, id: view === 'module' ? params.get('id') || '' : '' };
  function url(route) {
    const target = new URL(location.href);
    target.searchParams.set('tab', route.tab);
    for (const name of ['view', 'id']) {
      if (route[name]) target.searchParams.set(name, route[name]); else target.searchParams.delete(name);
    }
    return target;
  }
  function entry(route, depth, dialog = false) { return { kagami: true, route, depth, dialog }; }
  history.scrollRestoration = 'manual';
  if (!history.state?.kagami) {
    const parent = { tab, view: '', id: '' };
    history.replaceState(entry(parent, 0), '', url(parent));
    if (view) history.pushState(entry(current, 1), '', url(current));
  }
  function publish(route) {
    const previous = current;
    scroll.set(routeKey(previous), window.scrollY);
    current = route;
    onRoute(route, routeKey(previous) !== routeKey(route));
    requestAnimationFrame(() => window.scrollTo({ top: scroll.get(routeKey(route)) || 0, behavior: 'instant' }));
  }
  window.addEventListener('popstate', () => {
    backing = false;
    if (onDialogBack() === false) {
      history.pushState(entry(current, (history.state?.depth || 0) + 1, true), '', url(current));
      return;
    }
    if (pendingTab) {
      const route = { tab: pendingTab, view: '', id: '' };
      pendingTab = null;
      history.replaceState(entry(route, 0), '', url(route));
      publish(route);
      return;
    }
    const route = history.state?.kagami ? history.state.route : { tab: 'status', view: '', id: '' };
    publish(route);
  });
  return {
    get route() { return current; },
    tab(next) {
      if (backing || !TABS.some(item => item.id === next)) return;
      const depth = history.state?.depth || 0;
      if (depth) { backing = true; pendingTab = next; history.go(-depth); return; }
      const route = { tab: next, view: '', id: '' };
      history.replaceState(entry(route, 0), '', url(route));
      publish(route);
    },
    open(view, id = '') {
      if (backing || !VIEW_PARENTS[view]) return;
      const route = { tab: VIEW_PARENTS[view], view, id };
      history.pushState(entry(route, (history.state?.depth || 0) + 1), '', url(route));
      publish(route);
    },
    back() { if (!backing) { backing = true; history.back(); } },
    openDialog() { history.pushState(entry(current, (history.state?.depth || 0) + 1, true), '', url(current)); },
    recoverDialog() { if (history.state?.dialog) this.back(); },
  };
}
