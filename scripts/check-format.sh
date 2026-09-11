#!/usr/bin/env bash
set -euo pipefail
cd "$(dirname "$0")/.."
git ls-files -z -- '*.c' '*.cc' '*.cpp' '*.cxx' '*.h' '*.hh' '*.hpp' '*.hxx' \
    ':!:**/third_party/**' ':!:**/external/**' ':!:**/generated/**' |
    xargs -0 -r clang-format --dry-run --Werror
