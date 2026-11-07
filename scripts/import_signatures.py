#!/usr/bin/env python3
# SPDX-License-Identifier: GPL-2.0-or-later
"""The C signatures of the runtime's imports, from the debug info: what a host-call bridge needs
to call each one with the game's arguments (recompiled code holds them as x86 SysV registers;
an AArch64 host passes them its own way).

    python3 scripts/import_signatures.py out/imports.txt -o src/import_signatures.inc

out/imports.txt: BB_IMPORT_DUMP (name, host address, file, offset in it). Each import becomes
{"name", "R:ARGS"}: R the return (v void, i integer/pointer, f float, d double), ARGS one letter
per parameter (i, f, d), "..." appended for a variadic function; 'x' marks what a bridge cannot
pass (structs by value, long double): such an import is listed and stays on the x86 thunk.
Needs pyelftools (the recompiler's venv has it)."""
import argparse
import os
import sys

from elftools.elf.elffile import ELFFile


def strip(die):
    """Through typedefs and qualifiers to the type itself (None: void)."""
    while die is not None and die.tag in ("DW_TAG_typedef", "DW_TAG_const_type", "DW_TAG_volatile_type",
                                          "DW_TAG_restrict_type", "DW_TAG_atomic_type"):
        die = die.get_DIE_from_attribute("DW_AT_type") if "DW_AT_type" in die.attributes else None
    return die


def kind(die):
    """One letter for a parameter or return type."""
    t = strip(die)
    if t is None:
        return "v"
    if t.tag in ("DW_TAG_pointer_type", "DW_TAG_reference_type", "DW_TAG_rvalue_reference_type",
                 "DW_TAG_enumeration_type", "DW_TAG_subroutine_type", "DW_TAG_ptr_to_member_type"):
        return "i"
    if t.tag == "DW_TAG_base_type":
        size = t.attributes["DW_AT_byte_size"].value
        encoding = t.attributes["DW_AT_encoding"].value
        if encoding == 0x04:  # DW_ATE_float
            return {4: "f", 8: "d"}.get(size, "x")
        return "i" if size <= 8 else "x"
    return "x"  # structs, unions, arrays by value


def entries(dwarf, cu):
    """(entry address, DIE) of each function with code in this CU: DW_AT_low_pc, or (GCC's
    hot/cold split functions) the start of each of DW_AT_ranges (relative to the CU's base address
    unless absolute or after a base address entry)."""
    ranges = None
    top = cu.get_top_DIE()
    cu_base = top.attributes["DW_AT_low_pc"].value if "DW_AT_low_pc" in top.attributes else 0
    for die in cu.iter_DIEs():
        if die.tag != "DW_TAG_subprogram":
            continue
        if "DW_AT_low_pc" in die.attributes:
            yield die.attributes["DW_AT_low_pc"].value, die
        if "DW_AT_ranges" in die.attributes:
            if ranges is None:
                ranges = dwarf.range_lists()
            try:
                rl = ranges.get_range_list_at_offset(die.attributes["DW_AT_ranges"].value, cu=cu)
            except TypeError:  # older pyelftools
                rl = ranges.get_range_list_at_offset(die.attributes["DW_AT_ranges"].value)
            base = cu_base
            for r in rl:
                if hasattr(r, "base_address"):  # a base address entry
                    base = r.base_address
                elif hasattr(r, "begin_offset"):
                    yield (r.begin_offset if r.is_absolute else base + r.begin_offset), die


def signature(die):
    """'R:ARGS' of a subprogram DIE (its declaration's when the definition points to one)."""
    decl = die
    if "DW_AT_specification" in die.attributes:
        decl = die.get_DIE_from_attribute("DW_AT_specification")
    if "DW_AT_abstract_origin" in die.attributes:
        decl = die.get_DIE_from_attribute("DW_AT_abstract_origin")
    ret = kind(decl.get_DIE_from_attribute("DW_AT_type")) if "DW_AT_type" in decl.attributes else "v"
    args, variadic = "", False
    source = die if any(c.tag == "DW_TAG_formal_parameter" for c in die.iter_children()) else decl
    for child in source.iter_children():
        if child.tag == "DW_TAG_formal_parameter":
            node = child
            if "DW_AT_type" not in node.attributes and "DW_AT_abstract_origin" in node.attributes:
                node = node.get_DIE_from_attribute("DW_AT_abstract_origin")
            args += kind(node.get_DIE_from_attribute("DW_AT_type")) if "DW_AT_type" in node.attributes else "x"
        elif child.tag == "DW_TAG_unspecified_parameters":
            variadic = True
    for child in decl.iter_children():
        if child.tag == "DW_TAG_unspecified_parameters":
            variadic = True
    name = decl.attributes.get("DW_AT_name")
    return ret + ":" + args + ("..." if variadic else ""), name.value.decode() if name else "?"


