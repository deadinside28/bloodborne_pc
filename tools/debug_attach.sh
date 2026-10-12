#!/usr/bin/env bash
# Attach gdb to a bb-probe that stopped with BB_FAULT_WAIT=1 (a fault or a GPU-library
# assertion) and save the state: every thread's stack, the faulting thread's registers and
# code, locals of the asserting thread. Usage: tools/debug_attach.sh [pid] [--keep]
# Without a pid the oldest running bb-probe is taken. The process is killed afterwards;
# --keep detaches instead so gdb can be attached again (gdb -p <pid>).
# The report goes to logs/crash-<time>-<pid>.txt.
set -euo pipefail
cd "$(dirname "$0")/.."
keep=0 pid=
for arg in "$@"; do
    case $arg in --keep) keep=1 ;; *) pid=$arg ;; esac
done
[[ -n $pid ]] || pid=$(pgrep -o -x bb-probe || true)
[[ -n $pid ]] || { echo "debug_attach: no bb-probe is running" >&2; exit 1; }
mkdir -p logs
out=logs/crash-$(date +%Y%m%d_%H%M%S)-$pid.txt
BB_DEBUG_OUT=$out BB_DEBUG_KEEP=$keep gdb -batch -q -iex "set debuginfod enabled on" -p "$pid" -x tools/debug_attach.py 2>&1 \
    | grep -vE '^\[(New|Thread debugging|Inferior)' || true
echo "debug_attach: report in $out"
