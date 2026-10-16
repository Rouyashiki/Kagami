import assert from 'node:assert/strict';
import { test } from 'node:test';
import { createStore } from '../../../webui/state.js';
import { renderPage } from '../../../webui/pages.js';

function deferred() {
  let resolve, reject;
  const promise = new Promise((yes, no) => { resolve = yes; reject = no; });
  return { promise, resolve, reject };
}

test('a delayed refresh completes before queued writes and pending input stays visible', async () => {
  const firstRead = deferred(), readStarted = deferred();
  const events = [];
  let reads = 0;
  let saved = { debug: false, verbose: false };
  const store = createStore({
    async loadConfig() {
      events.push('read');
      if (++reads === 1) { readStarted.resolve(); return firstRead.promise; }
      return { ...saved };
    },
  });
  const refresh = store.refresh(['config']);
  await readStarted.promise;
  const write = store.mutate('config:debug', async () => {
    events.push('write');
    saved.debug = true;
  }, { reload: ['config'], pendingConfig: { debug: true } });
  assert.deepEqual(events, ['read']);
  assert.equal(store.state.pendingConfig.debug, true);
  firstRead.resolve({ debug: false, verbose: false });
  await refresh;
  await write;
  assert.deepEqual(events, ['read', 'write', 'read']);
  assert.equal(store.state.resources.config.data.debug, true);
  assert.deepEqual(store.state.pendingConfig, {});
});

test('independent config mutations retain each pending field and merge sequentially', async () => {
  const gate = deferred(), started = deferred();
  const saved = { debug: false, verbose: false };
  const store = createStore({ async loadConfig() { return { ...saved }; } });
  const debug = store.mutate('config:debug', async () => {
    started.resolve(); await gate.promise; saved.debug = true;
  }, { reload: ['config'], pendingConfig: { debug: true } });
  await started.promise;
  const verbose = store.mutate('config:verbose', async () => { saved.verbose = true; }, {
    reload: ['config'], pendingConfig: { verbose: true },
  });
  assert.deepEqual(store.state.pendingConfig, { debug: true, verbose: true });
  gate.resolve();
  await Promise.all([debug, verbose]);
  assert.deepEqual(store.state.resources.config.data, { debug: true, verbose: true });
  assert.deepEqual(store.state.pendingConfig, {});
});

test('successful write with failed readback retries only the read', async () => {
  let writes = 0, failRead = true;
  const store = createStore({ async scanModules() {
    if (failRead) throw new Error('bridge read failed');
    return [{ id: 'visible', mode: 'magic' }];
  } });
  const operation = async () => { writes += 1; };
  assert.equal(await store.mutate('module:visible:mode', operation, { reload: ['modules'] }), false);
  assert.equal(store.state.mutations['module:visible:mode'].phase, 'unconfirmed');
  failRead = false;
  assert.equal(await store.state.mutations['module:visible:mode'].retry(), true);
  assert.equal(writes, 1);
  assert.equal(store.state.mutations['module:visible:mode'].phase, 'saved');
});

test('configuration apply failure keeps persisted readback and remains a distinct failure', async () => {
  const config = { enable_mount_hide: true };
  const store = createStore({ async loadConfig() { return { ...config }; } });
  const error = Object.assign(new Error('kernel rejected apply'), { configPersisted: true });
  await assert.rejects(store.mutate('config:enable_mount_hide', async () => { throw error; }, {
    reload: ['config'], pendingConfig: { enable_mount_hide: true },
  }), /kernel rejected apply/);
  assert.equal(store.state.mutations['config:enable_mount_hide'].phase, 'applyFailed');
  assert.equal(store.state.resources.config.data.enable_mount_hide, true);
  assert.deepEqual(store.state.pendingConfig, {});
});

test('a persisted configuration with unknown apply outcome retries reads only and keeps its distinct label', async () => {
  let writes = 0;
  const store = createStore({ async loadConfig() { return { enable_mount_hide: true }; } });
  const operation = async () => {
    writes += 1;
    throw Object.assign(new Error('apply response timed out'), { configPersisted: true, outcomeUnknown: true });
  };
  await assert.rejects(store.mutate('config:enable_mount_hide', operation, { reload: ['config'] }), /timed out/);
  for (let attempt = 0; attempt < 2; attempt += 1) {
    const record = store.state.mutations['config:enable_mount_hide'];
    assert.equal(record.phase, 'unknown');
    assert.equal(record.label, 'ui.applyUnknown');
    assert.equal(await record.retry(), false);
  }
  assert.equal(writes, 1);
  assert.equal(store.state.resources.config.data.enable_mount_hide, true);
});

