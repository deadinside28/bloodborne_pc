# SPDX-License-Identifier: GPL-2.0-or-later
"""The PS4 executable as recompiler input.

Reads a plaintext SELF (or plain ELF) with bbport's own parser (scripts/prepare.py), places
its loadable segments at their link addresses, applies its base-relative relocations (at link
base 0, so pointer tables hold link addresses), and takes function ranges from the unwind table:
the game's ELF has no section headers, so .eh_frame is found through PT_GNU_EH_FRAME and
.eh_frame_hdr, and each FDE gives [pc_begin, pc_begin + pc_range).
"""
import struct
import sys
from pathlib import Path

from .image import Function, Image

SCRIPTS = Path(__file__).resolve().parents[2] / "scripts"

PT_LOAD, PT_DYNAMIC, PT_GNU_EH_FRAME = 1, 2, 0x6474E550
PT_SCE_DYNLIBDATA, PT_SCE_RELRO = 0x61000000, 0x61000010
R_X86_64_RELATIVE = 8
R_X86_64_JUMP_SLOT = 7

# Imports that never return: CFG recovery ends a path after a call to one of them instead of
# decoding the padding and data that follow (32k calls to __stack_chk_fail alone).
NORETURN = ("__stack_chk_fail", "abort", "exit", "_exit", "_Exit", "quick_exit", "__cxa_throw",
            "__cxa_rethrow", "__cxa_call_unexpected", "__cxa_pure_virtual", "__cxa_bad_cast",
            "__cxa_bad_typeid", "_Unwind_Resume", "_ZSt9terminatev", "longjmp", "_longjmp",
            "pthread_exit", "scePthreadExit", "sceKernelExitProcess",
            "_ZSt14_Xlength_errorPKc", "_ZSt14_Xout_of_rangePKc", "_ZSt18_Xbad_allocv",
            "_ZSt10_Rng_abortPKc", "_ZSt7_ThrowcPKc", "_ZSt17__throw_bad_allocv",
            "_ZSt19__throw_logic_errorPKc", "_ZSt20__throw_length_errorPKc",
            "_ZSt20__throw_out_of_rangePKc", "_ZSt21__throw_runtime_errorPKc")


def _parse_self(data):
    if str(SCRIPTS) not in sys.path:
        sys.path.insert(0, str(SCRIPTS))
    import prepare  # bbport's SELF/ELF parser; checks every offset it reads
    return prepare.parse_self(data)


class _Reader:
    """Reads the image at link addresses."""

    def __init__(self, image):
        self.image = image

    def bytes(self, addr, n):
        b = self.image.read(addr, n)
        if b is None:
            raise ValueError(f"unmapped read {addr:#x}+{n}")
        return b

    def u8(self, a):
        return self.bytes(a, 1)[0]

    def uleb(self, a):
        v = shift = 0
        while True:
            b = self.u8(a)
            a += 1
            v |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                return v, a

    def sleb(self, a):
        v = shift = 0
        while True:
            b = self.u8(a)
            a += 1
            v |= (b & 0x7F) << shift
            shift += 7
            if not b & 0x80:
                if b & 0x40:
                    v -= 1 << shift
                return v, a

    def pointer(self, enc, a, datarel=0):
        """A DWARF-encoded pointer at a: (value, next address)."""
        if enc == 0xFF:
            return None, a
        fmt = enc & 0x0F
        sizes = {0x00: ("<Q", 8), 0x02: ("<H", 2), 0x03: ("<I", 4), 0x04: ("<Q", 8),
                 0x0A: ("<h", 2), 0x0B: ("<i", 4), 0x0C: ("<q", 8)}
        if fmt == 0x01:
            v, n = self.uleb(a)
        elif fmt == 0x09:
            v, n = self.sleb(a)
        elif fmt in sizes:
            f, size = sizes[fmt]
            v = struct.unpack(f, self.bytes(a, size))[0]
            n = a + size
        else:
            raise ValueError(f"pointer encoding {enc:#x}")
        app = enc & 0x70
        if app == 0x10:
            v += a
        elif app == 0x30:
            v += datarel
        elif app != 0:
            raise ValueError(f"pointer application {enc:#x}")
        return v & 0xFFFFFFFFFFFFFFFF, n


