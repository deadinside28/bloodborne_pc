#!/usr/bin/env bash
# Differential tests: build the corpus with each compiler/flag set (for the PS4's CPU, Jaguar),
# recompile it, compile the IR for x86-64 and AArch64, and compare against the original code.
#   recompiler/harness/run.sh [FILTER]
# Env: BB_RECOMP_VARIANTS="clang-O2 gcc-O3 ...", BB_RECOMP_OUT (default recompiler/out).
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$here")
out=${BB_RECOMP_OUT:-$root/out}
py=${BB_RECOMP_PY:-$(cd "$root/.." && pwd)/.venv-recomp/bin/python}
mkdir -p "$out"
cflags="-fno-math-errno -fPIC -shared -nostdlib -ffreestanding -fno-stack-protector -fcf-protection=none"
# variant: compiler-opt[:march]; march defaults to btver2 (Jaguar, the PS4 CPU: AVX, VEX encodings).
# x86-64 / x86-64-v2 variants exercise the legacy SSE encodings.
variants=${BB_RECOMP_VARIANTS:-"clang-O1 clang-O2 clang-O3 gcc-O1 gcc-O2 gcc-O3 clang-O2:x86-64 gcc-O3:x86-64 clang-O3:x86-64-v2"}
status=0
for v in $variants; do
    march=btver2; [[ $v == *:* ]] && march=${v#*:}
    base=${v%%:*}; cc=${base%-*}; opt=-${base#*-}
    v=${v/:/-}
    extra=()
    if [[ $cc == gcc ]]; then extra=(-fno-tree-loop-distribute-patterns); fi
    so=$out/corpus-$v.so
    $cc $opt -march=$march $cflags "${extra[@]}" -o "$so" "$here"/corpus/*.c "$here"/corpus/*.S
    if readelf -rW "$so" | grep -q JUMP_SLOT; then echo "[$v] corpus has PLT calls (libgcc?)"; status=1; continue; fi
    for mode in ${BB_RECOMP_MODES:-default abi}; do
        lf=(); [[ $mode == abi ]] && lf=(--abi)
        t=$v-$mode
        (cd "$root" && "$py" -W ignore -m recomp lift "$so" "${lf[@]}" -o "$out/corpus-$t.ll") 2> "$out/corpus-$t.lift.log"
        (cd "$root" && "$py" -W ignore -m recomp lift "$so" "${lf[@]}" --tso -o "$out/corpus-$t.tso.ll") 2>/dev/null
        grep -E '^(unsupported|cfg)' "$out/corpus-$t.lift.log" | sed "s/^/[$t] /" | head -20 || true
        clang -O2 -march=x86-64-v2 -c -Wno-override-module "$out/corpus-$t.ll" -o "$out/lifted-$t.o"
        for ir in corpus-$t corpus-$t.tso; do
            clang -O2 --target=aarch64-linux-gnu -march=armv8.2-a+rcpc -c -Wno-override-module "$out/$ir.ll" -o "$out/$ir.aarch64.o"
        done
        cc -O1 -g -I"$here" -o "$out/runner-$t" "$here/runner.c" "$here/recomp_loader.c" "$out/lifted-$t.o"
        printf '%-18s %6s KiB aarch64  ' "$t" $(( $(stat -c %s "$out/corpus-$t.aarch64.o") / 1024 ))
        "$out/runner-$t" "$so" -r "$out/expected-$v.bin" ${1:+-f "$1"} 2>"$out/runner-$t.log" | tail -1 || {
            status=1; grep -E '^(FAIL|    )' <("$out/runner-$t" "$so" ${1:+-f "$1"} 2>/dev/null) | head -20; }
    done
done
exit $status