class Module:
    """A binary's functions: its symbol table names the function at an address, the debug info
    gives that function's types (the definition with that name; among same-named statics, the one
    whose code starts there)."""

    def __init__(self, path):
        self.file = open(path, "rb")
        self.elf = ELFFile(self.file)
        self.dwarf = self.elf.get_dwarf_info()
        self.pie = self.elf.header["e_type"] == "ET_DYN"
        self.symbols = {}  # address -> names
        for section in ("symtab", "dynsym"):
            table = self.elf.get_section_by_name("." + section)
            if not table:
                continue
            for sym in table.iter_symbols():
                if sym["st_info"]["type"] == "STT_FUNC" and sym["st_value"]:
                    self.symbols.setdefault(sym["st_value"], set()).add(sym.name.split(".")[0])
        self.index = None
        # C++: the unqualified name too (functions in anonymous namespaces have no linkage name
        # in the debug info, only DW_AT_name)
        mangled = sorted({n for names in self.symbols.values() for n in names if n.startswith("_Z")})
        if mangled:
            import subprocess
            out = subprocess.run(["c++filt"], input="\n".join(mangled), capture_output=True, text=True).stdout
            plain = dict(zip(mangled, out.splitlines()))
            for names in self.symbols.values():
                for n in list(names):
                    if n in plain:
                        base = plain[n].replace("(anonymous namespace)", "").split("(")[0].split("::")[-1].split("<")[0].strip()
                        if base:
                            names.add(base)

    def build(self, wanted):
        """name -> [(code addresses, DIE)] for the wanted names: every subprogram DIE (also those
        without code: GCC folds identical bodies into one, the others' symbols are aliases)."""
        self.index = {}
        for cu in self.dwarf.iter_CUs():
            starts = {}
            for address, die in entries(self.dwarf, cu):
                starts.setdefault(die.offset, []).append(address)
            for die in cu.iter_DIEs():
                if die.tag != "DW_TAG_subprogram":
                    continue
                names = set()
                for node in (die, die.get_DIE_from_attribute("DW_AT_specification")
                             if "DW_AT_specification" in die.attributes else None,
                             die.get_DIE_from_attribute("DW_AT_abstract_origin")
                             if "DW_AT_abstract_origin" in die.attributes else None):
                    if node is None:
                        continue
                    for attr in ("DW_AT_name", "DW_AT_linkage_name", "DW_AT_MIPS_linkage_name"):
                        if attr in node.attributes:
                            name = node.attributes[attr].value.decode()
                            names.add(name)
                            names.add(name.split("<")[0])  # a template's instances: StubFunction<51>
                for n in names & wanted:
                    self.index.setdefault(n, []).append((starts.get(die.offset, []), die))

    def names_at(self, address):
        return self.symbols.get(address, set())

    def find(self, address):
        """The signature of the function at `address`, or None (not found, or ambiguous)."""
        cands = []
        for n in self.names_at(address):
            cands += self.index.get(n, [])
        exact = [d for starts, d in cands if address in starts]
        if exact:
            return signature(exact[0])
        # by name: the same signature whichever DIE (declarations, folded copies, same-named statics)
        sigs = {signature(d) for _, d in cands}
        sigs_only = {sig for sig, _ in sigs}
        if len(sigs_only) == 1:
            return next(iter(sigs))
        return None


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument("imports")
    ap.add_argument("-o", "--output", required=True)
    ap.add_argument("--root", default=".", help="where relative module paths in the dump start")
    a = ap.parse_args()
    modules = {}
    rows, missing, unsupported = [], [], []
    imports = []
    for line in open(a.imports):
        parts = line.split()
        if len(parts) < 4:
            continue
        name, address, path, offset = parts[0], int(parts[1], 16), parts[2], int(parts[3], 16)
        path = path if os.path.isabs(path) else os.path.join(a.root, path)
        if path not in modules:
            modules[path] = Module(path)
        m = modules[path]
        imports.append((name, m, offset if m.pie else address))
    for m in modules.values():
        m.build({n for _, mm, at in imports if mm is m for n in mm.names_at(at)})
    for name, m, at in imports:
        found = m.find(at)
        if found is None:
            missing.append(name)
            continue
        sig, fname = found
        rows.append((name, sig, fname))
        if "x" in sig:
            unsupported.append((name, fname, sig))
    with open(a.output, "w") as f:
        f.write("/* Generated by scripts/import_signatures.py from the debug info: each runtime import's\n"
                " * signature (return:arguments; i integer/pointer, f float, d double, v void, x what a\n"
                " * bridge cannot pass, ... variadic). Do not edit. */\n")
        for name, sig, fname in sorted(rows):
            f.write(f'{{"{name}", "{sig}"}}, /* {fname} */\n')
    print(f"{len(rows)} signatures, {len(missing)} not found in the debug info, {len(unsupported)} with "
          f"arguments a bridge cannot pass", file=sys.stderr)
    for n in missing[:20]:
        print(f"  not found: {n}", file=sys.stderr)
    for n, fname, sig in unsupported[:40]:
        print(f"  unsupported: {n} ({fname}) {sig}", file=sys.stderr)


if __name__ == "__main__":
    main()
