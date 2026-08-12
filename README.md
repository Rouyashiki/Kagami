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
- `module/`: KernelSU/APatch metamodule packaging files.
- `CMakeLists.txt`: native binary, WebUI, and package targets.

Build locally:

```sh
cmake -S . -B build
cmake --build build --target kagamid
```

Build the WebUI and package:

```sh
cmake --build build --target package
```

## Kasumi LKM assets

Kagami manages Kasumi as an optional LKM. Put independently built `.ko` assets
under `module/kasumi/` before packaging, or build one directly with
`KDIR=/path/to/ddk ./build.sh lkm --kmi android15-6.6`. The local build and CI
both pin [`Anatdx/Kasumi`](https://github.com/Anatdx/Kasumi) at `fix/issues`
while API 17 is integrated with the hook-debt fixes. The accepted filename formats,
full KMI matrix, and licensing details are in `module/kasumi/README.md`. If no
compatible asset is installed, boot continues with the OverlayFS/Magic Mount
fallback.
