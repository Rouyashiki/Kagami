# Kagami

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

- `webui/`: migrated React/Vite WebUI, renamed to Kagami/Kasumi paths and commands.
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

Build the WebUI and package:

```sh
cmake --build build --target package
```

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

`mount_hide_mode` selects the Kasumi mount-hide level. `normal` removes
root-owned mounts while preserving the real zygote_next shared namespace view;
`aggressive` also projects shared propagation and `/proc/*/ns/mnt` links. The
default is `normal`, and the aggressive setting requires a Kasumi module that
advertises `KSM_FEATURE_MOUNT_HIDE_AGGRESSIVE`.
