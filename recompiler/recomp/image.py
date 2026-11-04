# SPDX-License-Identifier: GPL-2.0-or-later
"""The x86-64 image to recompile: its bytes at link addresses and its function ranges.

Inputs today: an ELF64 x86-64 file (test corpus, or the linked probe image once a loader for
BBPROBE5 is added). Function ranges come from the symbol table when there is one, else from
the unwind table (.eh_frame), which is what the game provides (162,959 FDEs, no symbols).
"""
from dataclasses import dataclass, field

from elftools.elf.elffile import ELFFile


@dataclass
class Function:
    start: int
    end: int  # exclusive; 0 when unknown
    name: str = ""


@dataclass
class Image:
    segments: list = field(default_factory=list)  # (vaddr, bytes, executable)
    functions: dict = field(default_factory=dict)  # start -> Function
    link_base: int = 0

    def read(self, addr, size):
        for vaddr, data, _ in self.segments:
            if vaddr <= addr and addr + size <= vaddr + len(data):
                return data[addr - vaddr:addr - vaddr + size]
        return None

    def is_code(self, addr):
        return any(x and v <= addr < v + len(d) for v, d, x in self.segments)

    def code_window(self, addr, limit=4096):
        """Bytes from addr to the end of its executable segment (at most limit)."""
        for vaddr, data, x in self.segments:
            if x and vaddr <= addr < vaddr + len(data):
                return data[addr - vaddr:addr - vaddr + limit]
        return b""


def load_elf(path, use_symbols=True):
    img = Image()
    with open(path, "rb") as f:
        elf = ELFFile(f)
        if elf["e_machine"] != "EM_X86_64":
            raise ValueError(f"{path}: not x86-64")
        loads = [s for s in elf.iter_segments() if s["p_type"] == "PT_LOAD"]
        img.link_base = min(s["p_vaddr"] for s in loads)
        for s in loads:
            data = s.data() + b"\0" * (s["p_memsz"] - s["p_filesz"])
            img.segments.append((s["p_vaddr"], data, bool(s["p_flags"] & 1)))
        if use_symbols:
            for name in (".symtab", ".dynsym"):
                tab = elf.get_section_by_name(name)
                if not tab:
                    continue
                for sym in tab.iter_symbols():
                    if sym["st_info"]["type"] == "STT_FUNC" and sym["st_value"] and sym["st_shndx"] != "SHN_UNDEF":
                        a = sym["st_value"]
                        img.functions.setdefault(a, Function(a, a + sym["st_size"], sym.name))
        if not img.functions or not use_symbols:
            for fn in fde_ranges(elf):
                img.functions.setdefault(fn.start, fn)
    return img


def fde_ranges(elf):
    """Function ranges from .eh_frame FDEs (the game's only source of function boundaries)."""
    if not elf.get_section_by_name(".eh_frame"):
        return []
    dw = elf.get_dwarf_info()
    out = []
    for entry in dw.EH_CFI_entries():
        h = getattr(entry, "header", None)
        if h is None or "initial_location" not in h:
            continue  # CIE or terminator
        start = h["initial_location"]
        out.append(Function(start, start + h["address_range"], f"fde_{start:x}"))
    return out
