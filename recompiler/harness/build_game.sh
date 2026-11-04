#!/usr/bin/env bash
# Lift the game executable in shards and compile every shard for a target; report time and size.
#   recompiler/harness/build_game.sh EBOOT [MODE] [TARGET]
#     MODE: abi (default) | default | abi-tso | default-tso
#     TARGET: aarch64 (default) | x86_64
# Env: BB_RECOMP_JOBS (parallel compiles, default 4: a shard takes 1-3 GiB in clang).
# The IR and objects are derived from your dump: they stay in recompiler/out/game, never commit them.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$here")
out=${BB_RECOMP_OUT:-$root/out}/game
py=${BB_RECOMP_PY:-$(cd "$root/.." && pwd)/.venv-recomp/bin/python}
eboot=$1; mode=${2:-abi}; target=${3:-aarch64}
jobs=${BB_RECOMP_JOBS:-4}
lf=(); [[ $mode == abi* ]] && lf+=(--abi); [[ $mode == *tso ]] && lf+=(--tso)
dir=$out/$mode
case $target in
    aarch64) cflags=(--target=aarch64-linux-gnu -march=armv8.4-a+rcpc) ;;
    x86_64) cflags=(--target=x86_64-linux-gnu -march=x86-64-v2) ;;
    *) echo "unknown target $target" >&2; exit 2 ;;
esac
mkdir -p "$dir"
t0=$(date +%s)
(cd "$root" && "$py" -W ignore -m recomp game "$eboot" "${lf[@]}" --out "$dir" --report "$dir/survey.json") | tee "$dir/survey.txt" | head -6
t1=$(date +%s)
echo "lifted in $((t1 - t0)) s; compiling $(ls "$dir"/part-*.ll | wc -l) shards + table for $target with $jobs jobs"
objdir=$dir/obj-$target
mkdir -p "$objdir"
# one shard: object, and "<seconds> <peak KiB>" in <object>.time (os.wait4: no external `time`)
compile_once() {
    ll=$1; o=$2; shift 2
    python3 - "$o" clang -O2 -c -Wno-override-module "$@" "$ll" -o "$o" <<'PY'
import os, subprocess, sys, time
o, cmd = sys.argv[1], sys.argv[2:]
t = time.time()
with open(o + ".err", "w") as err:
    p = subprocess.Popen(cmd, stderr=err)
    _, status, ru = os.wait4(p.pid, 0)
open(o + ".time", "w").write(f"{time.time() - t:.1f} {ru.ru_maxrss}\n")
sys.exit(os.waitstatus_to_exitcode(status))
PY
}
compile() {
    ll=$1; o=$2/$(basename "$ll" .ll).o; shift 2
    compile_once "$ll" "$o" "$@" && return
    # LLVM 23.1's SelectionDAG crashes on a store it merges itself
    # (harness/llvm-bug-store-merge-bswap16.ll): retry with GlobalISel, which falls back to
    # SelectionDAG only where it cannot select
    if grep -q 'Cannot select' "$o.err" && compile_once "$ll" "$o" "$@" -fglobal-isel -mllvm -global-isel-abort=0; then
        : > "$o.err"
        echo "$(basename "$ll"): compiled with GlobalISel (LLVM SelectionDAG bug)"
    else
        echo "FAILED $(basename "$ll")"
    fi
}
export -f compile compile_once
ls "$dir"/part-*.ll "$dir"/table.ll | xargs -P "$jobs" -I{} bash -c 'compile "$@"' _ {} "$objdir" "${cflags[@]}"
t2=$(date +%s)
failed=$(grep -l . "$objdir"/*.err 2>/dev/null | wc -l)
python3 - "$objdir" "$((t2 - t1))" "$failed" <<'EOF'
import glob, os, sys
d, wall, failed = sys.argv[1], int(sys.argv[2]), int(sys.argv[3])
objs = glob.glob(os.path.join(d, "*.o"))
times = []
for t in glob.glob(os.path.join(d, "*.o.time")):
    s = open(t).read().split()
    if s:
        times.append((float(s[0]), int(s[1]) if len(s) > 1 else 0))
size = sum(os.path.getsize(o) for o in objs)
cpu = sum(t for t, _ in times)
peak = max((m for _, m in times), default=0)
print(f"compiled {len(objs)} objects in {wall} s wall ({cpu:.0f} s CPU); {failed} with errors or warnings")
import subprocess
text = 0
for k in range(0, len(objs), 50):
    r = subprocess.run(["llvm-size", *objs[k:k + 50]], capture_output=True, text=True).stdout.splitlines()[1:]
    text += sum(int(l.split()[0]) for l in r if l.split() and l.split()[0].isdigit())
print(f"machine code (.text) {text / 2**20:.0f} MiB; the original game's code ranges are 39.8 MiB ({text / 41719719:.1f}x)")
print(f"objects total {size / 2**20:.0f} MiB; slowest shard {max((t for t, _ in times), default=0):.0f} s; peak clang memory {peak / 2**20:.1f} GiB")
EOF
