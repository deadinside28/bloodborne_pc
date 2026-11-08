#!/usr/bin/env bash
# The recompiled game on an Android phone, CPU-only, over adb (no app, no root: /data/local/tmp):
# bb-probe built with the NDK with tools/a64_cpu_only_stubs.c (the GPU library, SDL3 and Vulkan
# as no-ops), pushed with the AArch64 game library (build_aot.sh, BB_AOT_TARGET=aarch64), the
# image run.sh prepared in out/, and the few game files the startup reads; run with --cpu-only.
# The game runs its startup until its first call into the graphics libraries (unresolved here).
#
#   tools/android_cpu_only.sh [LIBRARY]   (default recompiler/out/aot/sites10-aarch64/libbbaot.so)
# Env: ANDROID_SERIAL (adb device), BB_NDK (default: the newest under ~/.local/opt/android-sdk/ndk),
# BB_ADB, BB_GAME_DIR (as run.sh), BB_ANDROID_API (default 35).
set -euo pipefail
cd "$(dirname "$0")/.."
lib=$(readlink -f "${1:-recompiler/out/aot/sites10-aarch64/libbbaot.so}")
ndk=${BB_NDK:-$(ls -d "$HOME"/.local/opt/android-sdk/ndk/* | sort -V | tail -1)}
cc=$ndk/toolchains/llvm/prebuilt/linux-x86_64/bin/aarch64-linux-android${BB_ANDROID_API:-35}-clang
adb=(${BB_ADB:-$HOME/.local/opt/android-sdk/platform-tools/adb} ${ANDROID_SERIAL:+-s "$ANDROID_SERIAL"})
game=${BB_GAME_DIR:-../CUSA03173}
out=out/android
dir=/data/local/tmp/bbport
mkdir -p "$out/obj" "$out/include"
# SDL3's headers alone (the host's /usr/include must not mix with bionic's)
[[ -d $out/include/SDL3 ]] || cp -r /usr/include/SDL3 "$out/include/"
for source in third_party/LibAtrac9/C/src/*.c; do
    "$cc" -std=c99 -O2 -w -c "$source" -o "$out/obj/atrac9_$(basename "${source%.c}").o" &
done
wait
"$cc" -std=c11 -O2 -g -Wall -Wextra -Werror -pthread -I"$out/include" -I. -Isrc \
    src/probe.c src/runtime*.c src/vulkan_smoke.c recompiler/harness/recomp_hybrid.c tools/a64_cpu_only_stubs.c \
    "$out"/obj/atrac9_*.o -lm -ldl -Wl,--export-dynamic -o "$out/bb-probe"
echo "Built $out/bb-probe (Android, CPU-only)"
"${adb[@]}" shell "mkdir -p $dir/app0/dvdroot_ps4/sound $dir/user"
"${adb[@]}" push "$out/bb-probe" out/boot-linked.bin out/content.bin out/patches.bin "$dir/" >/dev/null
"${adb[@]}" push "$game/sce_sys" "$dir/app0/" >/dev/null
for f in sprj_mixer.fev sprj_multirpc.rpc sprj_multimix.mix sprj_multich.mch; do  # what the startup reads
    "${adb[@]}" push "$game/dvdroot_ps4/sound/$f" "$dir/app0/dvdroot_ps4/sound/" >/dev/null
done
"${adb[@]}" push "$lib" "$dir/libbbaot.so" >/dev/null
echo "== $("${adb[@]}" shell 'echo $(getprop ro.product.model) $(getprop ro.soc.model), Android $(getprop ro.build.version.release)')"
"${adb[@]}" shell "cd $dir && chmod 755 bb-probe && BB_AOT=$dir/libbbaot.so BB_AOT_ACCEPT_CHANGED=1 \
    ./bb-probe boot-linked.bin --content-profile content.bin --patches patches.bin --app0 $dir/app0 \
    --user $dir/user --timeout 120 --cpu-only"
