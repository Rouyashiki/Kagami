# Kasumi LKM assets

Kagami can build and package Kasumi modules directly. The build source tracks
[`Rouyashiki/Kasumi`](https://github.com/Rouyashiki/Kasumi) on **`main`**.

For a prepared Android kernel/DDK tree:

```sh
./build.sh lkm --ddk \
  --kasumi-dir /path/to/Kasumi --kmi android15-6.6
```

To include that asset in a module ZIP in the same command:

```sh
./build.sh package --with-lkm --ddk \
  --kasumi-dir /path/to/Kasumi --kmi android15-6.6
```

The helper copies the Kasumi source into Kagami's ignored `build/` directory,
so it never creates generated objects in your Kasumi checkout. Kagami discovers
the following packaged filenames, in order:

- `<kmi>_<arch>_kasumi_lkm.ko`, for example `android15-6.6_arm64_kasumi_lkm.ko`
- `<arch>_kasumi_lkm.ko`
- `kasumi_lkm.ko`

## Protocol baseline

Every supplied LKM must implement **Kasumi API 17**. The daemon is the sole
owner of the Kasumi client and capability FD; Manager commands are forwarded
to that daemon against the same UAPI header, whose `KSM_PROTOCOL_VERSION` is
17. A YukiSU hymo API 16 asset is not a compatible replacement. By default
Kagami rejects a version mismatch and falls back to OverlayFS/Magic Mount
rather than activating a mixed ABI.

GitHub Actions builds and bundles this API 17 matrix with the same DDK target
set as Kasumi/YukiSU: `android12-5.10`, `android13-5.10`, `android13-5.15`,
`android14-5.15`, `android14-6.1`, `android15-6.6`, and `android16-6.12`.

`kagamid lkm status` reports the detected KMI and selected asset. The module
loader deliberately keeps assets external to the binary, so Kagami can remain
a standalone metamodule and update compatible KMI builds independently.

When Kasumi is selected for a module, its module tree is materialized in the
shared Kagami mirror at `/dev/kagami_mirror` by default. OverlayFS uses the
same mirror; configure it once with `mirror_dir`/`mirror_img` rather than a
separate Kasumi storage path.

## License

Kasumi is licensed `Apache-2.0 OR GPL-2.0`. The compiled `.ko` is kernel-linked
and therefore packaged with Kasumi's GPL-2.0 license text and notice alongside
the assets.