test('a result-dependent success label survives readback-only retries', async () => {
  let writes = 0, failRead = true, labelCalls = 0;
  const store = createStore({ async loadConfig() {
    if (failRead) throw new Error('read unavailable');
    return { enable_mount_hide: true };
  } });
  const operation = async () => { writes += 1; return { persisted: true, runtimeApplied: true }; };
  const success = result => { labelCalls += 1; return result?.runtimeApplied ? 'ui.runtimeApplied' : 'ui.saved'; };
  assert.equal(await store.mutate('config:enable_mount_hide', operation, { reload: ['config'], success }), false);
  assert.equal(store.state.mutations['config:enable_mount_hide'].phase, 'unconfirmed');
  failRead = false;
  assert.equal(await store.state.mutations['config:enable_mount_hide'].retry(), true);
  assert.equal(store.state.mutations['config:enable_mount_hide'].label, 'ui.runtimeApplied');
  assert.equal(writes, 1);
  assert.equal(labelCalls, 1);
});

test('read failures retain stale data without preventing unrelated resource refresh', async () => {
  let fail = false;
  const store = createStore({
    async scanModules() { if (fail) throw new Error('module query failed'); return [{ id: 'visible' }]; },
    async getLkmStatus() { return { loaded: !fail, valid: true }; },
  });
  assert.equal(await store.refresh(['modules', 'lkm']), true);
  fail = true;
  assert.equal(await store.refresh(['modules', 'lkm']), false);
  assert.deepEqual(store.state.resources.modules.data, [{ id: 'visible' }]);
  assert.equal(store.state.resources.modules.ready, true);
  assert.equal(store.state.resources.modules.error, 'module query failed');
  assert.equal(store.state.resources.lkm.data.loaded, false);
  assert.equal(store.state.resources.lkm.error, '');
});

test('duplicate submission while a mutation is saving shares one operation', async () => {
  const gate = deferred();
  let writes = 0;
  const store = createStore({});
  const operation = async () => { writes += 1; await gate.promise; };
  const first = store.mutate('lkm:load', operation);
  const second = store.mutate('lkm:load', operation);
  assert.equal(first, second);
  gate.resolve();
  await first;
  assert.equal(writes, 1);
});

test('an unknown write outcome never replays a destructive command or claims it was saved', async () => {
  let writes = 0;
  const store = createStore({ async getLkmStatus() { return { loaded: true, valid: true }; } });
  const operation = async () => {
    writes += 1;
    throw Object.assign(new Error('root command timed out'), { outcomeUnknown: true });
  };
  await assert.rejects(store.mutate('lkm:unload', operation, { reload: ['lkm'] }), /timed out/);
  assert.notEqual(store.state.mutations['lkm:unload'].phase, 'saved');
  try { await store.state.mutations['lkm:unload'].retry(); } catch (_error) { /* unknown result may remain rejected */ }
  assert.equal(writes, 1, 'retry must only read back an unknown write outcome');
  assert.notEqual(store.state.mutations['lkm:unload'].phase, 'saved');
});

test('a log response cannot be published under a different selected log source', async () => {
  const system = deferred(), kernel = deferred(), systemStarted = deferred(), kernelStarted = deferred();
  const published = [];
  const store = createStore({ async readLogs(type) {
    if (type === 'system') { systemStarted.resolve(); return system.promise; }
    kernelStarted.resolve(); return kernel.promise;
  } }, () => {
    published.push({ type: store.state.logType, text: store.state.resources.logs.data });
  });
  const first = store.refresh(['logs']);
  await systemStarted.promise;
  store.state.logType = 'kernel';
  const second = store.refresh(['logs']);
  system.resolve('SYSTEM OLD');
  await first;
  await kernelStarted.promise;
  kernel.resolve('KERNEL CURRENT');
  await second;
  assert.equal(published.some(({ type, text }) => type === 'kernel' && text === 'SYSTEM OLD'), false);
  assert.equal(store.state.resources.logs.data, 'KERNEL CURRENT');
});

test('uppercase native HIDE rules are rendered as confirmed active', () => {
  const { state } = createStore({});
  Object.assign(state, { route: { tab: 'kasumi', view: 'hide' } });
  state.resources.userRules = { ready: true, loading: false, error: '', data: ['/system/hidden'] };
  state.resources.rules = { ready: true, loading: false, error: '', data: [{ type: 'HIDE', path: '/system/hidden', active: true, isUserDefined: true }] };
  assert.match(renderPage(state), /Confirmed active/);
  assert.doesNotMatch(renderPage(state), /not observed as active/);
});
