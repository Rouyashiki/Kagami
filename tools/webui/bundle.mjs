/* SPDX-License-Identifier: Apache-2.0 */
/*
 * Kagami - Bundle and minify the module WebUI for distribution.
 *
 * License: Apache-2.0
 *
 * Author: Anatdx
 */

import { build } from 'esbuild';
import { XMLBuilder, XMLParser } from 'fast-xml-parser';
import { minify } from 'html-minifier-terser';
import { copyFile, mkdir, readFile, rm, writeFile } from 'node:fs/promises';
import path from 'node:path';
import { fileURLToPath } from 'node:url';

const root = fileURLToPath(new URL('../../', import.meta.url));
const source = path.join(root, 'webui');
const output = path.join(root, 'build/webui');

// Only generated files live here. Rebuild from scratch so removed assets cannot
// survive into a module package; never rewrite the checked-in source assets.
await rm(output, { recursive: true, force: true });
await mkdir(output, { recursive: true });

const iconData = await readFile(path.join(source, 'icon.svg'));
const iconUrl = `data:image/svg+xml;base64,${iconData.toString('base64')}`;
const parser = new XMLParser({ ignoreAttributes: false, preserveOrder: true });
const builder = new XMLBuilder({ ignoreAttributes: false, preserveOrder: true });
const spriteSource = await readFile(path.join(source, 'assets/icons.svg'), 'utf8');
const spriteNotice = spriteSource.slice(0, spriteSource.indexOf('<svg'));
const sprite = parser.parse(spriteSource);
const svg = sprite.find((element) => element.svg);
if (!svg) throw new Error('Missing WebUI icon sprite');
Object.assign(svg[':@'], {
  '@_width': '0', '@_height': '0', '@_aria-hidden': 'true',
  '@_style': 'position:absolute;pointer-events:none',
});
for (const element of svg.svg) {
  if (element.symbol) element[':@']['@_id'] = `kg-icon-${element[':@']['@_id']}`;
}

const notice = '/* Kagami WebUI. Derived from KOWX712/ksu-webui-demo and YukiZygisk. Authors: KOWX712 and Anatdx. */';
const result = await build({
  absWorkingDir: source,
  entryPoints: ['index.js', 'styles.css'],
  outdir: output,
  write: false,
  bundle: true,
  format: 'esm',
  target: 'chrome87',
  loader: { '.svg': 'dataurl' },
  supported: { 'template-literal': false, 'inline-script': true, 'inline-style': true },
  minify: true,
  legalComments: 'eof',
  banner: {
    js: `/* SPDX-License-Identifier: MIT AND Apache-2.0 AND BSD-3-Clause AND 0BSD */\n${notice}`,
    css: `/* SPDX-License-Identifier: MIT */\n${notice}`,
  },
  logLevel: 'warning',
  plugins: [{
    name: 'inline-webui-assets',
    setup(context) {
      context.onLoad({ filter: /\.js$/ }, async ({ path: filename }) => {
        const relative = path.relative(source, filename);
        if (relative.startsWith('..') || path.isAbsolute(relative) || relative.split(path.sep)[0] === 'assets') return;
        const contents = await readFile(filename, 'utf8');
        return {
          contents: contents.replaceAll('./assets/icons.svg#', '#kg-icon-')
            .replaceAll('./icon.svg', iconUrl),
          loader: 'js',
        };
      });
    },
  }],
});

function replaceRequired(text, search, replacement) {
  if (!text.includes(search)) throw new Error(`Missing WebUI entry: ${search}`);
  // A callback preserves literal dollar signs in generated JavaScript/CSS.
  return text.replaceAll(search, () => replacement);
}

const notices = await Promise.all(['LICENSE.webui', 'LICENSE.material', 'LICENSE.lit', 'LICENSE.tslib', 'LICENSE.lucide']
  .map(async (name) => `${name}\n${await readFile(path.join(source, 'assets', name), 'utf8')}`));
const licenseNotice = `<!-- Original licenses:\n${notices.join('\n\n').replaceAll('--', '&#45;&#45;')}\n-->`;
let html = await readFile(path.join(source, 'index.html'), 'utf8');
html = replaceRequired(html, './icon.svg', iconUrl);
html = replaceRequired(html, '<link rel="stylesheet" href="./styles.css">', '<style>__KG_BUNDLED_CSS__</style>');
html = replaceRequired(html, '<script type="module" src="./index.js"></script>', '<script type="module">__KG_BUNDLED_JS__</script>');
html = replaceRequired(html, '<body>', `<body>${licenseNotice}${spriteNotice}${builder.build(sprite)}`);
html = await minify(html, {
  collapseWhitespace: true,
  conservativeCollapse: true,
  caseSensitive: true,
  removeComments: true,
  ignoreCustomComments: [/SPDX-License-Identifier:|Original licenses:|Copyright/],
});
for (const [name, marker, endTag] of [
  ['index.js', '__KG_BUNDLED_JS__', 'script'],
  ['styles.css', '__KG_BUNDLED_CSS__', 'style'],
]) {
  const file = result.outputFiles.find((entry) => path.basename(entry.path) === name);
  if (!file) throw new Error(`Missing WebUI bundle: ${name}`);
  // esbuild escapes inline literals; also protect retained legal comments from
  // terminating an HTML raw-text element.
  const text = file.text.replace(new RegExp(`</${endTag}`, 'gi'), `<\\/${endTag}`);
  html = replaceRequired(html, marker, text);
}
await writeFile(path.join(output, 'index.html'), html + '\n');

// The page itself is self-contained. WebUI X reads these two files natively,
// before HTML loads: window/back/exit settings and the shortcut icon cannot be
// replaced by inline page resources.
await copyFile(path.join(source, 'icon.svg'), path.join(output, 'icon.svg'));
const config = JSON.parse(await readFile(path.join(source, 'config.json'), 'utf8'));
await writeFile(path.join(output, 'config.json'), JSON.stringify(config) + '\n');
console.log(`Bundled WebUI: ${output}/index.html (${Buffer.byteLength(html + '\n')} bytes), plus native host config.json and icon.svg.`);
