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
# AArch64 replay under QEMU (skipped when the tools are missing): BB_ZIG, BB_QEMU
zig=${BB_ZIG:-$HOME/.local/opt/zig/zig}
qemu=${BB_QEMU:-$HOME/.local/opt/qemu/usr/bin/qemu-aarch64-static}
arm=1
if [[ ! -x $zig || ! -x $qemu ]]; then arm=0; echo "AArch64 replay skipped (need zig and qemu-aarch64-static; BB_ZIG / BB_QEMU)"; fi
arm_cc() { "$zig" cc -target aarch64-linux-musl -O1 -static -Wno-deprecated-non-prototype "$@" 2>&1 \
    | grep -vE 'dlstart|GETFUNCSYM|rcrt1|conflicting prototype|__dls2|warning generated|^ *\^|^ +[0-9]+ \|' || true; }
# Hybrid execution (original and recompiled code in one process): x86-64 hosts; BB_RECOMP_HYBRID=0 skips.
hybrid=0
if [[ $(uname -m) == x86_64 && ${BB_RECOMP_HYBRID:-1} != 0 ]]; then
    hybrid=1
    cc -c "$here/recomp_hybrid_x86.S" -o "$out/hybrid_x86.o"
fi
status=0
# The x87 emulator (recomp_x87_emu.h, the AArch64 hosts' x87) against the hardware: every form.
if [[ $(uname -m) == x86_64 && -z ${1:-} ]]; then
    cc -O2 -o "$out/x87_fuzz" "$here/x87_fuzz.c"
    printf '%-18s ' "x87 emulator"
    "$out/x87_fuzz" ${BB_X87_FUZZ:-2000} | tail -1 || { status=1; grep '^FAIL' <("$out/x87_fuzz" 200) | head -10; }
