# SPDX-License-Identifier: GPL-2.0-or-later
"""Device and gamepad detection and formatting helpers for BBPort launcher."""

import subprocess
from bbport_config import PACKAGED, PORT_DIR
from bbport_i18n import tr


def format_mouse_btn(val):
    """Formats raw mouse button binding identifiers into readable localized names."""
    if not val:
        return "—"
    names = {
        "left": tr("Левая кнопка"),
        "right": tr("Правая кнопка"),
        "middle": tr("Колёсико"),
        "x1": tr("Боковая 1"),
        "x2": tr("Боковая 2"),
        "wheelup": tr("Колесо вверх"),
        "wheeldown": tr("Колесо вниз"),
        "motion_up": tr("Движение вверх (аналог)"),
        "motion_down": tr("Движение вниз (аналог)"),
        "motion_left": tr("Движение влево (аналог)"),
        "motion_right": tr("Движение вправо (аналог)"),
    }
    parts = [p.strip() for p in str(val).split(",") if p.strip()]
    return ", ".join(names.get(p.lower(), p) for p in parts) if parts else "—"


def connected_gamepads():
    """Returns list of (GUID, name) of connected gamepads via bb-gpu-capabilities --gamepads."""
    tool = PORT_DIR / ("bin" if PACKAGED else "out") / "bb-gpu-capabilities"
    try:
        run = subprocess.run([str(tool), "--gamepads"], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return []
    return [tuple(line.split("\t", 1)) for line in run.stdout.splitlines() if "\t" in line]


def connected_displays():
    """(BB_DISPLAY value, label) of the monitors (bb-gpu-capabilities --displays), [] if unknown.
    The value is the monitor's name, or its number when two monitors share a name."""
    tool = PORT_DIR / ("bin" if PACKAGED else "out") / "bb-gpu-capabilities"
    try:
        run = subprocess.run([str(tool), "--displays"], capture_output=True, text=True, timeout=5)
    except (OSError, subprocess.SubprocessError):
        return []
    rows = [line.split("\t") for line in run.stdout.splitlines() if line.count("\t") == 2]
    names = [name for name, _, _ in rows]
    return [(name if names.count(name) == 1 else str(number),
             f"{number}: {name} ({size})" + (tr(", основной") if primary == "1" else ""))
            for number, (name, size, primary) in enumerate(rows, 1)]

