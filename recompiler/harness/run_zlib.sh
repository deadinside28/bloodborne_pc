#!/usr/bin/env bash
# Whole-library differential test on the system's libz (FDE function discovery, as for the game).
#   recompiler/harness/run_zlib.sh [LIBZ.so]
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$here")
out=${BB_RECOMP_OUT:-$root/out}
py=${BB_RECOMP_PY:-$(cd "$root/.." && pwd)/.venv-recomp/bin/python}
default=/usr/lib/libz.so.1
[[ -e $default ]] || default=/usr/lib64/libz.so.1 # Fedora
lib=$(readlink -f "${1:-$default}")
mkdir -p "$out"
# AArch64 replay under QEMU (skipped when the tools are missing): BB_ZIG, BB_QEMU
zig=${BB_ZIG:-$HOME/.local/opt/zig/zig}
qemu=${BB_QEMU:-$HOME/.local/opt/qemu/usr/bin/qemu-aarch64-static}
arm=1
if [[ ! -x $zig || ! -x $qemu ]]; then arm=0; echo "AArch64 replay skipped (need zig and qemu-aarch64-static; BB_ZIG / BB_QEMU)"; fi
arm_cc() { "$zig" cc -target aarch64-linux-musl -O1 -static -Wno-deprecated-non-prototype "$@" 2>&1 \
    | grep -vE 'dlstart|GETFUNCSYM|rcrt1|conflicting prototype|__dls2|warning generated|^ *\^|^ +[0-9]+ \|' || true; }
# zlib.h/zconf.h only (for z_stream), without the rest of the host's /usr/include; zlib-ng's
# compatible zconf.h (Fedora) includes its name mangling header too
mkdir -p "$out/zinc" && cp /usr/include/zlib.h /usr/include/zconf.h "$out/zinc/"
for extra in /usr/include/zlib_name_mangling*.h; do [[ -e $extra ]] && cp "$extra" "$out/zinc/"; done
# zlib-ng (Fedora's libz) picks its code by the CPU: the baseline SSE2 one (recomp_loader.c)
export RECOMP_CPUID=sse2
status=0
for mode in ${BB_RECOMP_MODES:-default abi}; do
    lf=(--fde); [[ $mode == abi ]] && lf+=(--abi)
    (cd "$root" && "$py" -W ignore -m recomp lift "$lib" "${lf[@]}" -o "$out/libz-$mode.ll") 2> "$out/libz-$mode.lift.log"
    grep -E '^(unsupported|cfg)' "$out/libz-$mode.lift.log" | head -10 || true
    clang -O2 -march=x86-64-v2 -c -Wno-override-module "$out/libz-$mode.ll" -o "$out/libz-$mode.o"
    cc -O1 -g -I"$here" -o "$out/zlib_test-$mode" "$here/zlib_test.c" "$here/recomp_loader.c" "$out/libz-$mode.o"
    echo "== libz, $mode lifting"
    "$out/zlib_test-$mode" "$lib" -q 2> "$out/zlib_test-$mode.log" | tee "$out/zlib_test-$mode.out" || status=1
    want=$(sed -n 's/^digest: \([0-9a-f]*\).*/\1/p' "$out/zlib_test-$mode.out")
    if (( arm )); then
        (cd "$root" && "$py" -W ignore -m recomp lift "$lib" "${lf[@]}" --tso -o "$out/libz-$mode.tso.ll") 2>/dev/null
        for ir in libz-$mode libz-$mode.tso; do
            clang -O2 --target=aarch64-linux-gnu -march=armv8.4-a+rcpc -c -Wno-override-module "$out/$ir.ll" -o "$out/$ir.aarch64.o"
            arm_cc -I"$here" -I"$out/zinc" -o "$out/$ir.zlib_test.a64" "$here/zlib_test.c" "$here/recomp_loader.c" "$out/$ir.aarch64.o"
            got=$("$qemu" "$out/$ir.zlib_test.a64" "$lib" -q -d 2> "$out/$ir.a64.log" | tee "$out/$ir.a64.out" | sed -n 's/^digest: \([0-9a-f]*\).*/\1/p' || true)
            checks=$(grep -o '[0-9]* checks total, [0-9]* failed' "$out/$ir.a64.out" || echo "no result")
            if [[ -n $got && $got == "$want" ]]; then echo "  aarch64 ${ir#libz-}: digest $got matches x86-64 ($checks)"
            else echo "  aarch64 ${ir#libz-}: digest '${got:-none}' != x86-64 '$want' ($checks)"; status=1; tail -3 "$out/$ir.a64.log"; fi
        done
    fi
done
exit $status
