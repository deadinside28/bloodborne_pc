#!/bin/bash
# KDE Plasma 6 (KWin): focus the window whose title starts with "Bloodborne" and print the active
# window afterwards ("BBFOCUS active=..."). Loads focus_bb.js as a KWin script over D-Bus.
here=$(cd -- "$(dirname -- "$0")" && pwd)
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript bbfocus >/dev/null 2>&1
id=$(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$here/focus_bb.js" bbfocus)
since=$(date '+%Y-%m-%d %H:%M:%S')
qdbus6 org.kde.KWin "/Scripting/Script$id" org.kde.kwin.Script.run >/dev/null
sleep 0.5
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript bbfocus >/dev/null
journalctl --user --since "$since" -o cat 2>/dev/null | grep -o 'BBFOCUS.*' | tail -1
