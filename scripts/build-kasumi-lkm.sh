#!/usr/bin/env bash
# Build a Kasumi API 17 LKM without writing generated files into the Kasumi
# checkout. The source is copied into Kagami's ignored build directory first.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
EXPECTED_REF="${KASUMI_REF:-main}"
EXPECTED_PROTOCOL=17
KASUMI_SOURCE="${KASUMI_DIR:-}"
KMI="${KASUMI_KMI:-}"
ARCH="arm64"
OUT_DIR="${KASUMI_OUT_DIR:-${PROJECT_ROOT}/module/kasumi}"
BUILD_ROOT="${KASUMI_BUILD_ROOT:-${PROJECT_ROOT}/build/kasumi-lkm}"
USE_DDK=0

die() {
    echo "error: $*" >&2
    exit 1
}

usage() {
    cat <<'EOF'
Usage: scripts/build-kasumi-lkm.sh --kmi <androidNN-x.y> [options]

Options:
  --source <dir>  Kasumi checkout (defaults: $KASUMI_DIR, ../Kasumi,
                  /Volumes/Workspace/Kasumi)
  --kmi <name>    KMI tag, e.g. android15-6.6 (required)
  --out <dir>     output directory (default: module/kasumi)
  --arch <arch>   target architecture (only arm64 is supported)
  --ddk           build through `ddk build <kmi>` instead of a local KDIR
  -h, --help      show this help

Required environment:
  KDIR            prepared Android kernel/DDK tree containing Makefile
                  (not needed with --ddk)

The Kasumi source must expose API 17 and match Kagami's checked-in UAPI. The
source checkout is never built in place; generated objects live under build/.
EOF
}

while [[ $# -gt 0 ]]; do
    case "$1" in
        --source)
            [[ $# -ge 2 ]] || die "--source requires a directory"
            KASUMI_SOURCE="$2"
            shift 2
            ;;
        --kmi)
            [[ $# -ge 2 ]] || die "--kmi requires a value"
            KMI="$2"
            shift 2
            ;;
        --out)
            [[ $# -ge 2 ]] || die "--out requires a directory"
            OUT_DIR="$2"
            shift 2
            ;;
        --arch)
            [[ $# -ge 2 ]] || die "--arch requires a value"
            ARCH="$2"
            shift 2
            ;;
        --ddk)
            USE_DDK=1
            shift
            ;;
        -h|--help)
            usage
            exit 0
            ;;
        *)
            die "unknown option: $1"
            ;;
    esac
done

[[ "$ARCH" == "arm64" ]] || die "Kasumi DDK integration currently supports only arm64"
[[ "$KMI" =~ ^[A-Za-z0-9._-]+$ ]] || die "--kmi must be a safe KMI tag"
if [[ "$USE_DDK" -eq 1 ]]; then
    command -v ddk >/dev/null 2>&1 || die "ddk is required with --ddk"
else
    [[ -n "${KDIR:-}" && -f "$KDIR/Makefile" ]] ||
        die "KDIR must point to a prepared kernel/DDK tree with a Makefile"
    command -v make >/dev/null 2>&1 || die "make is required"
fi

resolve_source() {
    local candidate
    local candidates=()
    [[ -n "$KASUMI_SOURCE" ]] && candidates+=("$KASUMI_SOURCE")
    candidates+=("${PROJECT_ROOT}/../Kasumi" "/Volumes/Workspace/Kasumi")
    for candidate in "${candidates[@]}"; do
        [[ -f "$candidate/src/Makefile" && -f "$candidate/src/include/kasumi_uapi.h" ]] || continue
        cd "$candidate" && pwd
        return 0
    done
    return 1
}

KASUMI_SOURCE="$(resolve_source)" ||
    die "Kasumi source not found; pass --source /path/to/Kasumi (checked out at ${EXPECTED_REF})"

if git -C "$KASUMI_SOURCE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    source_branch="$(git -C "$KASUMI_SOURCE" branch --show-current)"
    if [[ -n "$source_branch" && "$source_branch" != "$EXPECTED_REF" ]]; then
        die "Kasumi source is on '${source_branch}', expected '${EXPECTED_REF}'"
    fi
fi

grep -Eq "^[[:space:]]*#define[[:space:]]+KSM_PROTOCOL_VERSION[[:space:]]+${EXPECTED_PROTOCOL}$" \
    "$KASUMI_SOURCE/src/include/kasumi_uapi.h" ||
    die "Kasumi source is not API ${EXPECTED_PROTOCOL}; use ${EXPECTED_REF}"
cmp -s "${PROJECT_ROOT}/include/kagami/kasumi_uapi.h" \
    "$KASUMI_SOURCE/src/include/kasumi_uapi.h" ||
    die "Kagami UAPI differs from Kasumi ${EXPECTED_REF}; sync the header before building an LKM"

WORK_DIR="${BUILD_ROOT}/${KMI}-${ARCH}"
rm -rf "$WORK_DIR"
mkdir -p "$WORK_DIR" "$OUT_DIR"
cp -a "$KASUMI_SOURCE/." "$WORK_DIR/"
rm -rf "$WORK_DIR/.git" "$WORK_DIR/.claude"

if [[ -n "${KASUMI_VERSION:-}" ]]; then
    version="$KASUMI_VERSION"
elif git -C "$KASUMI_SOURCE" rev-parse --is-inside-work-tree >/dev/null 2>&1; then
    version="0.1.$(( $(git -C "$KASUMI_SOURCE" rev-list --count HEAD) + 200 ))"
else
    version="0.1.local"
fi

echo "Building Kasumi API ${EXPECTED_PROTOCOL} for ${KMI}/${ARCH} from ${KASUMI_SOURCE}"
if [[ "$USE_DDK" -eq 1 ]]; then
    # ddk mounts the current directory as /build and exports KDIR there. The
    # copied Kasumi root already contains its DDK-aware Makefile.
    (
        cd "$WORK_DIR"
        ddk build "$KMI" -- \
            "CC=${CC:-clang}" \
            "CFLAGS_MODULE=-DKASUMI_VERSION=\\\"${version}\\\""
    )
else
    make -C "$KDIR" -j"$(getconf _NPROCESSORS_ONLN 2>/dev/null || echo 4)" \
        ARCH="$ARCH" M="$WORK_DIR/src" CC="${CC:-clang}" \
        "CFLAGS_MODULE=-DKASUMI_VERSION=\\\"${version}\\\"" modules
fi

KO_SOURCE="$WORK_DIR/src/kasumi_lkm.ko"
[[ -s "$KO_SOURCE" ]] || die "Kasumi build completed without kasumi_lkm.ko"
KO_NAME="${KMI}_${ARCH}_kasumi_lkm.ko"
KO_DEST="${OUT_DIR}/${KO_NAME}"
cp "$KO_SOURCE" "$KO_DEST"
if command -v llvm-strip >/dev/null 2>&1; then
    llvm-strip -d "$KO_DEST" 2>/dev/null || true
fi

# Keep the kernel-use licensing notice beside every emitted LKM. The packaging
# workflow stages these files once with the matrix artifacts.
cp "$KASUMI_SOURCE/NOTICE" "$OUT_DIR/KASUMI-NOTICE"
cp "$KASUMI_SOURCE/LICENSE-GPL-2.0" "$OUT_DIR/KASUMI-LICENSE-GPL-2.0"

echo "Built ${KO_DEST}"