def _cie_encoding(r, cie):
    """The FDE pointer encoding ('R' augmentation) of the CIE at cie."""
    length = struct.unpack("<I", r.bytes(cie, 4))[0]
    a = cie + 4
    if length == 0xFFFFFFFF:
        a += 8
    a += 4  # CIE id
    version = r.u8(a)
    a += 1
    aug = b""
    while (c := r.u8(a)) != 0:
        aug += bytes([c])
        a += 1
    a += 1
    _, a = r.uleb(a)  # code alignment
    _, a = r.sleb(a)  # data alignment
    if version == 1:
        a += 1
    else:
        _, a = r.uleb(a)
    enc = 0x00
    if aug.startswith(b"z"):
        _, a = r.uleb(a)
        for c in aug[1:]:
            if c == ord("R"):
                enc = r.u8(a)
                a += 1
            elif c == ord("L"):
                a += 1
            elif c == ord("P"):
                penc = r.u8(a)
                _, a = r.pointer(penc & 0x7F, a + 1)
            elif c in (ord("S"), ord("B")):
                pass
            else:
                raise ValueError(f"CIE augmentation {aug!r}")
    return enc


def fde_functions(image, hdr_addr):
    """Function ranges from .eh_frame_hdr's sorted table and the FDEs it points at."""
    r = _Reader(image)
    version, ptr_enc, count_enc, table_enc = r.bytes(hdr_addr, 4)
    if version != 1:
        raise ValueError(f".eh_frame_hdr version {version}")
    a = hdr_addr + 4
    _, a = r.pointer(ptr_enc, a, hdr_addr)
    count, a = r.pointer(count_enc, a, hdr_addr)
    cie_enc = {}
    out = []
    for _ in range(count):
        start, a = r.pointer(table_enc, a, hdr_addr)
        fde, a = r.pointer(table_enc, a, hdr_addr)
        length = struct.unpack("<I", r.bytes(fde, 4))[0]
        p = fde + 4
        if length == 0xFFFFFFFF:
            p += 8
        cie_ptr = struct.unpack("<I", r.bytes(p, 4))[0]
        cie = p - cie_ptr
        if cie not in cie_enc:
            cie_enc[cie] = _cie_encoding(r, cie)
        enc = cie_enc[cie]
        pc_begin, q = r.pointer(enc, p + 4)
        pc_range, _ = r.pointer(enc & 0x0F, q)  # the range: same format, no application
        if pc_begin != start:
            raise ValueError(f"FDE at {fde:#x}: pc_begin {pc_begin:#x} != table entry {start:#x}")
        out.append(Function(start, start + pc_range, f"fde_{start:x}"))
    return out


def load_ps4(path):
    """The game's executable (eboot.bin) as an Image with FDE function ranges."""
    img = Image()
    r = _load_into(img, path, 0)
    img.link_base = r["link_base"]
    img.entry = r["entry"]
    img.relocations_applied = r["applied"]
    return img


# Imports and exports that return twice or jump across frames: hybrid execution keeps them, and
# their callers, original (recomp/game.py).
NONLOCAL = ("setjmp", "_setjmp", "sigsetjmp", "__sigsetjmp", "longjmp", "_longjmp", "siglongjmp",
            "getcontext", "setcontext", "swapcontext", "makecontext")


def load_ps4_linked(eboot, modules):
    """The executable with the game's bundled system modules (libc.prx, ...) placed as bbport's
    scripts/link_modules.py links them: modules = [(path, base)], base an image offset. Their
    exports that never return or return twice are noted by address (img.noreturn_funcs,
    img.nonlocal_funcs): calls inside a module reach them directly, not through a GOT slot."""
    img = load_ps4(eboot)
    import prepare  # noqa: E402  (on sys.path after _parse_self)
    noreturn = {prepare.nid(n) for n in NORETURN}
    nonlocal_ = {prepare.nid(n) for n in NONLOCAL}
    img.noreturn_funcs, img.nonlocal_funcs = set(), set()
    for path, base in modules:
        r = _load_into(img, path, base)
        for name, addr in r["exports"].items():
            nid = name.split("#")[0]
            if nid in noreturn:
                img.noreturn_funcs.add(addr)
            if nid in nonlocal_:
                img.nonlocal_funcs.add(addr)
    return img


