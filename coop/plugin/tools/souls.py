import struct, zlib

def dcx(data):
    if data[:4] != b'DCX\0':
        return data
    i = data.index(b'DCA\0') + 8
    return zlib.decompress(data[i:])

def bnd4(data):
    assert data[:4] == b'BND4', data[:4]
    count = struct.unpack_from('<i', data, 0x0C)[0]
    hsize = struct.unpack_from('<q', data, 0x20)[0]
    unicode, fmt = data[0x30], data[0x31]
    out = {}
    for k in range(count):
        p = 0x40 + k * hsize
        assert hsize == 36, hsize  # the layout BB's binders use: sizes, offset, id, name
        csize = struct.unpack_from('<q', data, p + 8)[0]
        off = struct.unpack_from('<I', data, p + 0x18)[0]
        name = ''
        if True:
            no = struct.unpack_from('<I', data, p + 0x20)[0]
            if unicode:
                e = no
                while data[e:e + 2] != b'\0\0':
                    e += 2
                name = data[no:e].decode('utf-16le')
            else:
                name = data[no:data.index(b'\0', no)].decode('shift_jis')
        out[name] = data[off:off + csize]
    return out

def param_rows(data):
    rows = struct.unpack_from('<H', data, 0x0A)[0]
    f2d = data[0x2D]
    out = {}
    if f2d & 4:  # 64-bit offsets
        for k in range(rows):
            rid, off = struct.unpack_from('<iq', data, 0x40 + k * 0x18)[0], struct.unpack_from('<q', data, 0x40 + k * 0x18 + 8)[0]
            out[rid] = off
    else:
        base = 0x30
        for k in range(rows):
            rid, off, _ = struct.unpack_from('<iII', data, base + k * 12)
            out[rid] = off
    return out

def emevd_events(data):
    assert data[:4] == b'EVD\0'
    big = data[4]
    is64 = data[5] == 0xFF
    assert is64, 'expected 64-bit EMEVD'
    ev_count, ev_off, ins_count, ins_off = struct.unpack_from('<qqqq', data, 0x10)
    events = []
    for k in range(ev_count):
        eid, n, ioff, pc, poff, rest = struct.unpack_from('<qqqqqI', data, ev_off + k * 0x30)
        ins = []
        for j in range(n):
            bank, iid = struct.unpack_from('<II', data, ins_off + ioff + j * 0x20)
            ins.append((bank, iid))
        events.append((eid, ins))
    return events
