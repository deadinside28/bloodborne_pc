"""no_walls.py <dvdroot_ps4> <mod folder>: the walls the game raises between areas while two play
together, taken out as a loose-file mod for bbport (its launcher merges <data>/mods/* at start).

The wall event deactivates its object and map effect, waits for a multiplayer state (2 joined,
3 connecting), then turns both on until the session ends - in every machine's own copy of the
world, so both machines need the mod. Edited, each line checked against the 1.09 dump first:
- common 7600 and m29 12907600 (the Chalice Dungeons' copy): line 3, its first multiplayer test,
  becomes END: the wall stays off.
- m34_00_00_00 13400010 (Hunter's Nightmare): this wall is also up alone until flag 9471, so only
  its multiplayer tests change, each into "flag 9471 off" (the alone case's own test): together
  the wall comes and goes as it does alone.
Nothing else changes; the files are DCX-compressed again like the game's."""
import os
import struct
import sys
import zlib

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import souls  # noqa: E402

END = (1000, 4, bytes(4))                    # END Unconditionally(End)
FLAG_OFF = bytes([0xFF, 0, 0, 0]) + struct.pack('<I', 9471)       # 3[00] group -1: 9471 off
FLAG_OFF_2 = bytes([0xFE, 0, 0, 0]) + struct.pack('<I', 9471)     # the same in group -2


def header(d, i):
    return struct.unpack_from('<Q', d, 0x10 + 8 * i)[0]


def lines(d, eid):
    """Offsets of the event's instruction headers."""
    for k in range(header(d, 0)):
        e, n, io = struct.unpack_from('<QQq', d, header(d, 1) + 48 * k)
        if e == eid:
            return [header(d, 3) + io + 32 * j for j in range(n)]
    raise SystemExit(f'event {eid} not found')


def line(d, at):
    bank, iid, alen, aoff = struct.unpack_from('<iiQq', d, at)
    base = header(d, 13)
    return bank, iid, bytes(d[base + aoff:base + aoff + alen])


def expect(d, at, bank, iid, raw):
    got = line(d, at)
    if got != (bank, iid, raw):
        raise SystemExit(f'unexpected line at {at:#x}: {got} (wanted {bank}[{iid}] {raw.hex()})')


def set_line(d, at, bank, iid, aoff=None, alen=None):
    struct.pack_into('<ii', d, at, bank, iid)
    if aoff is not None:
        struct.pack_into('<Qq', d, at + 8, alen, aoff)


def append_args(d, raw):
    """Puts raw at the end of the arguments block (16 bytes, keeping the blocks after it
    aligned as before); returns its offset in the block and the new file."""
    raw = raw.ljust(16, b'\0')
    at = header(d, 13) + header(d, 12)
    out = bytearray(d[:at] + raw + d[at:])
    for i in (1, 3, 5, 7, 9, 11, 13, 15):  # block offsets: those at or after the insert move
        if header(out, i) >= at:
            struct.pack_into('<Q', out, 0x10 + 8 * i, header(out, i) + len(raw))
    struct.pack_into('<Q', out, 0x10 + 8 * 12, header(out, 12) + len(raw))
    struct.pack_into('<I', out, 0x0C, len(out))
    return at - header(d, 13), out


def mp_test(group, state):
    return 3, 6, bytes([group & 0xFF, state, 0, 0])


def wall_off(d, eid):
    at = lines(d, eid)
    expect(d, at[3], *mp_test(-1, 3))
    expect(d, at[4], *mp_test(-1, 2))
    if line(d, at[6])[:2] != (2005, 3):
        raise SystemExit(f'event {eid}: line 6 is not the wall going up')
    set_line(d, at[3], END[0], END[1])
    base = header(d, 13)
    aoff = struct.unpack_from('<q', d, at[3] + 16)[0]
    d[base + aoff:base + aoff + 4] = END[2]
    return d


def nightmare_wall(d):
    at = lines(d, 13400010)
    expect(d, at[3], *mp_test(-1, 3))
    expect(d, at[4], *mp_test(-1, 2))
    expect(d, at[5], 3, 0, FLAG_OFF)
    expect(d, at[9], *mp_test(-2, 3))
    expect(d, at[10], *mp_test(-2, 2))
    alen, aoff = struct.unpack_from('<Qq', d, at[5] + 8)
    for k in (3, 4):
        set_line(d, at[k], 3, 0, aoff, alen)
    aoff2, d = append_args(d, FLAG_OFF_2)
    at = lines(d, 13400010)
    for k in (9, 10):
        set_line(d, at[k], 3, 0, aoff2, len(FLAG_OFF_2))
    return d


def dcx(original, data):
    """data compressed as the original's DCX (DFLT, zlib level 9: the game's 78 DA)."""
    z = zlib.compress(bytes(data), 9)
    head = bytearray(original[:0x4C])
    assert head[:4] == b'DCX\0' and head[0x28:0x2C] == b'DFLT' and head[0x44:0x48] == b'DCA\0'
    struct.pack_into('>II', head, 0x1C, len(data), len(z))
    return bytes(head) + z


EDITS = {
    'common.emevd.dcx': lambda d: wall_off(d, 7600),
    'm29.emevd.dcx': lambda d: wall_off(d, 12907600),
    'm34_00_00_00.emevd.dcx': nightmare_wall,
}


def main(root, mod):
    os.makedirs(os.path.join(mod, 'dvdroot_ps4', 'event'), exist_ok=True)
    for name, edit in EDITS.items():
        original = open(os.path.join(root, 'event', name), 'rb').read()
        data = edit(bytearray(souls.dcx(original)))
        out = dcx(original, data)
        assert souls.dcx(out) == bytes(data)
        open(os.path.join(mod, 'dvdroot_ps4', 'event', name), 'wb').write(out)
        print(f'{name}: {len(out)} bytes')


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2])
