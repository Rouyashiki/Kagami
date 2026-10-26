// SPDX-License-Identifier: MIT
// All reads and writes share a queue so a late read cannot undo a newer edit.
export function createStore(api, notify = () => {}) {
  const resources = Object.fromEntries(['config', 'system', 'modules', 'storage', 'lkm', 'userRules', 'rules', 'meta', 'logs', 'conflicts', 'kasumiConflicts']
    .map(name => [name, { data: null, ready: false, loading: false, error: '' }]));
  const state = { resources, mutations: {}, pendingConfig: {}, logType: 'system', updated: '' };
  let queue = Promise.resolve();
  const reads = {
    config: () => api.loadConfig(), system: () => api.getSystemInfo(), modules: () => api.scanModules(),
    storage: () => api.getStorageUsage(), lkm: () => api.getLkmStatus(), userRules: () => api.getUserHideRules(true),
    rules: () => api.getAllRules(), meta: () => api.getMeta(), logs: () => api.readLogs(state.logType),
    conflicts: () => api.checkConflicts(),
    kasumiConflicts: () => api.getKasumiConflicts(),
  };
  const schedule = task => {
    const next = queue.catch(() => {}).then(task);
    queue = next;
    return next;
  };
  async function read(names) {
    names = [...new Set(names)];
    for (const name of names) resources[name].loading = true;
    notify();
    await Promise.all(names.map(async name => {
      const resource = resources[name];
      const requestedLogType = name === 'logs' ? state.logType : null;
      try {
        const value = await reads[name]();
        if (name === 'logs' && requestedLogType !== state.logType) return;
        resource.data = value; resource.ready = true; resource.error = '';
      } catch (error) {
        if (name !== 'logs' || requestedLogType === state.logType) resource.error = error.message || String(error);
      }
      finally { resource.loading = false; }
    }));
    state.updated = new Date().toLocaleTimeString();
    notify();
    return names.every(name => !resources[name].error);
  }
  const refresh = names => schedule(() => read(names));
  function mutate(key, operation, { reload = [], success = 'ui.saved', pendingConfig = null } = {}) {
    if (state.mutations[key]?.phase === 'saving') return state.mutations[key].promise;
    const previous = state.mutations[key];
    // A successful write with a failed read must only retry its readback.
    const unknownOutcome = previous?.phase === 'unknown' && previous.operation === operation;
    const retryRead = (previous?.phase === 'unconfirmed' || unknownOutcome) && previous.operation === operation;
    const record = { phase: 'saving', error: '', label: 'ui.saving', operation, retry: null, promise: null };
    state.mutations[key] = record;
    if (pendingConfig) Object.assign(state.pendingConfig, pendingConfig);
    record.retry = () => mutate(key, operation, { reload, success, pendingConfig });
    const promise = schedule(async () => {
      let outcome;
      try {
        if (!retryRead) outcome = await operation();
        const verified = await read(reload);
        record.successLabel = retryRead ? previous.successLabel : typeof success === 'function' ? success(outcome) : success;
        record.phase = unknownOutcome ? 'unknown' : verified ? 'saved' : 'unconfirmed';
        record.label = unknownOutcome ? previous.label : verified ? record.successLabel : 'ui.unconfirmed';
        if (unknownOutcome) record.error = previous.error;
        return verified && !unknownOutcome;
      } catch (error) {
        record.phase = error.outcomeUnknown ? 'unknown' : error.configPersisted ? 'applyFailed' : 'error';
        record.label = error.outcomeUnknown ? error.configPersisted ? 'ui.applyUnknown' : 'ui.outcomeUnknown' : error.configPersisted ? 'ui.applyFailed' : 'ui.saveFailed';
        record.error = error.message || String(error);
        await read(reload);
        throw error;
      } finally {
        if (pendingConfig) for (const field of Object.keys(pendingConfig)) delete state.pendingConfig[field];
        notify();
      }
    });
    record.promise = promise;
    notify();
    return promise;
  }
  return { state, refresh, mutate, settled: () => queue.catch(() => {}), read };
}
