#!/bin/bash
# SPDX-License-Identifier: GPL-2.0-or-later
# Run beside a replay on the phone: RenderDoc's replay server is stopped when the phone's
# available memory drops below 1.5 GB or a reading fails (a replay of a large capture can take
# what the rest of the phone needs). watch.sh [adb serial] [log]
serial=${1:-${ANDROID_SERIAL:-}}
log=${2:-/dev/stderr}
adb=(timeout 10 adb ${serial:+-s "$serial"})
while true; do
    kb=$("${adb[@]}" shell 'grep MemAvailable /proc/meminfo' < /dev/null 2>/dev/null | awk '{print $2}')
    echo "$(date +%T) $kb" >> "$log"
    if ! [[ $kb =~ ^[0-9]+$ ]] || (( kb < 1572864 )); then
        "${adb[@]}" shell am force-stop org.renderdoc.renderdoccmd.arm64 < /dev/null
        echo "watch.sh: replay server stopped at ${kb:-no reading} kB available" >> "$log"
        exit 0
    fi
    sleep 2
done
