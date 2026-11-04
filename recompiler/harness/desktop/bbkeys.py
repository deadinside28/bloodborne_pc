#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""Press keys in the game window through a temporary uinput keyboard (needs write access to
/dev/uinput, which logind grants the seat's user). Refuses to send unless the Bloodborne window
is the active one (focus_bb.sh focuses it first).

  bbkeys.py enter down down enter wait:3 esc hold:w:2.5
    key names: enter up down left right esc space q e f r tab backspace w a s d shift g
               i j k l (the camera)
    wait:SECONDS   pause;   hold:KEY:SECONDS   hold a key down
"""
import fcntl, os, struct, subprocess, sys, time

HERE = os.path.dirname(os.path.abspath(__file__))
KEYS = {"enter": 28, "up": 103, "down": 108, "left": 105, "right": 106, "esc": 1, "space": 57,
        "q": 16, "e": 18, "f": 33, "r": 19, "tab": 15, "backspace": 14, "w": 17, "a": 30,
        "s": 31, "d": 32, "shift": 42, "g": 34, "i": 23, "j": 36, "k": 37, "l": 38}
UI_SET_EVBIT, UI_SET_KEYBIT = 0x40045564, 0x40045565
UI_DEV_SETUP, UI_DEV_CREATE, UI_DEV_DESTROY = 0x405C5503, 0x5501, 0x5502
EV_SYN, EV_KEY = 0, 1


def event(fd, type_, code, value):
    os.write(fd, struct.pack("llHHi", 0, 0, type_, code, value))


def key(fd, code, down):
    event(fd, EV_KEY, code, 1 if down else 0)
    event(fd, EV_SYN, 0, 0)


def main(args):
    focus = subprocess.run([os.path.join(HERE, "focus_bb.sh")], capture_output=True, text=True).stdout.strip()
    # BB_KEYS_FOCUS_SEEN=1: KWin's script output does not reach the journal on this system (its
    # debug logging is off); the caller checked another way that the game is the active window
    # (a capture of the active window: spectacle -b -n -a).
    if "Bloodborne" not in focus and os.environ.get("BB_KEYS_FOCUS_SEEN") != "1":
        sys.exit(f"not sending: the game window is not active ({focus or 'no answer from KWin'})")
    fd = os.open("/dev/uinput", os.O_WRONLY | os.O_NONBLOCK)
    try:
        fcntl.ioctl(fd, UI_SET_EVBIT, EV_KEY)
        for code in KEYS.values():
            fcntl.ioctl(fd, UI_SET_KEYBIT, code)
        setup = struct.pack("HHHH80sI", 0x03, 0x1234, 0x5678, 1, b"bbport test keyboard", 0)
        fcntl.ioctl(fd, UI_DEV_SETUP, setup)
        fcntl.ioctl(fd, UI_DEV_CREATE)
        time.sleep(3.0)  # let libinput and KWin pick the new keyboard up (1 s was too short on Bazzite)
        for a in args:
            if a.startswith("wait:"):
                time.sleep(float(a[5:]))
                continue
            hold = 0.08
            if a.startswith("hold:"):
                _, a, secs = a.split(":")
                hold = float(secs)
            code = KEYS[a]
            key(fd, code, True)
            time.sleep(hold)
            key(fd, code, False)
            time.sleep(0.35)  # the game polls input once a frame; menus need distinct presses
            print(f"sent {a}" + (f" ({hold}s)" if hold > 0.1 else ""))
        time.sleep(0.2)
    finally:
        fcntl.ioctl(fd, UI_DEV_DESTROY)
        os.close(fd)


if __name__ == "__main__":
    main(sys.argv[1:])
