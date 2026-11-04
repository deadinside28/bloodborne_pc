#!/usr/bin/env bash
# Whole-library differential test on the system's libz (FDE function discovery, as for the game).
#   recompiler/harness/run_zlib.sh [LIBZ.so]
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$here")
out=${BB_RECOMP_OUT:-$root/out}
py=${BB_RECOMP_PY:-$(cd "$root/.." && pwd)/.venv-recomp/bin/python}
lib=$(readlink -f "${1:-/usr/lib/libz.so.1}")
mkdir -p "$out"
status=0
for mode in ${BB_RECOMP_MODES:-default abi}; do
    lf=(--fde); [[ $mode == abi ]] && lf+=(--abi)
    (cd "$root" && "$py" -W ignore -m recomp lift "$lib" "${lf[@]}" -o "$out/libz-$mode.ll") 2> "$out/libz-$mode.lift.log"
    grep -E '^(unsupported|cfg)' "$out/libz-$mode.lift.log" | head -10 || true
    clang -O2 -march=x86-64-v2 -c -Wno-override-module "$out/libz-$mode.ll" -o "$out/libz-$mode.o"
    cc -O1 -g -I"$here" -o "$out/zlib_test-$mode" "$here/zlib_test.c" "$here/recomp_loader.c" "$out/libz-$mode.o"
    echo "== libz, $mode lifting"
    "$out/zlib_test-$mode" "$lib" -q 2> "$out/zlib_test-$mode.log" || status=1
done
exit $status