fi
for v in $variants; do
    march=btver2; [[ $v == *:* ]] && march=${v#*:}
    base=${v%%:*}; cc=${base%-*}; opt=-${base#*-}
    v=${v/:/-}
    extra=()
    # -Wno-psabi: calls.c passes 32-byte vectors, which the builds without AVX pass in memory.
    if [[ $cc == gcc ]]; then extra=(-fno-tree-loop-distribute-patterns -Wno-psabi); fi
    so=$out/corpus-$v.so
    $cc $opt -march=$march $cflags "${extra[@]}" -o "$so" "$here"/corpus/*.c "$here"/corpus/*.S
    if readelf -rW "$so" | grep -q JUMP_SLOT; then echo "[$v] corpus has PLT calls (libgcc?)"; status=1; continue; fi
    # corpus/sites.S's guest sites: gsite_add_* (3 bytes), gsite_vec_* and gsite_ret_* (4 bytes), gsite_jmp_* (2)
    nm -D --defined-only "$so" | awk '$3 ~ /^gsite_add_/ {print "0x"$1, 3, "000000", $3} $3 ~ /^gsite_vec_/ {print "0x"$1, 4, "00000000", $3} $3 ~ /^gsite_ret_/ {print "0x"$1, 4, "00000000", $3} $3 ~ /^gsite_jmp_/ {print "0x"$1, 2, "0000", $3}' \
        > "$out/sites-$v.txt"
    st=(--sites "$out/sites-$v.txt")
    for mode in ${BB_RECOMP_MODES:-default abi}; do
        lf=(); [[ $mode == abi ]] && lf=(--abi)
        t=$v-$mode
        (cd "$root" && "$py" -W ignore -m recomp lift "$so" "${lf[@]}" "${st[@]}" -o "$out/corpus-$t.ll") 2> "$out/corpus-$t.lift.log"
        (cd "$root" && "$py" -W ignore -m recomp lift "$so" "${lf[@]}" "${st[@]}" --tso -o "$out/corpus-$t.tso.ll") 2>/dev/null
        grep -E '^(unsupported|cfg)' "$out/corpus-$t.lift.log" | sed "s/^/[$t] /" | head -20 || true
        clang -O2 -march=x86-64-v2 -c -Wno-override-module "$out/corpus-$t.ll" -o "$out/lifted-$t.o"
        for ir in corpus-$t corpus-$t.tso; do
            clang -O2 --target=aarch64-linux-gnu -march=armv8.4-a+rcpc -c -Wno-override-module "$out/$ir.ll" -o "$out/$ir.aarch64.o"
        done
        cc -O1 -g -I"$here" -o "$out/runner-$t" "$here/runner.c" "$here/recomp_loader.c" "$out/lifted-$t.o"
        printf '%-18s %6s KiB aarch64  ' "$t" $(( $(stat -c %s "$out/corpus-$t.aarch64.o") / 1024 ))
        "$out/runner-$t" "$so" -r "$out/expected-$v.bin" ${1:+-f "$1"} 2>"$out/runner-$t.log" | tail -1 || {
            status=1; grep -E '^(FAIL|    )' <("$out/runner-$t" "$so" ${1:+-f "$1"} 2>/dev/null) | head -20; }
        if [[ $(uname -m) == x86_64 ]]; then  # x87 through the emulator, as on AArch64
            cc -O1 -g -DRECOMP_X87_EMU -I"$here" -o "$out/runner-$t-x87emu" "$here/runner.c" "$here/recomp_loader.c" "$out/lifted-$t.o"
            printf '  %-28s ' "x87 emulated"
            "$out/runner-$t-x87emu" "$so" -c "$out/expected-$v.bin" ${1:+-f "$1"} 2>"$out/runner-$t-x87emu.log" | tail -1 || {
                status=1; grep -E '^(FAIL|    )' <("$out/runner-$t-x87emu" "$so" -c "$out/expected-$v.bin" ${1:+-f "$1"} 2>/dev/null) | head -10; }
        fi
        if (( hybrid )); then
            # Hybrid execution: a subset recompiled (all, either half, a third), entered from the
            # original code through entry patches and calling back into it.
            for sel in 1:0 2:0 2:1 3:1; do
                h=$t-hybrid-${sel/:/_}
                (cd "$root" && "$py" -W ignore -m recomp lift "$so" "${lf[@]}" "${st[@]}" --select "$sel" -o "$out/corpus-$h.ll") 2>/dev/null
                clang -O2 -march=x86-64-v2 -c -Wno-override-module "$out/corpus-$h.ll" -o "$out/lifted-$h.o"
                cc -O1 -g -DRECOMP_HYBRID -I"$here" -o "$out/runner-$h" "$here/runner.c" "$here/recomp_loader.c" \
                    "$here/recomp_hybrid.c" "$out/hybrid_x86.o" "$out/lifted-$h.o"
                printf '  %-28s ' "hybrid ${sel}"
                "$out/runner-$h" "$so" -H -c "$out/expected-$v.bin" ${1:+-f "$1"} 2>"$out/runner-$h.log" | tail -1 || {
                    status=1; { "$out/runner-$h" "$so" -H -c "$out/expected-$v.bin" ${1:+-f "$1"} 2>&1 || true; } \
                        | grep -E '^(FAIL|    )|patch|ud2|fault' | head -20; }
            done
        fi
        if (( arm )); then
            for ir in corpus-$t corpus-$t.tso; do
                arm_cc -I"$here" -o "$out/$ir.runner.a64" "$here/runner.c" "$here/recomp_loader.c" "$out/$ir.aarch64.o"
                printf '  %-28s ' "aarch64 ${ir#corpus-}"
                "$qemu" "$out/$ir.runner.a64" "$so" -c "$out/expected-$v.bin" ${1:+-f "$1"} 2>"$out/$ir.a64.log" | tail -1 || {
                    status=1; grep -E '^(FAIL|    )' <("$qemu" "$out/$ir.runner.a64" "$so" -c "$out/expected-$v.bin" ${1:+-f "$1"} 2>/dev/null) | head -10; }
            done
        fi
    done
done
exit $status