def _load_into(img, path, base):
    """Places a PS4 module (SELF or ELF) in img at image offset `base`: segments, base-relative
    relocations (value base + addend), import GOT slots (img.imports, img.noreturn_got) and FDE
    function ranges. Returns its entry, link base, relocation count and exports {name: address}."""
    data = Path(path).read_bytes()
    elf, header, ph, _segments, _missing = _parse_self(data)
    if not hasattr(img, "imports"):
        img.imports = {}  # GOT slot -> import name (NID#library#module)
        img.noreturn_got = set()
    loads = [p for p in ph if p["type"] in (PT_LOAD, PT_SCE_RELRO)]
    first_new = len(img.segments)
    for p in loads:
        seg = bytearray(elf[p["offset"]:p["offset"] + p["filesz"]]) + bytes(p["memsz"] - p["filesz"])
        img.segments.append([base + p["vaddr"], seg, bool(p["flags"] & 1)])
        if not p["flags"] & 2 and p["type"] == PT_LOAD:  # RELRO is written by the loader first
            img.readonly.append((base + p["vaddr"], base + p["vaddr"] + p["memsz"]))
    new = img.segments[first_new:]
    # base-relative relocations (type 8: 231,478 of the game's 235,000)
    dyn = next(p for p in ph if p["type"] == PT_DYNAMIC)
    tags = {}
    for pos in range(dyn["offset"], dyn["offset"] + dyn["filesz"], 16):
        tag, value = struct.unpack_from("<QQ", elf, pos)
        if tag == 0:
            break
        tags[tag] = value
    lib = next(p for p in ph if p["type"] == PT_SCE_DYNLIBDATA)
    blob = elf[lib["offset"]:lib["offset"] + lib["filesz"]]
    strings = blob[tags[0x61000035]:tags[0x61000035] + tags[0x61000037]]
    syms = blob[tags[0x61000039]:tags[0x61000039] + tags[0x6100003F]]

    def symbol_name(i):
        off = struct.unpack_from("<I", syms, i * 24)[0]
        return strings[off:strings.index(0, off)].decode("ascii")
    import prepare  # noqa: E402  (on sys.path after _parse_self)
    noreturn_nids = {prepare.nid(n): n for n in NORETURN}
    applied = 0
    for off_tag, size_tag in ((0x61000029, 0x6100002D), (0x6100002F, 0x61000031)):
        if off_tag not in tags:
            continue
        table = blob[tags[off_tag]:tags[off_tag] + tags[size_tag]]
        for pos in range(0, len(table), 24):
            target, info, addend = struct.unpack_from("<QQq", table, pos)
            if info & 0xFFFFFFFF == R_X86_64_JUMP_SLOT:
                name = symbol_name(info >> 32)
                img.imports[base + target] = name
                if name.split("#")[0] in noreturn_nids:
                    img.noreturn_got.add(base + target)
            if info & 0xFFFFFFFF == R_X86_64_RELATIVE:
                for seg in new:
                    if seg[0] <= base + target and base + target + 8 <= seg[0] + len(seg[1]):
                        struct.pack_into("<Q", seg[1], base + target - seg[0], (base + addend) & 0xFFFFFFFFFFFFFFFF)
                        applied += 1
                        break
    img.segments[first_new:] = [(v, bytes(d), x) for v, d, x in new]
    exports = {}  # defined symbols: name -> address
    for i in range(len(syms) // 24):
        name_off, _info, _other, shndx, value, _size = struct.unpack_from("<IBBHQQ", syms, i * 24)
        if shndx and value:
            exports[strings[name_off:strings.index(0, name_off)].decode("ascii")] = base + value
    eh = next((p for p in ph if p["type"] == PT_GNU_EH_FRAME), None)
    if eh is None:
        raise ValueError("no PT_GNU_EH_FRAME: no function ranges")
    for fn in fde_functions(img, base + eh["vaddr"]):
        img.functions.setdefault(fn.start, fn)
    return dict(entry=base + header[4], link_base=base + min(p["vaddr"] for p in loads), applied=applied,
                exports=exports)


def is_ps4(path):
    with open(path, "rb") as f:
        magic = f.read(4)
    return magic == b"O\x15=\x1d"  # PS4 SELF
