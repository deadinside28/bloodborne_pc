#!/usr/bin/env bash
# The recompiled game on AArch64 without an AArch64 machine: bb-probe built for aarch64-linux-gnu
# in a CPU-only form (the GPU library, SDL3 and Vulkan replaced by tools/a64_cpu_only_stubs.c),
# run under qemu-aarch64 with the AArch64 game library (build_aot.sh, BB_AOT_TARGET=aarch64).
# The game runs its startup until its first call into the graphics libraries (unresolved here).
#
#   tools/a64_cpu_only.sh SYSROOT [LIBRARY] [extra bb-probe arguments]
#     SYSROOT: an AArch64 glibc (Fedora's glibc and libgcc aarch64 RPMs extracted, with
#              lib/ld-linux-aarch64.so.1 -> ../usr/lib/ld-linux-aarch64.so.1)
#     LIBRARY: default recompiler/out/aot/sites10-aarch64/libbbaot.so
# Env: BB_ZIG, BB_QEMU, BB_GAME_DIR (as run.sh); the files run.sh prepared in out/ (boot-linked.bin,
# content.bin, patches.bin) are used as they are.
set -euo pipefail
cd "$(dirname "$0")/.."
sysroot=$(readlink -f "$1"); shift
lib=$(readlink -f "${1:-recompiler/out/aot/sites10-aarch64/libbbaot.so}"); [[ $# -gt 0 ]] && shift
zig=${BB_ZIG:-$HOME/.local/opt/zig/zig}
qemu=${BB_QEMU:-$HOME/.local/opt/qemu/usr/bin/qemu-aarch64-static}
game=${BB_GAME_DIR:-../CUSA03173}
out=out/a64
mkdir -p "$out/obj"
cc=("$zig" cc -target aarch64-linux-gnu.2.38)
for source in third_party/LibAtrac9/C/src/*.c; do
    "${cc[@]}" -std=c99 -O2 -w -c "$source" -o "$out/obj/atrac9_$(basename "${source%.c}").o" &
done
wait
read -r -a includes <<< "$(pkg-config --cflags vulkan sdl3)"
"${cc[@]}" -std=c11 -O2 -g -Wall -Wextra -pthread "${includes[@]}" -I. -Isrc -idirafter /usr/include \
    src/probe.c src/runtime*.c src/vulkan_smoke.c recompiler/harness/recomp_hybrid.c tools/a64_cpu_only_stubs.c \
    "$out"/obj/atrac9_*.o -lm -ldl -rdynamic -o "$out/bb-probe"
echo "Built $out/bb-probe (aarch64, CPU-only)"
BB_AOT="$lib" exec "$qemu" -L "$sysroot" "$out/bb-probe" out/boot-linked.bin --content-profile out/content.bin \
    --patches out/patches.bin --app0 "$game" --user "${BB_USER_DIR:-user}" --timeout "${BB_TIMEOUT:-0}" --cpu-only "$@"
