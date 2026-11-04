#!/usr/bin/env bash
# A hybrid execution library for bbport (BB_AOT): the listed game functions recompiled for x86-64.
#   recompiler/harness/build_aot.sh EBOOT LIST LIVE [NAME]
#     LIST: link addresses, one per line ('#' comments); LIVE: bbport's code as it runs
#     (BB_AOT_DUMP=out/aot/live.bin bash run.sh, closed once it is written), or several dumps
#     separated by commas, one per configuration the library must run in: settings install hooks
#     of their own (BB_FRAME_STATS: the GPU range collector, 0x26aa860), and bbport refuses a
#     library when any of its functions differs from the code it was lifted from.
#   -> recompiler/out/aot/NAME/libbbaot.so; run with BB_AOT=<that path> bash run.sh
# Env: BB_RECOMP_MODE=default (whole register file at calls, the default) | abi; BB_RECOMP_VOLATILE=0.
# Derived from your game dump: it stays under recompiler/out, never commit it.
set -euo pipefail
here=$(cd -- "$(dirname -- "$0")" && pwd)
root=$(dirname "$here")
py=${BB_RECOMP_PY:-$(cd "$root/.." && pwd)/.venv-recomp/bin/python}
eboot=$(readlink -f "$1"); list=$(readlink -f "$2"); name=${4:-$(basename "$2" .txt)}
live=(); IFS=, read -r -a dumps <<< "$3"
for d in "${dumps[@]}"; do live+=(--live "$(readlink -f "$d")"); done
dir=${BB_RECOMP_OUT:-$root/out}/aot/$name
lf=(); [[ ${BB_RECOMP_MODE:-default} == abi ]] && lf=(--abi)
# Guest memory accesses are volatile: the x86 hardware keeps their order, and LLVM can no longer
# drop or hoist them (a loop waiting on a flag another thread sets must keep reading it; without
# this the game hung at its first frames with the 200 hottest functions). BB_RECOMP_VOLATILE=0: off.
[[ ${BB_RECOMP_VOLATILE:-1} == 1 ]] && lf+=(--volatile)
# The game's system modules (libc.prx, libSceFios2.prx) as run.sh links them (out/link.json):
# their functions can be listed too. BB_AOT_LINK=none: the executable alone.
link=${BB_AOT_LINK:-$(cd "$root/.." && pwd)/out/link.json}
[[ $link != none && -f $link ]] && lf+=(--link "$link")
rm -rf "$dir"; mkdir -p "$dir"
# Functions that stay original whatever the list says: bbport's page-write tracking recognises
# them by the faulting instruction's address (gpu/games/bloodborne.cpp DynamicWriters).
keep_original='^0x(2ab7350|20858a0)$'
grep -v '^\s*#' "$list" | awk '{print $1}' | grep -v '^$' | grep -viE "$keep_original" > "$dir/list.txt" || true
(cd "$root" && "$py" -W ignore -m recomp game "$eboot" "${lf[@]}" --only "$dir/list.txt" "${live[@]}" \
    --out "$dir" --report "$dir/survey.json") > "$dir/survey.txt"
grep -E '^(hybrid|  left original|  lifted, entry)' "$dir/survey.txt" || true
# BB_RECOMP_JOBS: parallel compiles (default: the cores, at most 8; a shard of the whole game
# takes 1-3 GiB in clang).
jobs=${BB_RECOMP_JOBS:-$(( $(nproc) < 8 ? $(nproc) : 8 ))}
# The IR goes once compiled and the objects once linked (the whole game: ~10 GB of IR, ~3 GB of
# objects); BB_AOT_KEEP_IR=1 keeps both.
keep=${BB_AOT_KEEP_IR:-0}
ls "$dir"/part-*.ll "$dir"/table.ll | xargs -P "$jobs" -I{} sh -c \
    'clang -O2 -fPIC --target=x86_64-linux-gnu -march=x86-64-v2 -c -Wno-override-module "$1" -o "${1%.ll}.o" && { [ "$2" = 1 ] || rm -f "$1"; }' _ {} "$keep"
clang -shared -o "$dir/libbbaot.so" "$dir"/*.o
[[ $keep == 1 ]] || rm -f "$dir"/*.o
echo "$dir/libbbaot.so: $(( $(stat -c %s "$dir/libbbaot.so") / 1024 )) KiB"
