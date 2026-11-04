#!/usr/bin/env bash
# Run the AArch64 test binaries built by harness/run.sh and harness/run_zlib.sh on a real device
# over adb (no app, no root: /data/local/tmp). Run those two scripts first; this one only pushes
# and runs what they left in out/.
#   recompiler/harness/device/run_device.sh [FILTER]
# Env: ANDROID_SERIAL (adb device), BB_RECOMP_OUT.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$(dirname "$here")")
out=${BB_RECOMP_OUT:-$root/out}
dir=/data/local/tmp/bbrecomp
adb=(adb ${ANDROID_SERIAL:+-s "$ANDROID_SERIAL"})
"${adb[@]}" shell "mkdir -p $dir" >/dev/null
push() { "${adb[@]}" push "$@" >/dev/null 2>&1 || { echo "adb push $1 failed"; exit 2; }; }
status=0

echo "== device: $("${adb[@]}" shell 'echo $(getprop ro.product.model) $(getprop ro.soc.model), Android $(getprop ro.build.version.release)')"
if [[ -f $out/va_probe.a64 ]]; then
    push "$out/va_probe.a64" "$dir/va_probe"
    "${adb[@]}" shell "chmod 755 $dir/va_probe && $dir/va_probe | sed -n 2p"
fi

# corpus: replay the original code's recorded results
for so in "$out"/corpus-*.so; do
    v=$(basename "$so" .so); v=${v#corpus-}
    [[ -f $out/expected-$v.bin ]] || continue
    push "$so" "$dir/corpus-$v.so"
    push "$out/expected-$v.bin" "$dir/expected-$v.bin"
    for r in "$out"/corpus-"$v"-{default,abi}{,.tso}.runner.a64; do
        [[ -f $r ]] || continue
        name=$(basename "$r" .runner.a64); name=${name#corpus-}
        push "$r" "$dir/runner"
        printf '%-34s ' "$name"
        res=$("${adb[@]}" shell "chmod 755 $dir/runner && $dir/runner $dir/corpus-$v.so -c $dir/expected-$v.bin ${1:+-f $1} 2>/dev/null | tail -1")
        echo "$res"
        [[ $res == *" 0 failed, 0 not recompiled"* ]] || status=1
    done
    "${adb[@]}" shell "rm -f $dir/expected-$v.bin $dir/corpus-$v.so"
done

# libz: the digest must equal the x86-64 run's (where it was compared to the original library)
lib=$(readlink -f /usr/lib/libz.so.1)
push "$lib" "$dir/libz.so"
for mode in default abi; do
    want=$(sed -n 's/^digest: \([0-9a-f]*\).*/\1/p' "$out/zlib_test-$mode.out" 2>/dev/null || true)
    [[ -n $want ]] || continue
    for ir in libz-$mode libz-$mode.tso; do
        [[ -f $out/$ir.zlib_test.a64 ]] || continue
        push "$out/$ir.zlib_test.a64" "$dir/zlib_test"
        t0=$(date +%s.%N)
        res=$("${adb[@]}" shell "chmod 755 $dir/zlib_test && $dir/zlib_test $dir/libz.so -q -d 2>&1")
        t1=$(date +%s.%N)
        got=$(sed -n 's/^digest: \([0-9a-f]*\).*/\1/p' <<< "$res")
        secs=$(python3 -c "print(f'{$t1 - $t0:.1f}')")
        if [[ $got == "$want" ]]; then echo "libz ${ir#libz-}: digest $got matches x86-64 (${secs}s)"
        else echo "libz ${ir#libz-}: digest '${got:-none}' != x86-64 '$want'"; tail -3 <<< "$res"; status=1; fi
    done
done
exit $status
