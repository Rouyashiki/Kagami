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

The daemon requires an exact Kasumi API 19 match. It uses per-file atomic
transactions for complete rule and configuration updates. Both older and newer
versions are rejected before configuration; there is no legacy update path.
Operational ioctl commands are specific to the protocol version. Only version
discovery uses a shared command.

The DDK targets are `android12-5.10`, `android13-5.10`, `android13-5.15`,
`android14-5.15`, `android14-6.1`, `android15-6.6`, `android16-6.12` and
`android17-6.18`.

`kagamid lkm status` reports the detected KMI and selected asset. The module
loader deliberately keeps assets external to the binary, so Kagami can remain
a standalone metamodule and update compatible KMI builds independently.

The daemon first uses `finit_module`. On failure, its bundled
`lkmloader` helper patches unresolved ELF symbols from the built-in
kernel section of `/proc/kallsyms`, writes the patched image to a sealed memfd,
and calls `finit_module`. It falls back to `init_module` only when the fd-based
path is unavailable. The helper is independent of KernelSU userspace and
receives the same Kasumi ownership nonce as the direct load path.
An exact `ENOEXEC` vermagic mismatch from newly emitted kernel-log records
rebuilds `.modinfo` and is retried once; unrelated `ENOEXEC` failures are not
rewritten.

When Kasumi is selected for a module, its module tree is materialized in the
shared Kagami mirror at `/dev/kagami_mirror` by default. OverlayFS uses the
same mirror; configure it once with `mirror_dir`/`mirror_img` rather than a
separate Kasumi storage path.

## Persistent kstat rules

`kasumi_kstat_rules` stores the complete kstat table in the existing configuration
document. `kagamid config apply` and the module rebuild publish it atomically with
their other configuration fields. The embedded controller accepts the same
array through `ksud kagami config apply`. A missing key or an empty array clears
the kstat table on a successful full apply. A failed transaction preserves the
previous published table and the saved configuration.

```json
{
  "kasumi_kstat_rules": [
    {
      "fields": 9,
      "target_path": "/system/example",
      "spoofed_ino": "9007199254740993",
      "spoofed_size": "0"
    }
  ]
}
```

`fields` is a required uint32 JSON integer. Its bits select inode (1), device (2),
link count (4), size (8), atime (16), mtime (32), ctime (64), block size (128) and
blocks (256). Zero is a valid mask. Selected values may explicitly be zero.
Omitted numeric members default to zero. Unknown bits and unknown member names
are rejected.

Every 64-bit member uses an unsigned decimal JSON string, including
`target_ino`, `target_dev`, `spoofed_ino`, `spoofed_dev`, `spoofed_nlink`,
`spoofed_size`, `spoofed_atime_sec`, `spoofed_atime_nsec`, `spoofed_mtime_sec`,
`spoofed_mtime_nsec`, `spoofed_ctime_sec`, `spoofed_ctime_nsec`, `spoofed_blksize`
and `spoofed_blocks`. Integer parsing preserves all uint64 values exactly.
Device IDs, link counts and block sizes must also fit uint32. Selected timestamp
nanoseconds must be below 1,000,000,000.

A rule requires an absolute `target_path` or a nonzero `target_ino`. The optional
uint32 integer `match_flags` defaults to zero for exact device matching, including
device zero. Setting it to 1 selects `KSM_MATCH_ANY_DEVICE`. A resolvable pathname
captures its current inode and device according to the kernel UAPI. API 19 requires
`KSM_CAP_ATOMIC_KSTAT` and enforces its advertised count and byte budgets.

## License

Kasumi is licensed `Apache-2.0 OR GPL-2.0`. The compiled `.ko` is kernel-linked
and therefore packaged with Kasumi's GPL-2.0 license text and notice alongside
the assets.
