#!/usr/bin/env bash
# The folder the Android app copies the game from (its first-run screen, SetupActivity):
# out/android/bbport-data/ with boot-linked.bin, content.bin and patches.bin as run.sh made them
# for this build (run bash run.sh once first), and app0/ for the game dump's contents. Copy the
# finished folder to the device (USB file transfer, a card) and choose it in the app.
#   tools/android_prepare.sh [GAME_DIR]   GAME_DIR: the dump (default BB_GAME_DIR or ../CUSA03173);
#   with it the dump is copied into app0/ too (about 30 GB).
set -euo pipefail
cd "$(dirname "$0")/.."
data=out/android/bbport-data
mkdir -p "$data/app0"
for f in boot-linked.bin content.bin patches.bin; do
    [[ -f out/$f ]] || { echo "out/$f is missing: run bash run.sh once first" >&2; exit 1; }
    cp out/$f "$data/"
done
game=${1:-${BB_GAME_DIR:-}}
if [[ -n $game ]]; then
    [[ -f $game/eboot.bin ]] || { echo "no eboot.bin in $game" >&2; exit 1; }
    echo "copying the game dump into $data/app0 ..."
    cp -r "$game"/. "$data/app0/"
fi
cat <<EOT
$data: boot-linked.bin, content.bin, patches.bin$( [[ -n $game ]] && echo ", app0/ (the game)" )
$( [[ -z $game ]] && echo "Put the game dump's contents (eboot.bin, sce_module/, sce_sys/, dvdroot_ps4/...) into $data/app0/." )
Copy the folder to the device and choose it in the app (first start).
EOT
