# Kagami

## Controller diagnostics and isolation

The standalone controller keeps its own configuration and state in
`/data/adb/kagami`, independently of YukiSU's embedded controller. It no longer
installs a controller alias into YukiSU's private `ksu/bin` directory.

`daemon.log` uses DEBUG, INFO, WARN and ERROR records. Debug/verbose mode enables
query and per-rule details; mutations, backend selections and failures remain
visible without it. The first writer of each boot rotates the previous file to
`daemon.log.old`. Same-boot restarts preserve the current file.

Installation and post-fs-data prepare controller metadata as `adb_data_file`
with private runtime permissions. System/vendor/product payload trees retain
their own labels. This avoids treating Kagami's control files as system payloads
when Android init performs restorecon, without widening SELinux permissions.

Kagami is a new Android metamodule skeleton. The project keeps the old web UI lineage, but the native side is being rewritten in C++ with an opt-in Kasumi mount pipeline.

Backend priority:

1. OverlayFS
2. Magic Mount fallback
3. Kasumi (explicit per-module or global selection)

## Shared module mirror

OverlayFS and explicitly selected Kasumi modules share one non-`/data` mirror:
`/dev/kagami_mirror` by default. Its tmpfs/ext4/EROFS choice and optional
loop image are controlled through `mirror_dir`, `mirror_img`, and
`mirror_img_size_mb`. Magic Mount keeps its existing independent work tmpfs;
when every module resolves to Magic Mount, Kagami does not mount the shared
mirror at all.

The current tree intentionally contains only the project skeleton:

- `webui/`: readable Material 3 Expressive WebUI sources with offline controls.
- `tools/webui/`: pinned dependencies and WebUI build tools.
- `src/`: C++ daemon, Kasumi control plane, and hybrid mount backends.
- `third_party/lkmloader/`: standalone LKM loader submodule.
- `module/`: KernelSU/APatch metamodule packaging files.
- `CMakeLists.txt`: native binary, WebUI, and package targets.

Build locally:

```sh
git submodule update --init
cmake -S . -B build
cmake --build build --target kagamid lkmloader
```

Build the WebUI and package (requires Node.js 22 and npm):

```sh
cmake --build build --target package
```

For a browser preview or WebUI-only build:

```sh
cd tools/webui
npm ci --include=dev --no-audit --no-fund
npm run bundle
```

The bundle in `build/webui/` contains `index.html`, `config.json`, and
`icon.svg`. Page scripts, styles, icons, and dependency notices are embedded
in the HTML; the other two files preserve native WebUI host settings and
shortcut support. Package targets rebuild this output and copy it into
`webroot/`. `webui/` stays readable and does not need network access at runtime.
To package without a WebUI, configure `-DKAGAMI_PACKAGE_WEBUI=OFF` or pass
`--no-webui` to `build.sh package`.

## Kasumi LKM assets

