#!/usr/bin/env bash
# Compare both copies with Kagami's formatting so upstream's kernel style does
# not conflict with the native formatting check. Keep the full header check.
set -euo pipefail

PROJECT_ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
KAGAMI_UAPI="${PROJECT_ROOT}/include/kagami/kasumi_uapi.h"
EXPECTED_PROTOCOL=17

die() {
    echo "error: $*" >&2
    exit 1
}

[[ $# -eq 1 ]] || die "usage: scripts/check-kasumi-uapi.sh <Kasumi UAPI header>"
[[ -f "$1" ]] || die "Kasumi UAPI header not found: $1"
command -v clang-format >/dev/null 2>&1 || die "clang-format is required to check the Kasumi UAPI"

for header in "$KAGAMI_UAPI" "$1"; do
    grep -Eq "^[[:space:]]*#define[[:space:]]+KSM_PROTOCOL_VERSION[[:space:]]+${EXPECTED_PROTOCOL}[[:space:]]*$" \
        "$header" || die "${header} is not API ${EXPECTED_PROTOCOL}"
done

CHECK_DIR="$(mktemp -d)"
trap 'rm -f "$CHECK_DIR/kagami.h" "$CHECK_DIR/kasumi.h"; rmdir "$CHECK_DIR"' EXIT
clang-format --style=file --assume-filename="$KAGAMI_UAPI" < "$KAGAMI_UAPI" > "$CHECK_DIR/kagami.h"
clang-format --style=file --assume-filename="$KAGAMI_UAPI" < "$1" > "$CHECK_DIR/kasumi.h"
if ! diff -u --label "$KAGAMI_UAPI (formatted)" --label "$1 (formatted)" \
    "$CHECK_DIR/kagami.h" "$CHECK_DIR/kasumi.h"; then
    die "Kagami UAPI differs from Kasumi after formatting; sync the header before building an LKM"
fi
