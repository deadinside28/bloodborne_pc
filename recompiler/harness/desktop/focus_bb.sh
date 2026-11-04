#!/bin/bash
# KDE Plasma 6 (KWin): focus the window whose title starts with "Bloodborne" and print the active
# window afterwards ("BBFOCUS active=..."). Loads focus_bb.js as a KWin script over D-Bus.
# qdbus6 or qdbus (distributions name Qt 6's either way); inside a distrobox without either, the
# script runs itself on the host (host-spawn), where KWin's journal is too.
here=$(cd -- "$(dirname -- "$0")" && pwd)
q=$(command -v qdbus6 || command -v qdbus)
if [[ -z $q ]]; then
    if command -v host-spawn >/dev/null && [[ -z ${BB_FOCUS_ON_HOST:-} ]]; then
        exec host-spawn env BB_FOCUS_ON_HOST=1 bash "$here/focus_bb.sh"
    fi
    echo "focus_bb.sh: no qdbus6/qdbus" >&2
    exit 1
fi
"$q" org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript bbfocus >/dev/null 2>&1
id=$("$q" org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$here/focus_bb.js" bbfocus)
since=$(date '+%Y-%m-%d %H:%M:%S')
"$q" org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null
sleep 0.5
"$q" org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript bbfocus >/dev/null
journalctl --user --since "$since" -o cat 2>/dev/null | grep -o 'BBFOCUS.*' | tail -1
