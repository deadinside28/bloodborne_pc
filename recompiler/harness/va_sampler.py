#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
# Usage: va_sampler.py OUT.json  (run while bbport runs; stops when bb-probe exits, or after 3 h)
# Samples /proc/<bb-probe>/maps every 2 s; records the highest mapped end in the guest ranges.
# Guest memory: low region [0x0800000000, 0x1000000000) and user memory [0x1000000000, 0xfc00000000).
import os, time, json, sys
out = sys.argv[1]
best = {"low_max_end": 0, "user_max_end": 0, "user_bytes_peak": 0, "samples": 0, "pid": None,
        "first_sample": None, "last_sample": None}
def pid():
    for p in os.listdir("/proc"):
        if p.isdigit():
            try:
                if open(f"/proc/{p}/comm").read().strip() == "bb-probe":
                    return int(p)
            except OSError:
                pass
deadline = time.time() + 3 * 3600
while time.time() < deadline:
    p = pid()
    if p is None:
        if best["pid"]:
            break          # the game ended
        time.sleep(2); continue
    best["pid"] = p
    try:
        lines = open(f"/proc/{p}/maps").read().splitlines()
    except OSError:
        break
    user = 0
    for l in lines:
        a, b = (int(x, 16) for x in l.split()[0].split("-"))
        if 0x0800000000 <= a < 0x1000000000:
            best["low_max_end"] = max(best["low_max_end"], b)
        elif 0x1000000000 <= a < 0xfc00000000:
            best["user_max_end"] = max(best["user_max_end"], b)
            if "---p" not in l:  # accessible mappings only
                user += b - a
    best["user_bytes_peak"] = max(best["user_bytes_peak"], user)
    best["samples"] += 1
    now = time.strftime("%H:%M:%S")
    best["first_sample"] = best["first_sample"] or now
    best["last_sample"] = now
    json.dump({k: (hex(v) if k.endswith("max_end") else v) for k, v in best.items()}, open(out, "w"), indent=1)
    time.sleep(2)