Kagami manages Kasumi as an optional LKM. Put independently built `.ko` assets
under `module/kasumi/` before packaging, or build one directly with
`KDIR=/path/to/ddk ./build.sh lkm --kmi android15-6.6`. The local build and CI
both track [`Rouyashiki/Kasumi`](https://github.com/Rouyashiki/Kasumi) on `main`. The
accepted filename formats, full KMI matrix, and licensing details are in
`module/kasumi/README.md`. If no
compatible asset is installed, boot continues with the OverlayFS/Magic Mount
fallback.

Kagami first tries the normal `finit_module` path. If the kernel rejects a
module because a required symbol is no longer exported, the bundled
`lkmloader` submodule resolves undefined ELF symbols from the built-in kernel
portion of `/proc/kallsyms`, writes the patched image to a sealed memfd, and
retries through `finit_module`. The helper is an independent MIT-licensed
executable and does not depend on `ksud`; `init_module` is retained only as a
compatibility fallback when the fd-based path is unavailable. An exact
kmsg-confirmed vermagic mismatch rebuilds `.modinfo` and is retried once.

Kasumi API 18 retains ordinary mount hiding through `enable_mount_hide` and
`kagamid kasumi mount-hide off|on|normal`. The aggressive mode and its WebUI
selector are removed. Existing `mount_hide_mode` fields are ignored when
loading and removed by the next `config merge-json`; new writes to that field
are rejected. Update the controller and the Kasumi LKM together.

The C++ client includes the self-contained `include/kagami/kasumi_uapi.h`
directly, synchronized from Kasumi's `src/include/kasumi_uapi.h`.
`kagamid kasumi conflicts` reads the API 18 diagnostic queue;
`kagamid kasumi ack-conflicts SEQUENCE` acknowledges only through the displayed
snapshot. Sequences are decimal strings in JSON so WebUI does not lose 64-bit
precision. WebUI requires explicit confirmation and retains the warning if ACK
fails; later conflicts remain pending. Loading Kasumi itself can be refused
before any hook is installed, in which case the load error and kernel log carry
the diagnosis because a rejected LKM cannot retain its queue.

## Rule policy bypass

API 18 uses bit 0 of `kasumi_syscall_arg.type` as `KSM_RULE_BYPASS_POLICY`:
`type = 0` preserves the existing policy, and `type = 1` explicitly opts a rule
into global visibility or hiding. API 17 and existing controllers initialize
this field to zero. The structure size and ioctl encodings do not change.
This field is a flag mask, not a directory-entry file type.

The flag applies to `ADD_RULE`, `ADD_MERGE_RULE`, and `HIDE_RULE`; other bits
are rejected with `EINVAL`. Non-rule commands require zero. Materialized merge
children inherit the parent flag. Managed user hides use the same bit in
`kasumi_user_hide_arg.flags` on UPSERT, and QUERY returns the saved flags.
Other managed-hide requests still require zero input flags. RETRY preserves
the saved flag, including across storage rebinding.

Check `KSM_FEATURE_RULE_BYPASS` (feature bit 15, name `rule_bypass`) before
requesting bypass. Kagami rejects requests when this capability is unavailable
instead of silently treating them as ordinary rules.

Bypass rules apply even to processes excluded by policy, including privileged
and storage-service observers. They still obey Kasumi enablement, teardown,
and internal control/resolution recursion guards. Ordinary sibling rules keep
their existing policy. Bypass does not change mount hiding, maps spoofing, or
other unrelated features.

Manager and Kagami WebUI expose module and individual-rule switches, initially
off. The module flag applies to every Kasumi rule owned by that module; a
single path rule uses the existing longest-prefix selection. Effective bypass
is the module flag OR the selected path-rule flag. OverlayFS and Magic Mount
do not consume this setting. WebUI reads saved flags back before applying
module bypass and path-rule edits. Enabled, mountable modules already assigned
to Kasumi this boot are refreshed immediately; other modules retain the saved
settings for the next boot. Changing the mount backend still requires a reboot.
An apply failure is reported separately from a successful save. Saved user hides
are submitted immediately and retain their existing binding-status reporting.
Hide rule options are collapsed by default; expand a row or the add dialog's
rule options to change its global switch. A global badge stays visible on
collapsed saved rows.

CLI examples (use `ksud kagami` as the prefix in YukiSU):

```sh
kagamid module set-bypass example on
kagamid module add-rule example /system/etc/example kasumi --bypass
kagamid module apply example
kagamid hide add /data/local/tmp/example --bypass
kagamid hide list --details
```

Omitting `--bypass` writes zero; `set-bypass example off` disables the module
flag. Module rules persist a boolean `bypass`. Module-wide and user-hide
overrides are stored as `"on"` entries in `module_bypass.json` and
`hide_bypass.json`; absence means off. `hide list` retains the legacy string
array, while `--details` returns `{path, bypass}` objects. `LIST_RULES` reports
the numeric flags after ADD, MERGE, and HIDE records.

CLI module edits save settings; run `module apply <id>` after completing the
edits to refresh eligible Kasumi mappings. It returns `{"applied":true}` for a
successful refresh, `{"applied":false}` when the module must wait for boot,
and a nonzero exit status on failure. It never migrates a live OverlayFS or
Magic Mount module to Kasumi.
