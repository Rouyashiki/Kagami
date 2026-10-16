/* SPDX-License-Identifier: Apache-2.0 */
import assert from 'node:assert/strict';
import { after, before, test } from 'node:test';
import { createServer } from 'node:http';
import { mkdir, readFile, unlink, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';
import { chromium } from 'playwright-core';
import { installHostFixture } from './browser.fixture.mjs';

const root = fileURLToPath(new URL('../../../', import.meta.url));
const output = path.join(root, 'build/webui-tests/logo');
const svg = await readFile(path.join(root, 'webui/icon.svg'), 'utf8');
const iconUrl = `data:image/svg+xml;base64,${Buffer.from(svg).toString('base64')}`;
const mime = { '.html': 'text/html', '.js': 'text/javascript', '.css': 'text/css', '.svg': 'image/svg+xml' };
const matrixResults = [];
const matrixImages = [];
const monochromeImages = [];
let browser;
let origin;
const server = createServer(async (request, response) => {
  try {
    if (request.url === '/') {
      response.writeHead(200, { 'Content-Type': 'text/html' });
      response.end(`<!doctype html><base href="/webui/"><link rel="stylesheet" href="/webui/styles.css">
        <style>body{padding:24px}.fixture-row{display:flex;align-items:center;gap:24px;padding:20px}.sample{padding:12px;background:#eee}.sample.dark{background:#222}.sample.color{background:#f3b9e1}</style>
        <main id="fixture"><div class="boot-panel"><span class="brand-mark"></span></div>
        <div class="side-brand"><span class="brand-mark"></span></div>
        <div class="module-intro"><span class="brand-mark"></span></div><div id="buttons" class="fixture-row"></div><div id="sizes" class="fixture-row"></div></main>
        <script type="module">import '/webui/assets/material.js';import {button} from '/webui/ui.js';
          document.querySelector('#buttons').innerHTML=['filled','tonal','text'].map(type=>button('check','Action',{},type,'plus')).join('');
          document.querySelector('#sizes').innerHTML=[24,32,36,48,64,72].map(size=>'<div class="sample"><span class="brand-mark" style="width:'+size+'px;height:'+size+'px"></span></div>').join('');
          await Promise.all([...document.querySelectorAll('#buttons > *')].map(button=>button.updateComplete));window.ready=true;</script>`);
      return;
    }
    const filename = path.resolve(root, decodeURIComponent(new URL(request.url, 'http://localhost').pathname).slice(1));
    assert.ok(filename.startsWith(path.join(root, 'webui') + path.sep));
    response.writeHead(200, { 'Content-Type': mime[path.extname(filename)] || 'application/octet-stream' });
    response.end(await readFile(filename));
  } catch {
    response.writeHead(404);
    response.end('Not found');
  }
});

before(async () => {
  await mkdir(output, { recursive: true });
  await new Promise(resolve => server.listen(0, '127.0.0.1', resolve));
  origin = `http://127.0.0.1:${server.address().port}`;
  browser = await chromium.launch({ headless: true, ...(process.env.KAGAMI_BROWSER_EXECUTABLE_PATH ? { executablePath: process.env.KAGAMI_BROWSER_EXECUTABLE_PATH } : {}) });
});
after(async () => {
  if (matrixResults.length) {
    await writeFile(path.join(output, 'actual-buttons-matrix.json'), JSON.stringify(matrixResults, null, 2) + '\n');
    const asImage = (data) => `data:image/png;base64,${data.toString('base64')}`;
    const html = `<!doctype html><meta charset="utf-8"><title>Kagami logo and actual button verification</title>
      <style>*{box-sizing:border-box}body{margin:0;padding:24px;background:#eff5f2;color:#18251f;font:14px Arial,sans-serif}h1{font-size:22px;margin:0 0 8px}p{margin:6px 0 18px}.monochrome{display:flex;gap:16px;margin-bottom:20px}.mono{padding:10px 18px;background:repeating-conic-gradient(#ccd4d0 0% 25%,#fff 0% 50%) 0/16px 16px;border-radius:12px}.mono img{display:block;width:96px;height:96px}.grid{display:grid;grid-template-columns:repeat(4,1fr);gap:12px}.cell{background:#fff;border:1px solid #d6e1da;border-radius:12px;padding:12px;min-width:0}.cell h2{font-size:14px;margin:0 0 8px}.sample{height:74px;display:flex;align-items:center;justify-content:space-between;gap:8px}.sample img{max-width:185px}.sample small{font-size:11px;color:#526058}</style>
      <h1>Kagami · logo and actual page buttons</h1><p>Black / white masks retain transparent cuts. Every button: one line, icon beside label, touch target ≥ 48 × 48 CSS px.</p>
      <div class="monochrome">${monochromeImages.map(({ color, data }) => `<div><div class="mono"><img src="${asImage(data)}"></div><p>${color}</p></div>`).join('')}</div>
      <div class="grid">${['en', 'zh-CN', 'ar'].flatMap(language => [320, 390, 840, 1280].map(width => `<div class="cell"><h2>${language} · ${width}px · ${language === 'ar' ? 'RTL' : 'LTR'}</h2>${matrixImages.filter(item => item.language === language && item.width === width).map(item => `<div class="sample"><small>${item.action === 'rule-add' ? 'Module rule' : 'Partition'}<br>${item.touchWidth.toFixed(0)} × ${item.touchHeight.toFixed(0)}</small><img src="${asImage(item.data)}"></div>`).join('')}</div>`)).join('')}</div>`;
    await writeFile(path.join(output, 'verification-report.html'), html);
    const report = await browser.newPage({ viewport: { width: 1280, height: 950 }, deviceScaleFactor: 1 });
    await report.setContent(html);
    await report.screenshot({ path: path.join(output, 'verification-report.png'), fullPage: true });
    await report.close();
  }
  await browser?.close();
  await new Promise(resolve => server.close(resolve));
});

test('the mirror and rounded reflection cuts have real transparency', async () => {
  const page = await browser.newPage();
  try {
    const result = await page.evaluate(async ({ svg, iconUrl }) => {
      const document = new DOMParser().parseFromString(svg, 'image/svg+xml');
      const image = new Image();
      image.src = iconUrl;
      await image.decode();
      const canvas = globalThis.document.createElement('canvas');
      canvas.width = canvas.height = 256;
      const context = canvas.getContext('2d');
      context.drawImage(image, 0, 0, 256, 256);
      const pixel = ([x, y]) => [...context.getImageData(Math.floor(x * 4), Math.floor(y * 4), 1, 1).data];
      return {
        viewBox: document.documentElement.getAttribute('viewBox'),
        paths: document.querySelectorAll('path').length,
        forbidden: document.querySelectorAll('defs,rect,circle,mask,linearGradient,radialGradient,filter,image,style').length,
        rule: document.querySelector('path').getAttribute('fill-rule'),
        clear: [[0, 0], [32, 4], [60, 32], [22, 32], [27, 27], [32, 22], [26, 42], [34, 34], [42, 26], [20.94, 33.06]].map(pixel),
        solid: [[14, 32], [32, 48], [48, 32], [19.88, 34.12]].map(pixel),
      };
    }, { svg, iconUrl });
    assert.equal(result.viewBox, '0 0 64 64');
    assert.equal(result.paths, 1);
    assert.equal(result.forbidden, 0);
    assert.equal(result.rule, 'evenodd');
    assert.ok(result.clear.every(pixel => pixel[3] === 0), JSON.stringify(result.clear));
    assert.ok(result.solid.every(pixel => pixel.join() === '15,108,100,255'), JSON.stringify(result.solid));
  } finally { await page.close(); }
});

test('brand masks follow light, dark and host colors without RTL mirroring', async () => {
  const page = await browser.newPage({ viewport: { width: 1100, height: 760 } });
  try {
    await page.goto(origin);
    await page.waitForFunction(() => window.ready);
    const colors = [];
    for (const theme of ['light', 'dark', 'host']) {
      const result = await page.evaluate(theme => {
        const root = document.documentElement;
        if (theme === 'host') { delete root.dataset.theme; root.style.setProperty('--primary', '#7654ab'); }
        else root.dataset.theme = theme;
        for (const sample of document.querySelectorAll('.sample')) sample.className = `sample ${theme === 'dark' ? 'dark' : theme === 'host' ? 'color' : ''}`;
        return [...document.querySelectorAll('.boot-panel .brand-mark,.side-brand .brand-mark,.module-intro .brand-mark')].map(element => {
          const style = getComputedStyle(element);
          return { width: element.getBoundingClientRect().width, color: style.backgroundColor, mask: style.maskImage || style.webkitMaskImage, transform: style.transform };
        });
      }, theme);
      assert.deepEqual(result.map(item => item.width), [48, 36, 72]);
      assert.ok(result.every(item => item.mask.includes('icon.svg') && item.transform === 'none'));
      assert.equal(new Set(result.map(item => item.color)).size, 1);
      colors.push(result[0].color);
      await page.locator('#sizes').screenshot({ path: path.join(output, `${theme}-24-to-72.png`) });
    }
    assert.equal(new Set(colors).size, 3);
    assert.equal(colors[2], 'rgb(118, 84, 171)');
    const mark = page.locator('.module-intro .brand-mark');
    const ltr = await mark.screenshot();
    await page.evaluate(() => { document.documentElement.dir = 'rtl'; });
    assert.deepEqual(await mark.screenshot(), ltr);
  } finally { await page.close(); }
});

test('Material button icons occupy the icon slot beside the label in both directions', async () => {
  const page = await browser.newPage({ viewport: { width: 1100, height: 760 } });
  try {
    await page.goto(origin);
    await page.waitForFunction(() => window.ready);
    await page.waitForFunction(() => [...document.querySelectorAll('#buttons svg')].every(svg => svg.getBBox().width > 0));
    for (const direction of ['ltr', 'rtl']) {
      const positions = await page.evaluate(direction => {
        document.documentElement.dir = direction;
        return [...document.querySelectorAll('#buttons > *')].map(button => {
          const icon = button.querySelector('svg');
          const iconBox = icon.getBoundingClientRect();
          const labelBox = button.shadowRoot.querySelector('.label').getBoundingClientRect();
          const style = getComputedStyle(icon);
          return { slot: icon.slot, margin: style.marginInlineEnd, iconX: iconBox.x, iconEnd: iconBox.right, labelX: labelBox.x, labelEnd: labelBox.right, deltaY: Math.abs(iconBox.y + iconBox.height / 2 - labelBox.y - labelBox.height / 2) };
        });
      }, direction);
      for (const position of positions) {
        assert.equal(position.slot, 'icon');
        assert.equal(position.margin, '0px');
        assert.ok(position.deltaY < 1, JSON.stringify(position));
        assert.ok(direction === 'ltr' ? position.iconEnd < position.labelX : position.labelEnd < position.iconX, JSON.stringify(position));
      }
      await page.locator('#buttons').screenshot({ path: path.join(output, `buttons-${direction}.png`) });
    }
  } finally { await page.close(); }
});

test('pure black and pure white production masks preserve transparent cutouts', async () => {
  const page = await browser.newPage({ viewport: { width: 600, height: 600 }, deviceScaleFactor: 1 });
  try {
    await page.goto(origin);
    await page.waitForFunction(() => window.ready);
    await page.evaluate(() => {
      document.documentElement.style.background = 'transparent';
      document.body.style.background = 'transparent';
      document.querySelector('#fixture').innerHTML = '<span id="monochrome" class="brand-mark" style="width:256px;height:256px"></span>';
    });
    for (const [color, value] of [['black', 0], ['white', 255]]) {
      await page.evaluate(color => document.documentElement.style.setProperty('--md-sys-color-primary', color), color);
      const data = await page.locator('#monochrome').screenshot({ path: path.join(output, `${color}-transparent.png`), omitBackground: true });
      const pixels = await page.evaluate(async imageUrl => {
        const image = new Image(); image.src = imageUrl; await image.decode();
        const canvas = document.createElement('canvas'); canvas.width = canvas.height = 256;
        const context = canvas.getContext('2d'); context.drawImage(image, 0, 0);
        const pixel = ([x, y]) => [...context.getImageData(Math.floor(x * 4), Math.floor(y * 4), 1, 1).data];
        return { clear: [[0, 0], [32, 4], [22, 32], [27, 27], [32, 22], [26, 42], [34, 34], [42, 26], [20.94, 33.06]].map(pixel), solid: [[14, 32], [32, 48], [48, 32], [19.88, 34.12]].map(pixel) };
      }, `data:image/png;base64,${data.toString('base64')}`);
      assert.ok(pixels.clear.every(pixel => pixel[3] === 0), `${color} cutouts: ${JSON.stringify(pixels.clear)}`);
      assert.ok(pixels.solid.every(pixel => pixel.join() === `${value},${value},${value},255`), `${color} fill: ${JSON.stringify(pixels.solid)}`);
      monochromeImages.push({ color, data });
    }
  } finally { await page.close(); }
});

test('actual rule and partition buttons retain horizontal layout and 48px touch targets across languages and widths', async () => {
  for (const language of ['en', 'zh-CN', 'ar']) {
    const context = await browser.newContext({ viewport: { width: 390, height: 844 }, deviceScaleFactor: 1, reducedMotion: 'reduce' });
    const errors = [], external = [], failed = [];
    context.on('page', page => {
      page.on('pageerror', error => errors.push(error.message));
      page.on('response', response => { if (response.status() >= 400) failed.push(`${response.status()} ${response.url()}`); });
    });
    await context.route('**/*', route => {
      if (/^https:\/\/mui\.kernelsu\.org\/internal\/(colors|insets)\.css$/.test(route.request().url())) return route.abort();
      if (new URL(route.request().url()).origin !== origin) { external.push(route.request().url()); return route.abort(); }
      return route.continue();
    });
    const page = await context.newPage();
    await context.addInitScript(installHostFixture, { language });
    page.setDefaultTimeout(8000);
    try {
      for (const width of [320, 390, 840, 1280]) {
        await page.setViewportSize({ width, height: 844 });
        for (const [action, query] of [['rule-add', '?tab=modules&view=module&id=alpha_fonts'], ['partition-add', '?tab=settings&view=partitions']]) {
          await page.goto(`${origin}/webui/index.html${query}`, { waitUntil: 'networkidle' });
          const button = page.locator(`[data-action="${action}"]`);
          await button.waitFor();
          await button.scrollIntoViewIfNeeded();
          await page.waitForFunction(action => {
            const button = document.querySelector(`[data-action="${action}"]`);
            return button && !button.disabled && button.querySelector('svg')?.getBBox().width > 0;
          }, action);
          const geometry = await button.evaluate(button => {
            const icon = button.querySelector('svg'), label = button.shadowRoot.querySelector('.label');
            const iconBox = icon.getBoundingClientRect(), labelBox = label.getBoundingClientRect();
            const touch = button.shadowRoot.querySelector('.touch').getBoundingClientRect();
            const host = button.getBoundingClientRect();
            return { direction: document.documentElement.dir, language: document.documentElement.lang, text: button.textContent.trim(), slot: icon.slot, iconX: iconBox.x, iconEnd: iconBox.right, labelX: labelBox.x, labelEnd: labelBox.right, labelHeight: labelBox.height, deltaY: Math.abs(iconBox.y + iconBox.height / 2 - labelBox.y - labelBox.height / 2), touchX: touch.x, touchY: touch.y, touchWidth: touch.width, touchHeight: touch.height, hostWidth: host.width, hostHeight: host.height, pageWidth: document.documentElement.scrollWidth, viewportWidth: innerWidth };
          });
          const name = `${language}-${width}-${action}`;
          assert.equal(geometry.language, language, name);
          assert.equal(geometry.direction, language === 'ar' ? 'rtl' : 'ltr', name);
          assert.equal(geometry.slot, 'icon', name);
          assert.ok(geometry.deltaY < 1, `${name}: icon and label must be vertically centered: ${JSON.stringify(geometry)}`);
          assert.ok(geometry.direction === 'rtl' ? geometry.labelEnd < geometry.iconX : geometry.iconEnd < geometry.labelX, `${name}: icon and label must occupy one horizontal row`);
          assert.ok(geometry.labelHeight <= 24, `${name}: label wraps onto multiple lines`);
          assert.ok(Math.min(geometry.touchWidth, geometry.touchHeight, geometry.hostWidth, geometry.hostHeight) >= 48, `${name}: touch target below 48px: ${JSON.stringify(geometry)}`);
          assert.ok(geometry.pageWidth <= geometry.viewportWidth + 1, `${name}: horizontal page overflow`);
          const data = await button.screenshot({ path: path.join(output, `${name}.png`) });
          matrixImages.push({ language, width, action, data, touchWidth: geometry.touchWidth, touchHeight: geometry.touchHeight });
          // Material's native button content box is smaller than its .touch
          // element. Exercise both extended vertical edges, outside the label.
          for (const edge of ['top', 'bottom']) {
            await button.scrollIntoViewIfNeeded();
            await page.evaluate(() => new Promise(resolve => requestAnimationFrame(() => requestAnimationFrame(resolve))));
            const touch = await button.evaluate(button => {
              const box = button.shadowRoot.querySelector('.touch').getBoundingClientRect();
              return { x: box.x, y: box.y, width: box.width, height: box.height };
            });
            const point = { x: touch.x + touch.width / 2, y: touch.y + (edge === 'top' ? 1 : touch.height - 1) };
            await page.evaluate(() => {
              window.__buttonEvents = [];
              if (window.__buttonEventsInstalled) return;
              window.__buttonEventsInstalled = true;
              for (const type of ['pointerdown', 'pointerup', 'click']) document.addEventListener(type, event => {
                window.__buttonEvents.push({ type, y: event.clientY, target: event.target.tagName, action: event.target.getAttribute('data-action'), path: event.composedPath().slice(0, 5).map(element => element.tagName || element.nodeName), open: document.querySelector('#editor').open });
              }, true);
            });
            const hit = await page.evaluate(({ x, y }) => {
              const target = document.elementFromPoint(x, y);
              return { tag: target?.tagName, action: target?.getAttribute('data-action'), shadow: target?.shadowRoot?.elementFromPoint(x, y)?.outerHTML.slice(0, 180) };
            }, point);
            await page.mouse.click(point.x, point.y);
            try { await page.locator('#editor[open]').waitFor(); }
            catch (error) {
              await page.screenshot({ path: path.join(output, `${name}-${edge}-failed.png`), fullPage: true });
              const events = await page.evaluate(() => window.__buttonEvents);
              throw new Error(`${name} ${edge} touch edge failed: ${JSON.stringify({ point, hit, geometry, events })}`, { cause: error });
            }
            assert.equal(await page.locator(`#editor input[name="${action === 'rule-add' ? 'path' : 'value'}"]`).count(), 1, `${name}: ${edge} touch edge opens the expected editor`);
            await page.locator('[data-action="dialog-cancel"]').click();
            await page.locator('#editor').waitFor({ state: 'hidden' });
            await unlink(path.join(output, `${name}-${edge}-failed.png`)).catch(error => { if (error.code !== 'ENOENT') throw error; });
          }
          assert.deepEqual(await page.evaluate(() => window.__kagamiFixture.unhandled), [], name);
          matrixResults.push({ language, width, action, status: 'passed', ...geometry });
        }
      }
      assert.deepEqual(errors, [], 'Uncaught browser errors');
      assert.deepEqual(external, [], 'Unexpected external requests');
      assert.deepEqual(failed, [], 'Missing local resources');
    } finally { await context.close(); }
  }
});
