# SPDX-License-Identifier: GPL-2.0-or-later
"""x86-64 -> LLVM IR.

Execution model (see recompiler/README.md):
- One LLVM function per guest function: `void @F_<link address>(ptr noalias %st)`.
- %st is the guest register file (struct RecompState in harness/recomp_rt.h). Registers live in
  allocas for the body and are written back to %st at every call, exit and return; mem2reg/DSE
  remove what is unused. Flags are function-local (not preserved across call/ret, as the SysV
  ABI and every compiler assume).
- Guest memory is host memory at the guest address (identity mapping). Code addresses are link
  addresses + @__recomp_slide (the image's load offset; 0 for an image at its link address).
- Calls push the guest return address, as the game's stack layout requires; `ret` pops it and
  returns to the LLVM caller. Indirect transfers go through @__recomp_dispatch.
- tso=True (AArch64 only): x86's memory ordering, for each load and store that may see another
  thread's data. rcpc="check" (the default): scalar accesses are acquire loads / release stores
  (ldapr/stlr: an acquire orders the load before everything after it, a release orders everything
  before the store ahead of it, and store->load stays free, exactly x86-TSO); those that cross a
  16-byte granule (which ldapr/stlr cannot do even with LSE2) take a plain access with a barrier.
  rcpc="fault": no check, the runtime's SIGBUS handler performs the crossing ones. rcpc=None:
  barriers only: dmb ishld after loads, dmb ishst before stores (earlier loads are ordered by their
  own barrier). Vector accesses always use the barriers. No barrier for stack accesses (rsp-based, and rbp-based
  in functions that set up a frame pointer: locals are assumed not to be shared with other
  threads) or for loads from read-only image segments (constants, jump tables).
"""
import iced_x86 as X

M = X.Mnemonic
OK = X.OpKind
R = X.Register
CC = X.ConditionCode

GPR64 = [R.RAX, R.RCX, R.RDX, R.RBX, R.RSP, R.RBP, R.RSI, R.RDI,
         R.R8, R.R9, R.R10, R.R11, R.R12, R.R13, R.R14, R.R15]
GPR_INDEX = {r: i for i, r in enumerate(GPR64)}
HIGH8 = {R.AH: R.RAX, R.CH: R.RCX, R.DH: R.RDX, R.BH: R.RBX}
RAX, RCX, RDX, RSP, RBP, RSI, RDI = 0, 1, 2, 4, 5, 6, 7
FLAGS = ("cf", "pf", "af", "zf", "sf", "of")

IMM_KINDS = {OK.IMMEDIATE8, OK.IMMEDIATE8_2ND, OK.IMMEDIATE16, OK.IMMEDIATE32, OK.IMMEDIATE64,
             OK.IMMEDIATE8TO16, OK.IMMEDIATE8TO32, OK.IMMEDIATE8TO64, OK.IMMEDIATE32TO64}

STATE_TYPE = "%State = type { [16 x i64], i64, i64, i64, i32, i32, [16 x i128], [16 x i128] }"
ST_GPR, ST_RIP, ST_FS, ST_GS, ST_MXCSR, ST_XMM, ST_YMMH = 0, 1, 2, 3, 4, 6, 7


class Unsupported(Exception):
    pass


def sconst(v, bits):
    v &= (1 << bits) - 1
    if bits > 1 and v >> (bits - 1):
        v -= 1 << bits
    return str(v)


def fname(addr):
    return f"@F_{addr:x}"


def xmm_index(reg):
    if R.XMM0 <= reg <= R.XMM15:
        return reg - R.XMM0, 128
    if R.YMM0 <= reg <= R.YMM15:
        return reg - R.YMM0, 256
    return None, 0


class FunctionLifter:
    def __init__(self, mod, cfg):
        self.mod = mod
        self.cfg = cfg
        self.out = []
        self.n = 0
        self.exits = []  # (label, kind, value)
        self.cur_ins = None
        self.frame_pointer = False

    # ---- emission helpers -------------------------------------------------
    def t(self):
        self.n += 1
        return f"%t{self.n}"

    def e(self, line):
        self.out.append("  " + line)

    def op(self, text):
        v = self.t()
        self.e(f"{v} = {text}")
        return v

    def label(self, name):
        self.out.append(f"{name}:")

    def uniq(self, name):
        """A label name unique in this function (the same instruction can be lifted in two
        blocks when decoding paths overlap)."""
        self.n += 1
        return f"{name}_{self.n}"

    # ---- register file ------------------------------------------------------
    def gpr_ptr(self, i):
        return f"%r{i}"

    def get64(self, i):
        return self.op(f"load i64, ptr %r{i}")

    def set64(self, i, v):
        self.e(f"store i64 {v}, ptr %r{i}")

    def read_reg(self, reg):
        if reg in HIGH8:
            full = self.get64(GPR_INDEX[HIGH8[reg]])
            s = self.op(f"lshr i64 {full}, 8")
            return self.op(f"trunc i64 {s} to i8"), 8
        if reg == R.RIP:
            raise Unsupported("rip operand")
        info = X.RegisterInfo(reg)
        full = info.full_register
        if full in GPR_INDEX:
            bits = info.size * 8
            v = self.get64(GPR_INDEX[full])
            if bits == 64:
                return v, 64
            return self.op(f"trunc i64 {v} to i{bits}"), bits
        idx, bits = xmm_index(reg)
        if idx is not None:
            return self.read_vec(idx, bits), bits
        raise Unsupported(f"register {reg}")

    def write_reg(self, reg, v, bits):
        if reg in HIGH8:
            i = GPR_INDEX[HIGH8[reg]]
            old = self.get64(i)
            z = self.op(f"zext i8 {v} to i64")
            s = self.op(f"shl i64 {z}, 8")
            m = self.op(f"and i64 {old}, -65281")
            self.set64(i, self.op(f"or i64 {m}, {s}"))
            return
        info = X.RegisterInfo(reg)
        full = info.full_register
        if full in GPR_INDEX:
            i = GPR_INDEX[full]
            rb = info.size * 8
            if rb != bits:
                v = self.resize(v, bits, rb)
            if rb == 64:
                self.set64(i, v)
            elif rb == 32:
                self.set64(i, self.op(f"zext i32 {v} to i64"))
            else:
                old = self.get64(i)
                z = self.op(f"zext i{rb} {v} to i64")
                m = self.op(f"and i64 {old}, {sconst(~((1 << rb) - 1), 64)}")
                self.set64(i, self.op(f"or i64 {m}, {z}"))
            return
        idx, rbits = xmm_index(reg)
        if idx is not None:
            self.write_vec(idx, v, bits, rbits)
            return
        raise Unsupported(f"register {reg}")

    def read_vec(self, idx, bits):
        lo = self.op(f"load i128, ptr %x{idx}")
        if bits == 128:
            return lo
        hi = self.op(f"load i128, ptr %h{idx}")
        a = self.op(f"zext i128 {lo} to i256")
        b = self.op(f"zext i128 {hi} to i256")
        c = self.op(f"shl i256 {b}, 128")
        return self.op(f"or i256 {a}, {c}")

    def write_vec(self, idx, v, bits, reg_bits, zero_upper=None):
        """Write an xmm/ymm register. Legacy SSE keeps bits 128..255; VEX zeroes them."""
        if zero_upper is None:
            zero_upper = self.is_vex()
        if bits == 256:
            lo = self.op(f"trunc i256 {v} to i128")
            s = self.op(f"lshr i256 {v}, 128")
            hi = self.op(f"trunc i256 {s} to i128")
            self.e(f"store i128 {lo}, ptr %x{idx}")
            self.e(f"store i128 {hi}, ptr %h{idx}")
            return
        if bits < 128:
            v = self.op(f"zext i{bits} {v} to i128")
        self.e(f"store i128 {v}, ptr %x{idx}")
        if zero_upper:
            self.e(f"store i128 0, ptr %h{idx}")

    def is_vex(self):
        return self.cur_ins.encoding in (X.EncodingKind.VEX, X.EncodingKind.EVEX)

    def resize(self, v, frm, to, signed=False):
        if frm == to:
            return v
        if frm > to:
            return self.op(f"trunc i{frm} {v} to i{to}")
        return self.op(f"{'sext' if signed else 'zext'} i{frm} {v} to i{to}")

    # ---- flags ----------------------------------------------------------------
    def setf(self, name, v):
        self.e(f"store i1 {v}, ptr %{name}")

    def getf(self, name):
        return self.op(f"load i1, ptr %{name}")

    def flags_zsp(self, res, bits):
        self.setf("zf", self.op(f"icmp eq i{bits} {res}, 0"))
        self.setf("sf", self.op(f"icmp slt i{bits} {res}, 0"))
        lo = self.resize(res, bits, 8)
        p = self.op(f"call i8 @llvm.ctpop.i8(i8 {lo})")
        pb = self.op(f"trunc i8 {p} to i1")
        self.setf("pf", self.op(f"xor i1 {pb}, true"))

    def flags_af(self, a, b, res, bits):
        x = self.op(f"xor i{bits} {a}, {b}")
        y = self.op(f"xor i{bits} {x}, {res}")
        z = self.op(f"and i{bits} {y}, 16")
        self.setf("af", self.op(f"icmp ne i{bits} {z}, 0"))

    def flags_logic(self, res, bits):
        self.setf("cf", "false")
        self.setf("of", "false")
        self.setf("af", "false")
        self.flags_zsp(res, bits)

    def flags_add(self, a, b, res, bits, carry_in=None):
        if carry_in is None:
            self.setf("cf", self.op(f"icmp ult i{bits} {res}, {a}"))
        else:
            w = bits * 2
            za = self.resize(a, bits, w)
            zb = self.resize(b, bits, w)
            zc = self.op(f"zext i1 {carry_in} to i{w}")
            s1 = self.op(f"add i{w} {za}, {zb}")
            s2 = self.op(f"add i{w} {s1}, {zc}")
            hi = self.op(f"lshr i{w} {s2}, {bits}")
            self.setf("cf", self.op(f"trunc i{w} {hi} to i1"))
        x = self.op(f"xor i{bits} {a}, {res}")
        y = self.op(f"xor i{bits} {b}, {res}")
        z = self.op(f"and i{bits} {x}, {y}")
        self.setf("of", self.op(f"icmp slt i{bits} {z}, 0"))
        self.flags_af(a, b, res, bits)
        self.flags_zsp(res, bits)

    def flags_sub(self, a, b, res, bits, borrow_in=None):
        if borrow_in is None:
            self.setf("cf", self.op(f"icmp ult i{bits} {a}, {b}"))
        else:
            w = bits * 2
            za = self.resize(a, bits, w)
            zb = self.resize(b, bits, w)
            zc = self.op(f"zext i1 {borrow_in} to i{w}")
            s = self.op(f"add i{w} {zb}, {zc}")
            self.setf("cf", self.op(f"icmp ult i{w} {za}, {s}"))
        x = self.op(f"xor i{bits} {a}, {b}")
        y = self.op(f"xor i{bits} {a}, {res}")
        z = self.op(f"and i{bits} {x}, {y}")
        self.setf("of", self.op(f"icmp slt i{bits} {z}, 0"))
        self.flags_af(a, b, res, bits)
        self.flags_zsp(res, bits)

    def cond(self, cc):
        g = self.getf
        if cc == CC.O:
            return g("of")
        if cc == CC.NO:
            return self.op(f"xor i1 {g('of')}, true")
        if cc == CC.B:
            return g("cf")
        if cc == CC.AE:
            return self.op(f"xor i1 {g('cf')}, true")
        if cc == CC.E:
            return g("zf")
        if cc == CC.NE:
            return self.op(f"xor i1 {g('zf')}, true")
        if cc in (CC.BE, CC.A):
            v = self.op(f"or i1 {g('cf')}, {g('zf')}")
            return v if cc == CC.BE else self.op(f"xor i1 {v}, true")
        if cc == CC.S:
            return g("sf")
        if cc == CC.NS:
            return self.op(f"xor i1 {g('sf')}, true")
        if cc == CC.P:
            return g("pf")
        if cc == CC.NP:
            return self.op(f"xor i1 {g('pf')}, true")
        if cc in (CC.L, CC.GE):
            v = self.op(f"xor i1 {g('sf')}, {g('of')}")
            return v if cc == CC.L else self.op(f"xor i1 {v}, true")
        if cc in (CC.LE, CC.G):
            l = self.op(f"xor i1 {g('sf')}, {g('of')}")
            v = self.op(f"or i1 {g('zf')}, {l}")
            return v if cc == CC.LE else self.op(f"xor i1 {v}, true")
        raise Unsupported(f"condition {cc}")

    # ---- memory -----------------------------------------------------------------
    def ea(self, ins, with_segment=True):
        """Effective address of the instruction's memory operand (i64)."""
        if ins.is_ip_rel_memory_operand:
            addr = self.code_addr(ins.ip_rel_memory_address)
            return self.segment(ins, addr) if with_segment else addr
        addr = sconst(ins.memory_displacement, 64)
        addr32 = any(r != R.NONE and X.RegisterInfo(r).size == 4 for r in (ins.memory_base, ins.memory_index))
        if ins.memory_base != R.NONE:
            b = self.read_reg64(ins.memory_base)
            addr = self.op(f"add i64 {b}, {addr}")
        if ins.memory_index != R.NONE:
            if ins.is_vsib:
                raise Unsupported("vsib")
            ix = self.read_reg64(ins.memory_index)
            if ins.memory_index_scale != 1:
                ix = self.op(f"mul i64 {ix}, {ins.memory_index_scale}")
            addr = self.op(f"add i64 {addr}, {ix}")
        if addr32:  # 0x67 address size: the sum wraps at 32 bits
            addr = self.op(f"and i64 {addr}, 4294967295")
        if with_segment:
            addr = self.segment(ins, addr)
        if not addr.startswith("%"):
            addr = self.op(f"add i64 0, {addr}")
        return addr

    def segment(self, ins, addr):
        if ins.memory_segment not in (R.FS, R.GS):
            return addr
        field = ST_FS if ins.memory_segment == R.FS else ST_GS
        p = self.op(f"getelementptr inbounds %State, ptr %st, i32 0, i32 {field}")
        base = self.op(f"load i64, ptr {p}")
        return self.op(f"add i64 {addr}, {base}")

    def read_reg64(self, reg):
        info = X.RegisterInfo(reg)
        if info.full_register not in GPR_INDEX:
            raise Unsupported(f"address register {reg}")
        v = self.get64(GPR_INDEX[info.full_register])
        if info.size == 4:  # 32-bit addressing
            v = self.op(f"and i64 {v}, 4294967295")
        return v

    def code_addr(self, link_addr):
        return self.op(f"add i64 %slide, {sconst(link_addr, 64)}")

    def stack_relative(self, ins):
        """No ordering barrier needed: a stack access (rsp, or rbp in a frame-pointer function),
        or a RIP-relative access to a read-only image segment."""
        if ins.memory_segment in (R.FS, R.GS):
            return False
        if ins.memory_base in (R.RSP, R.ESP):
            return True
        if ins.memory_base == R.RBP and self.frame_pointer and self.mod.local_frames:
            return True
        return ins.is_ip_rel_memory_operand and self.mod.image.is_readonly(ins.ip_rel_memory_address)

    def load(self, addr, bits, local=False):
        p = self.op(f"inttoptr i64 {addr} to ptr")
        if self.mod.volatile and not local:
            return self.op(f"load volatile i{bits}, ptr {p}, align 1")
        if self.mod.tso and not local and self.mod.rcpc and bits <= 64:
            # an acquire load (ldapr/ldapur), which assumes alignment;
            # LSE2 CPUs accept misalignment within 16 bytes and fault across a 16-byte boundary
            # ("fault": the runtime's SIGBUS handler does those; "check": test inline and use a
            # plain load + barrier for the crossing ones)
            if self.mod.rcpc == "check" and bits > 8:
                return self._split_access(addr, bits // 8,
                                          lambda: self.op(f"load atomic i{bits}, ptr {p} acquire, align {bits // 8}"),
                                          lambda: self._plain_load_acquire(p, bits), bits)
            return self.op(f"load atomic i{bits}, ptr {p} acquire, align {bits // 8}")
        v = self.op(f"load i{bits}, ptr {p}, align 1")
        if self.mod.tso and not local:
            self.e("fence acquire")
        return v

    def store(self, addr, v, bits, local=False):
        p = self.op(f"inttoptr i64 {addr} to ptr")
        if self.mod.volatile and not local:
            self.e(f"store volatile i{bits} {v}, ptr {p}, align 1")
            return
        if self.mod.tso and not local and self.mod.rcpc and bits <= 64:
            fast = lambda: self.e(f"store atomic i{bits} {v}, ptr {p} release, align {bits // 8}")  # stlr / stlur
            if self.mod.rcpc == "check" and bits > 8:
                def slow():
                    self.store_barrier()
                    self.e(f"store i{bits} {v}, ptr {p}, align 1")
                self._split_access(addr, bits // 8, fast, slow, None)
            else:
                fast()
            return
        if self.mod.tso and not local:
            self.store_barrier()
        self.e(f"store i{bits} {v}, ptr {p}, align 1")

    def _plain_load_acquire(self, p, bits):
        v = self.op(f"load i{bits}, ptr {p}, align 1")
        self.e("fence acquire")
        return v

    def _split_access(self, addr, size, fast, slow, bits):
        """if (addr & 15) + size <= 16: fast() else slow(); a phi of their values when bits."""
        lo = self.op(f"and i64 {addr}, 15")
        end = self.op(f"add i64 {lo}, {size}")
        inside = self.op(f"icmp ule i64 {end}, 16")
        f, s_, j = self.uniq("rc_fast"), self.uniq("rc_slow"), self.uniq("rc_join")
        self.e(f"br i1 {inside}, label %{f}, label %{s_}")
        self.label(f)
        vf = fast()
        self.e(f"br label %{j}")
        self.label(s_)
        vs = slow()
        self.e(f"br label %{j}")
        self.label(j)
        if bits:
            return self.op(f"phi i{bits} [ {vf}, %{f} ], [ {vs}, %{s_} ]")
        return None

    def store_barrier(self):
        """Store-store ordering before a shared store: dmb ishst (AArch64 barrier option 10)."""
        self.mod.intrinsics.add("declare void @llvm.aarch64.dmb(i32)")
        self.e("call void @llvm.aarch64.dmb(i32 10)")

    def mem_bits(self, ins):
        return X.MemorySizeInfo(ins.memory_size).size * 8

    # ---- operands -------------------------------------------------------------------
    def op_bits(self, ins, i):
        k = ins.op_kind(i)
        if k == OK.REGISTER:
            r = ins.op_register(i)
            if r in HIGH8:
                return 8
            idx, bits = xmm_index(r)
            if idx is not None:
                return bits
            return X.RegisterInfo(r).size * 8
        if k == OK.MEMORY:
            return self.mem_bits(ins)
        return None

    def read(self, ins, i, bits=None):
        k = ins.op_kind(i)
        if k == OK.REGISTER:
            v, b = self.read_reg(ins.op_register(i))
            if bits and bits != b:
                v = self.resize(v, b, bits)
            return v
        if k == OK.MEMORY:
            mb = self.mem_bits(ins)
            v = self.load(self.ea(ins), mb, self.stack_relative(ins))
            if bits and bits != mb:
                v = self.resize(v, mb, bits)
            return v
        if k in IMM_KINDS:
            return sconst(ins.immediate(i), bits or 64)
        raise Unsupported(f"operand kind {k}")

    def write(self, ins, i, v, bits, addr=None):
        k = ins.op_kind(i)
        if k == OK.REGISTER:
            self.write_reg(ins.op_register(i), v, bits)
            return
        if k == OK.MEMORY:
            mb = self.mem_bits(ins)
            if mb != bits:
                v = self.resize(v, bits, mb)
            self.store(addr if addr is not None else self.ea(ins), v, mb, self.stack_relative(ins))
            return
        raise Unsupported(f"write to operand kind {k}")

    def rmw(self, ins):
        """(value, bits, addr) of operand 0 for read-modify-write (address computed once)."""
        bits = self.op_bits(ins, 0)
        if ins.op_kind(0) == OK.MEMORY:
            addr = self.ea(ins)
            return self.load(addr, bits, self.stack_relative(ins)), bits, addr
        return self.read(ins, 0), bits, None

    # ---- whole-state transfer ---------------------------------------------------------
    # Vector registers that cross a boundary. Default: all of them. abi=True (SysV, which the
    # PS4 toolchain follows): arguments in xmm0-7, results in xmm0-1, all vector registers
    # caller-saved, so a call needs xmm0-7 stored before it and only xmm0-1 read back after it.
    def vec_set(self, where):
        if not self.mod.abi:
            return range(16)
        return {"call": range(8), "after_call": range(2), "ret": range(2), "entry": range(16)}[where]

    def spill(self, where="call"):
        for i in range(16):
            p = self.op(f"getelementptr inbounds %State, ptr %st, i32 0, i32 {ST_GPR}, i32 {i}")
            self.e(f"store i64 {self.get64(i)}, ptr {p}")
        for i in self.vec_set(where):
            for arr, field in (("x", ST_XMM), ("h", ST_YMMH)):
                p = self.op(f"getelementptr inbounds %State, ptr %st, i32 0, i32 {field}, i32 {i}")
                v = self.op(f"load i128, ptr %{arr}{i}")
                self.e(f"store i128 {v}, ptr {p}")

    def reload(self, where="after_call"):
        for i in range(16):
            p = self.op(f"getelementptr inbounds %State, ptr %st, i32 0, i32 {ST_GPR}, i32 {i}")
            self.set64(i, self.op(f"load i64, ptr {p}"))
        for i in self.vec_set(where):
            for arr, field in (("x", ST_XMM), ("h", ST_YMMH)):
                p = self.op(f"getelementptr inbounds %State, ptr %st, i32 0, i32 {field}, i32 {i}")
                v = self.op(f"load i128, ptr {p}")
                self.e(f"store i128 {v}, ptr %{arr}{i}")

    def push(self, v):
        sp = self.get64(RSP)
        n = self.op(f"sub i64 {sp}, 8")
        self.store(n, v, 64, local=True)
        self.set64(RSP, n)

    def pop(self):
        sp = self.get64(RSP)
        v = self.load(sp, 64, local=True)
        self.set64(RSP, self.op(f"add i64 {sp}, 8"))
        return v

    def call_target(self, link_target, jump=False):
        """Direct call: the lifted function; an import stub's target (its GOT entry, read at run
        time); or the dispatcher for code we do not have. jump: a jump, not a call, to code that is
        not a known function start (the middle of other code): __recomp_dispatch_jump, as [rsp]
        is no return address there."""
        if link_target in self.mod.lifted_names:
            self.e(f"call void {fname(link_target)}(ptr %st)")
            return
        got = self.mod.import_stub(link_target)
        if got is not None:  # `jmp [rip+got]`: go where the stub would go, without lifting it
            p = self.op(f"inttoptr i64 {self.code_addr(got)} to ptr")
            target = self.op(f"load i64, ptr {p}")
            self.e(f"call void @__recomp_dispatch(ptr %st, i64 {target})")
        else:
            self.mod.unresolved.add(link_target)
            how = "__recomp_dispatch_jump" if jump and link_target not in self.mod.image.functions else "__recomp_dispatch"
            self.e(f"call void @{how}(ptr %st, i64 {self.code_addr(link_target)})")

    def exit_to(self, link_target):
        """Leave the function towards link_target (tail call, or a jump into other code)."""
        self.spill()
        self.call_target(link_target, jump=True)
        self.e("ret void")

    def exit_dynamic(self, runtime_target):
        """An indirect jump the CFG could not resolve: a tail call, or a jump into the middle of
        code. __recomp_dispatch_jump: a recompiled function start is called (a tail call); any
        other target gets control for good (hybrid execution: the original code continues there,
        on the guest's stack, and returns along its return addresses)."""
        self.spill()
        self.e(f"call void @__recomp_dispatch_jump(ptr %st, i64 {runtime_target})")
        self.e("ret void")

    def trap(self, kind):
        self.spill()
        self.e(f"call void @__recomp_trap(ptr %st, i64 {self.code_addr(self.cur_ins.ip)}, i32 {kind})")
        self.e("unreachable")

    # ---- function -----------------------------------------------------------------------
    def lift(self):
        cfg = self.cfg
        entry = cfg.blocks.get(cfg.start)
        ins0 = entry.insns[:2] if entry else []
        # `push rbp; mov rbp, rsp`: rbp addresses this function's frame until its epilogue
        self.frame_pointer = len(ins0) == 2 and ins0[0].mnemonic == M.PUSH and ins0[0].op0_register == R.RBP \
            and ins0[1].mnemonic == M.MOV and ins0[1].op0_kind == OK.REGISTER and ins0[1].op0_register == R.RBP \
            and ins0[1].op1_kind == OK.REGISTER and ins0[1].op1_register == R.RSP
        self.out.append(f"define void {fname(cfg.start)}(ptr noalias %st) {{")
        self.label("entry")
        for i in range(16):
            self.e(f"%r{i} = alloca i64")
        for i in range(16):
            self.e(f"%x{i} = alloca i128")
            self.e(f"%h{i} = alloca i128")
        for f in FLAGS:
            self.e(f"%{f} = alloca i1")
            self.e(f"store i1 false, ptr %{f}")
        self.e("%slide = load i64, ptr @__recomp_slide")
        self.reload("entry")
        self.e(f"br label %b_{cfg.start:x}")
        for start in sorted(cfg.blocks):
            self.lift_block(cfg.blocks[start])
        for lbl, kind, value in self.exits:
            self.label(lbl)
            if kind == "tail":
                self.exit_to(value)
        self.out.append("}")
        return "\n".join(self.out)

    def target_label(self, addr):
        if addr in self.cfg.blocks:
            return f"%b_{addr:x}"
        lbl = f"x_{len(self.exits)}_{addr:x}"
        self.exits.append((lbl, "tail", addr))
        return f"%{lbl}"

    def lift_block(self, b):
        self.label(f"b_{b.start:x}")
        terminated = False
        for ins in b.insns:
            self.cur_ins = ins
            try:
                terminated = self.lift_insn(ins, b)
            except Unsupported as ex:
                self.mod.unsupported[str(ex)] = self.mod.unsupported.get(str(ex), 0) + 1
                self.trap(1)
                terminated = True
            if terminated:
                break
        if not terminated:
            last = b.insns[-1]
            nxt = last.next_ip
            self.e(f"br label {self.target_label(nxt)}")

    def lift_insn(self, ins, b):
        """Emit one instruction. True when it ended the block."""
        mn = ins.mnemonic
        h = self.mod.handlers.get(mn)
        if h is None:
            raise Unsupported(f"mnemonic {X_MNEMONIC_NAMES.get(mn, mn)}")
        if ins.has_rep_prefix or ins.has_repne_prefix:
            if not ins.is_string_instruction:
                pass  # `rep ret`, `rep bsf` (= tzcnt encoding handled by decoder)
        return h(self, ins, b) or False


X_MNEMONIC_NAMES = {v: k for k, v in vars(M).items() if isinstance(v, int)}


# ======================================================================================
# Instruction semantics. Each handler: (lifter, ins, block) -> True if it ends the block.
# ======================================================================================
H = {}


def sem(*mnemonics):
    def deco(fn):
        for m in mnemonics:
            H[m] = fn
        return fn
    return deco


@sem(M.NOP, M.ENDBR64, M.PAUSE, M.PREFETCHT0, M.PREFETCHT1, M.PREFETCHT2, M.PREFETCHNTA, M.PREFETCHW,
     M.LFENCE, M.SFENCE)
def _nop(L, ins, b):
    pass


@sem(M.MFENCE)
def _mfence(L, ins, b):
    L.e("fence seq_cst")


@sem(M.INT3, M.UD2)
def _trap(L, ins, b):
    L.trap(2)
    return True


@sem(M.MOV)
def _mov(L, ins, b):
    bits = L.op_bits(ins, 0)
    if ins.op_kind(0) == OK.REGISTER and ins.op_register(0) in (R.FS, R.GS, R.ES, R.DS, R.SS, R.CS):
        raise Unsupported("segment register write")
    if ins.op_kind(1) == OK.REGISTER and ins.op_register(1) in (R.FS, R.GS, R.ES, R.DS, R.SS, R.CS):
        raise Unsupported("segment register read")
    L.write(ins, 0, L.read(ins, 1, bits), bits)


@sem(M.MOVZX, M.MOVSX, M.MOVSXD)
def _movx(L, ins, b):
    db = L.op_bits(ins, 0)
    sb = L.op_bits(ins, 1)
    v = L.read(ins, 1)
    L.write(ins, 0, L.resize(v, sb, db, signed=ins.mnemonic != M.MOVZX), db)


@sem(M.LEA)
def _lea(L, ins, b):
    bits = L.op_bits(ins, 0)
    a = L.ea(ins, with_segment=False)
    L.write(ins, 0, L.resize(a, 64, bits), bits)


@sem(M.XCHG)
def _xchg(L, ins, b):
    bits = L.op_bits(ins, 0)
    if ins.op_kind(0) == OK.REGISTER and ins.op_kind(1) == OK.REGISTER:
        a = L.read(ins, 0)
        c = L.read(ins, 1)
        L.write(ins, 0, c, bits)
        L.write(ins, 1, a, bits)
        return
    mi, ri = (0, 1) if ins.op_kind(0) == OK.MEMORY else (1, 0)
    addr = L.ea(ins)
    p = L.op(f"inttoptr i64 {addr} to ptr")
    v = L.read(ins, ri)
    old = L.op(f"atomicrmw xchg ptr {p}, i{bits} {v} seq_cst, align {bits // 8}")
    L.write(ins, ri, old, bits)


ALU = {M.ADD: "add", M.SUB: "sub", M.AND: "and", M.OR: "or", M.XOR: "xor", M.ADC: "adc", M.SBB: "sbb",
       M.CMP: "sub", M.TEST: "and"}
ATOMIC_OP = {"add": "add", "sub": "sub", "and": "and", "or": "or", "xor": "xor"}


@sem(*ALU)
def _alu(L, ins, b):
    kind = ALU[ins.mnemonic]
    bits = L.op_bits(ins, 0)
    writes = ins.mnemonic not in (M.CMP, M.TEST)
    # xor r, r: zero idiom (no dependency on the old value)
    if ins.mnemonic in (M.XOR, M.SUB) and ins.op_kind(0) == OK.REGISTER and ins.op_kind(1) == OK.REGISTER \
            and ins.op_register(0) == ins.op_register(1):
        L.write(ins, 0, "0", bits)
        if ins.mnemonic == M.XOR:
            L.flags_logic("0", bits)
        else:
            L.flags_sub("0", "0", "0", bits)
        return
    if ins.has_lock_prefix and kind in ATOMIC_OP:
        addr = L.ea(ins)
        p = L.op(f"inttoptr i64 {addr} to ptr")
        src = L.read(ins, 1, bits)
        old = L.op(f"atomicrmw {ATOMIC_OP[kind]} ptr {p}, i{bits} {src} seq_cst, align {bits // 8}")
        res = L.op(f"{kind} i{bits} {old}, {src}")
        _alu_flags(L, kind, old, src, res, bits, None)
        return
    if ins.has_lock_prefix:
        raise Unsupported(f"lock {kind}")
    a, bits, addr = L.rmw(ins) if writes else (L.read(ins, 0), bits, None)
    src = L.read(ins, 1, bits)
    carry = None
    if kind == "adc":
        carry = L.getf("cf")
        s = L.op(f"add i{bits} {a}, {src}")
        res = L.op(f"add i{bits} {s}, {L.op(f'zext i1 {carry} to i{bits}')}")
    elif kind == "sbb":
        carry = L.getf("cf")
        s = L.op(f"sub i{bits} {a}, {src}")
        res = L.op(f"sub i{bits} {s}, {L.op(f'zext i1 {carry} to i{bits}')}")
    else:
        res = L.op(f"{kind} i{bits} {a}, {src}")
    _alu_flags(L, kind, a, src, res, bits, carry)
    if writes:
        L.write(ins, 0, res, bits, addr)


def _alu_flags(L, kind, a, src, res, bits, carry):
    if kind in ("add", "adc"):
        L.flags_add(a, src, res, bits, carry)
    elif kind in ("sub", "sbb"):
        L.flags_sub(a, src, res, bits, carry)
    else:
        L.flags_logic(res, bits)


@sem(M.INC, M.DEC)
def _incdec(L, ins, b):
    if ins.has_lock_prefix:
        bits = L.op_bits(ins, 0)
        p = L.op(f"inttoptr i64 {L.ea(ins)} to ptr")
        op = "add" if ins.mnemonic == M.INC else "sub"
        a = L.op(f"atomicrmw {op} ptr {p}, i{bits} 1 seq_cst, align {bits // 8}")
        res = L.op(f"{op} i{bits} {a}, 1")
    else:
        a, bits, addr = L.rmw(ins)
        op = "add" if ins.mnemonic == M.INC else "sub"
        res = L.op(f"{op} i{bits} {a}, 1")
        L.write(ins, 0, res, bits, addr)
    cf = L.getf("cf")
    if op == "add":
        L.flags_add(a, "1", res, bits)
    else:
        L.flags_sub(a, "1", res, bits)
    L.setf("cf", cf)


@sem(M.NEG)
def _neg(L, ins, b):
    if ins.has_lock_prefix:  # no atomicrmw for negation: compare-and-swap loop
        bits = L.op_bits(ins, 0)
        p = L.op(f"inttoptr i64 {L.ea(ins)} to ptr")
        loop, done = L.uniq("lockneg"), L.uniq("lockneg_done")
        L.e(f"br label %{loop}")
        L.label(loop)
        a = L.op(f"load atomic i{bits}, ptr {p} monotonic, align {bits // 8}")
        res = L.op(f"sub i{bits} 0, {a}")
        pair = L.op(f"cmpxchg ptr {p}, i{bits} {a}, i{bits} {res} seq_cst seq_cst, align {bits // 8}")
        ok = L.op(f"extractvalue {{ i{bits}, i1 }} {pair}, 1")
        L.e(f"br i1 {ok}, label %{done}, label %{loop}")
        L.label(done)
        L.flags_sub("0", a, res, bits)
        L.setf("cf", L.op(f"icmp ne i{bits} {a}, 0"))
        return
    a, bits, addr = L.rmw(ins)
    res = L.op(f"sub i{bits} 0, {a}")
    L.flags_sub("0", a, res, bits)
    L.setf("cf", L.op(f"icmp ne i{bits} {a}, 0"))
    L.write(ins, 0, res, bits, addr)


@sem(M.NOT)
def _not(L, ins, b):
    if ins.has_lock_prefix:
        bits = L.op_bits(ins, 0)
        p = L.op(f"inttoptr i64 {L.ea(ins)} to ptr")
        L.op(f"atomicrmw xor ptr {p}, i{bits} -1 seq_cst, align {bits // 8}")
        return
    a, bits, addr = L.rmw(ins)
    L.write(ins, 0, L.op(f"xor i{bits} {a}, -1"), bits, addr)


@sem(M.SHL, M.SAL, M.SHR, M.SAR)
def _shift(L, ins, b):
    a, bits, addr = L.rmw(ins)
    mask = 63 if bits == 64 else 31
    cnt = L.read(ins, 1, 8) if ins.op_count > 1 else "1"
    c8 = L.op(f"and i8 {cnt}, {mask}")
    c64 = L.op(f"zext i8 {c8} to i64")
    signed = ins.mnemonic == M.SAR
    w = L.resize(a, bits, 64, signed=signed) if bits < 64 else a
    if ins.mnemonic in (M.SHL, M.SAL):
        r64 = L.op(f"shl i64 {w}, {c64}")
    elif signed:
        r64 = L.op(f"ashr i64 {w}, {c64}")
    else:
        r64 = L.op(f"lshr i64 {w}, {c64}")
    res = L.resize(r64, 64, bits)
    # CF: the last bit shifted out
    cm1 = L.op(f"sub i64 {c64}, 1")
    cm1 = L.op(f"and i64 {cm1}, 63")
    if ins.mnemonic in (M.SHL, M.SAL):
        if bits == 64:
            t = L.op(f"shl i64 {w}, {cm1}")
            cfv = L.op(f"icmp slt i64 {t}, 0")
        else:
            t = L.op(f"shl i64 {w}, {cm1}")
            t2 = L.op(f"lshr i64 {t}, {bits - 1}")
            cfv = L.op(f"trunc i64 {t2} to i1")
        sign = L.op(f"icmp slt i{bits} {res}, 0")
        ofv = L.op(f"xor i1 {sign}, {cfv}")
    else:
        t = L.op(f"{'ashr' if signed else 'lshr'} i64 {w}, {cm1}")
        cfv = L.op(f"trunc i64 {t} to i1")
        ofv = "false" if signed else L.op(f"icmp slt i{bits} {a}, 0")
    zero = L.op(f"icmp eq i8 {c8}, 0")
    for name, v in (("cf", cfv), ("of", ofv)):
        L.setf(name, L.op(f"select i1 {zero}, i1 {L.getf(name)}, i1 {v}"))
    old = {f: L.getf(f) for f in ("zf", "sf", "pf")}
    L.flags_zsp(res, bits)
    for f, v in old.items():
        L.setf(f, L.op(f"select i1 {zero}, i1 {v}, i1 {L.getf(f)}"))
    L.write(ins, 0, res, bits, addr)


@sem(M.ROL, M.ROR)
def _rot(L, ins, b):
    a, bits, addr = L.rmw(ins)
    mask = 63 if bits == 64 else 31
    cnt = L.read(ins, 1, 8) if ins.op_count > 1 else "1"
    c8 = L.op(f"and i8 {cnt}, {mask}")
    c = L.resize(c8, 8, bits) if bits != 8 else c8
    fn = "fshl" if ins.mnemonic == M.ROL else "fshr"
    res = L.op(f"call i{bits} @llvm.{fn}.i{bits}(i{bits} {a}, i{bits} {a}, i{bits} {c})")
    L.mod.intrinsics.add(f"declare i{bits} @llvm.{fn}.i{bits}(i{bits}, i{bits}, i{bits})")
    if ins.mnemonic == M.ROL:
        cfv = L.op(f"trunc i{bits} {res} to i1")
    else:
        cfv = L.op(f"icmp slt i{bits} {res}, 0")
    msb = L.op(f"icmp slt i{bits} {res}, 0")
    if ins.mnemonic == M.ROL:
        ofv = L.op(f"xor i1 {msb}, {cfv}")
    else:  # ROR: the two most significant bits differ
        sh = L.op(f"shl i{bits} {res}, 1")
        ofv = L.op(f"xor i1 {msb}, {L.op(f'icmp slt i{bits} {sh}, 0')}")
    zero = L.op(f"icmp eq i8 {c8}, 0")
    L.setf("cf", L.op(f"select i1 {zero}, i1 {L.getf('cf')}, i1 {cfv}"))
    L.setf("of", L.op(f"select i1 {zero}, i1 {L.getf('of')}, i1 {ofv}"))
    L.write(ins, 0, res, bits, addr)


@sem(M.SHLD, M.SHRD)
def _shxd(L, ins, b):
    a, bits, addr = L.rmw(ins)
    src = L.read(ins, 1, bits)
    mask = 63 if bits == 64 else 31
    cnt = L.read(ins, 2, 8)
    c8 = L.op(f"and i8 {cnt}, {mask}")
    if bits == 16:
        raise Unsupported("16-bit shld/shrd")
    c = L.resize(c8, 8, bits)
    if ins.mnemonic == M.SHLD:
        res = L.op(f"call i{bits} @llvm.fshl.i{bits}(i{bits} {a}, i{bits} {src}, i{bits} {c})")
        L.mod.intrinsics.add(f"declare i{bits} @llvm.fshl.i{bits}(i{bits}, i{bits}, i{bits})")
    else:
        res = L.op(f"call i{bits} @llvm.fshr.i{bits}(i{bits} {src}, i{bits} {a}, i{bits} {c})")
        L.mod.intrinsics.add(f"declare i{bits} @llvm.fshr.i{bits}(i{bits}, i{bits}, i{bits})")
    zero = L.op(f"icmp eq i8 {c8}, 0")
    # CF: the last bit shifted out of the destination (bit bits-c for shld, bit c-1 for shrd)
    c1 = L.op(f"select i1 {zero}, i{bits} 1, i{bits} {c}")  # keeps the shift amounts in range
    if ins.mnemonic == M.SHLD:
        amt = L.op(f"sub i{bits} {bits}, {c1}")
    else:
        amt = L.op(f"sub i{bits} {c1}, 1")
    t = L.op(f"lshr i{bits} {a}, {amt}")
    cfv = L.op(f"trunc i{bits} {t} to i1")
    ofv = L.op(f"xor i1 {L.op(f'icmp slt i{bits} {res}, 0')}, {L.op(f'icmp slt i{bits} {a}, 0')}")
    new = {"cf": cfv, "of": ofv}
    old = {f: L.getf(f) for f in ("cf", "of", "zf", "sf", "pf")}
    L.flags_zsp(res, bits)
    for f in ("zf", "sf", "pf"):
        new[f] = L.getf(f)
    for f, v in new.items():  # count 0: no flag changes
        L.setf(f, L.op(f"select i1 {zero}, i1 {old[f]}, i1 {v}"))
    L.write(ins, 0, res, bits, addr)


@sem(M.IMUL)
def _imul(L, ins, b):
    if ins.op_count == 1:
        return _mul1(L, ins, True)
    bits = L.op_bits(ins, 0)
    if ins.op_count == 2:
        a = L.read(ins, 0)
        c = L.read(ins, 1, bits)
    else:
        a = L.read(ins, 1, bits)
        c = L.read(ins, 2, bits)
    w = bits * 2
    full = L.op(f"mul i{w} {L.resize(a, bits, w, True)}, {L.resize(c, bits, w, True)}")
    res = L.resize(full, w, bits)
    back = L.resize(res, bits, w, True)
    ov = L.op(f"icmp ne i{w} {back}, {full}")
    L.setf("cf", ov)
    L.setf("of", ov)
    L.flags_zsp(res, bits)
    L.write(ins, 0, res, bits)


@sem(M.MUL)
def _mul(L, ins, b):
    return _mul1(L, ins, False)


def _mul1(L, ins, signed):
    bits = L.op_bits(ins, 0)
    src = L.read(ins, 0)
    w = bits * 2
    acc_reg = {8: R.AL, 16: R.AX, 32: R.EAX, 64: R.RAX}[bits]
    a, _ = L.read_reg(acc_reg)
    full = L.op(f"mul i{w} {L.resize(a, bits, w, signed)}, {L.resize(src, bits, w, signed)}")
    lo = L.resize(full, w, bits)
    hs = L.op(f"lshr i{w} {full}, {bits}")
    hi = L.resize(hs, w, bits)
    if bits == 8:
        L.write_reg(R.AX, L.resize(full, 16, 16), 16)
    else:
        L.write_reg(acc_reg, lo, bits)
        L.write_reg({16: R.DX, 32: R.EDX, 64: R.RDX}[bits], hi, bits)
    if signed:
        back = L.resize(lo, bits, w, True)
        ov = L.op(f"icmp ne i{w} {back}, {full}")
    else:
        ov = L.op(f"icmp ne i{bits} {hi}, 0")
    L.setf("cf", ov)
    L.setf("of", ov)
    L.flags_zsp(lo, bits)


@sem(M.DIV, M.IDIV)
def _div(L, ins, b):
    signed = ins.mnemonic == M.IDIV
    bits = L.op_bits(ins, 0)
    d = L.read(ins, 0)
    w = bits * 2
    if bits == 8:
        ax, _ = L.read_reg(R.AX)
        num = ax
    else:
        lo, _ = L.read_reg({16: R.AX, 32: R.EAX, 64: R.RAX}[bits])
        hi, _ = L.read_reg({16: R.DX, 32: R.EDX, 64: R.RDX}[bits])
        zl = L.resize(lo, bits, w)
        zh = L.resize(hi, bits, w)
        sh = L.op(f"shl i{w} {zh}, {bits}")
        num = L.op(f"or i{w} {sh}, {zl}")
    dw = L.resize(d, bits, w, signed)
    if signed:  # INT_MIN / -1 at the wide width is UB in IR: divide by 1 instead (overflows -> #DE)
        m1 = L.op(f"icmp eq i{w} {dw}, -1")
        mn = L.op(f"icmp eq i{w} {num}, {sconst(1 << (w - 1), w)}")
        both = L.op(f"and i1 {m1}, {mn}")
        dw = L.op(f"select i1 {both}, i{w} 1, i{w} {dw}")
    # #DE on zero divisor or quotient overflow
    zero = L.op(f"icmp eq i{bits} {d}, 0")
    ok_lbl = L.uniq(f"div_ok_{ins.ip:x}")
    bad_lbl = L.uniq(f"div_bad_{ins.ip:x}")
    L.e(f"br i1 {zero}, label %{bad_lbl}, label %{ok_lbl}_0")
    L.label(f"{ok_lbl}_0")
    q = L.op(f"{'sdiv' if signed else 'udiv'} i{w} {num}, {dw}")
    r = L.op(f"{'srem' if signed else 'urem'} i{w} {num}, {dw}")
    qt = L.resize(q, w, bits)
    back = L.resize(qt, bits, w, signed)
    ovf = L.op(f"icmp ne i{w} {back}, {q}")
    L.e(f"br i1 {ovf}, label %{bad_lbl}, label %{ok_lbl}")
    L.label(bad_lbl)
    L.trap(3)
    L.label(ok_lbl)
    rt = L.resize(r, w, bits)
    if bits == 8:
        z = L.op(f"zext i8 {rt} to i16")
        s = L.op(f"shl i16 {z}, 8")
        qz = L.op(f"zext i8 {qt} to i16")
        L.write_reg(R.AX, L.op(f"or i16 {s}, {qz}"), 16)
    else:
        L.write_reg({16: R.AX, 32: R.EAX, 64: R.RAX}[bits], qt, bits)
        L.write_reg({16: R.DX, 32: R.EDX, 64: R.RDX}[bits], rt, bits)


@sem(M.CBW, M.CWDE, M.CDQE)
def _cbw(L, ins, b):
    frm, to = {M.CBW: (R.AL, R.AX), M.CWDE: (R.AX, R.EAX), M.CDQE: (R.EAX, R.RAX)}[ins.mnemonic]
    v, fb = L.read_reg(frm)
    tb = fb * 2
    L.write_reg(to, L.resize(v, fb, tb, True), tb)


@sem(M.CWD, M.CDQ, M.CQO)
def _cwd(L, ins, b):
    a, d, bits = {M.CWD: (R.AX, R.DX, 16), M.CDQ: (R.EAX, R.EDX, 32), M.CQO: (R.RAX, R.RDX, 64)}[ins.mnemonic]
    v, _ = L.read_reg(a)
    L.write_reg(d, L.op(f"ashr i{bits} {v}, {bits - 1}"), bits)


@sem(M.SETO, M.SETNO, M.SETB, M.SETAE, M.SETE, M.SETNE, M.SETBE, M.SETA, M.SETS, M.SETNS,
     M.SETP, M.SETNP, M.SETL, M.SETGE, M.SETLE, M.SETG)
def _setcc(L, ins, b):
    c = L.cond(ins.condition_code)
    L.write(ins, 0, L.op(f"zext i1 {c} to i8"), 8)


@sem(M.CMOVO, M.CMOVNO, M.CMOVB, M.CMOVAE, M.CMOVE, M.CMOVNE, M.CMOVBE, M.CMOVA, M.CMOVS, M.CMOVNS,
     M.CMOVP, M.CMOVNP, M.CMOVL, M.CMOVGE, M.CMOVLE, M.CMOVG)
def _cmov(L, ins, b):
    bits = L.op_bits(ins, 0)
    c = L.cond(ins.condition_code)
    a = L.read(ins, 0)
    s = L.read(ins, 1, bits)  # the source is read (and may fault) whatever the condition
    L.write(ins, 0, L.op(f"select i1 {c}, i{bits} {s}, i{bits} {a}"), bits)  # 32-bit: zero-extends


@sem(M.BSWAP)
def _bswap(L, ins, b):
    bits = L.op_bits(ins, 0)
    v = L.read(ins, 0)
    L.mod.intrinsics.add(f"declare i{bits} @llvm.bswap.i{bits}(i{bits})")
    L.write(ins, 0, L.op(f"call i{bits} @llvm.bswap.i{bits}(i{bits} {v})"), bits)


@sem(M.POPCNT, M.LZCNT, M.TZCNT)
def _bitcount(L, ins, b):
    bits = L.op_bits(ins, 0)
    v = L.read(ins, 1, bits)
    name = {M.POPCNT: "ctpop", M.LZCNT: "ctlz", M.TZCNT: "cttz"}[ins.mnemonic]
    if name == "ctpop":
        L.mod.intrinsics.add(f"declare i{bits} @llvm.ctpop.i{bits}(i{bits})")
        res = L.op(f"call i{bits} @llvm.ctpop.i{bits}(i{bits} {v})")
    else:
        L.mod.intrinsics.add(f"declare i{bits} @llvm.{name}.i{bits}(i{bits}, i1)")
        res = L.op(f"call i{bits} @llvm.{name}.i{bits}(i{bits} {v}, i1 false)")
    zero = L.op(f"icmp eq i{bits} {v}, 0")
    if name == "ctpop":  # OF SF ZF AF CF PF: ZF = (source == 0), the rest cleared
        L.flags_logic(res, bits)
        L.setf("zf", zero)
        L.setf("sf", "false")
        L.setf("pf", "false")
    else:
        L.setf("cf", zero)
        L.setf("zf", L.op(f"icmp eq i{bits} {res}, 0"))
    L.write(ins, 0, res, bits)


@sem(M.BSF, M.BSR)
def _bsf(L, ins, b):
    bits = L.op_bits(ins, 0)
    v = L.read(ins, 1, bits)
    old = L.read(ins, 0)
    if ins.mnemonic == M.BSF:
        L.mod.intrinsics.add(f"declare i{bits} @llvm.cttz.i{bits}(i{bits}, i1)")
        r = L.op(f"call i{bits} @llvm.cttz.i{bits}(i{bits} {v}, i1 false)")
    else:
        L.mod.intrinsics.add(f"declare i{bits} @llvm.ctlz.i{bits}(i{bits}, i1)")
        lz = L.op(f"call i{bits} @llvm.ctlz.i{bits}(i{bits} {v}, i1 false)")
        r = L.op(f"sub i{bits} {bits - 1}, {lz}")
    zero = L.op(f"icmp eq i{bits} {v}, 0")
    L.setf("zf", zero)
    if bits == 32 and ins.op_kind(0) == OK.REGISTER:  # no zero-extension when the dest is kept
        i = GPR_INDEX[X.RegisterInfo(ins.op_register(0)).full_register]
        old64 = L.get64(i)
        L.set64(i, L.op(f"select i1 {zero}, i64 {old64}, i64 {L.op(f'zext i32 {r} to i64')}"))
        return
    L.write(ins, 0, L.op(f"select i1 {zero}, i{bits} {old}, i{bits} {r}"), bits)  # dest kept (AMD/Intel)


@sem(M.BT, M.BTS, M.BTR, M.BTC)
def _bt(L, ins, b):
    bits = L.op_bits(ins, 0)
    mem = ins.op_kind(0) == OK.MEMORY
    i = L.read(ins, 1, bits)
    addr = None
    if mem:
        addr = L.ea(ins)
        if ins.op_kind(1) == OK.REGISTER:  # bit string: a signed bit offset from the operand
            i64 = L.resize(i, bits, 64, signed=True) if bits < 64 else i
            words = L.op(f"ashr i64 {i64}, {bits.bit_length() - 1}")
            addr = L.op(f"add i64 {addr}, {L.op(f'mul i64 {words}, {bits // 8}')}")
    m = L.op(f"and i{bits} {i}, {bits - 1}")
    one = L.op(f"shl i{bits} 1, {m}")
    mn = ins.mnemonic
    if mem and ins.has_lock_prefix and mn != M.BT:
        p = L.op(f"inttoptr i64 {addr} to ptr")
        if mn == M.BTS:
            old = L.op(f"atomicrmw or ptr {p}, i{bits} {one} seq_cst, align {bits // 8}")
        elif mn == M.BTR:
            old = L.op(f"atomicrmw and ptr {p}, i{bits} {L.op(f'xor i{bits} {one}, -1')} seq_cst, align {bits // 8}")
        else:
            old = L.op(f"atomicrmw xor ptr {p}, i{bits} {one} seq_cst, align {bits // 8}")
        L.setf("cf", L.op(f"icmp ne i{bits} {L.op(f'and i{bits} {old}, {one}')}, 0"))
        return
    a = L.load(addr, bits, L.stack_relative(ins)) if mem else L.read(ins, 0)
    L.setf("cf", L.op(f"icmp ne i{bits} {L.op(f'and i{bits} {a}, {one}')}, 0"))
    if mn == M.BT:
        return
    if mn == M.BTS:
        res = L.op(f"or i{bits} {a}, {one}")
    elif mn == M.BTR:
        res = L.op(f"and i{bits} {a}, {L.op(f'xor i{bits} {one}, -1')}")
    else:
        res = L.op(f"xor i{bits} {a}, {one}")
    if mem:
        L.store(addr, res, bits, L.stack_relative(ins))
    else:
        L.write(ins, 0, res, bits)


@sem(M.CMPXCHG)
def _cmpxchg(L, ins, b):
    bits = L.op_bits(ins, 0)
    acc = {8: R.AL, 16: R.AX, 32: R.EAX, 64: R.RAX}[bits]
    expected, _ = L.read_reg(acc)
    new = L.read(ins, 1, bits)
    if ins.op_kind(0) == OK.MEMORY:
        p = L.op(f"inttoptr i64 {L.ea(ins)} to ptr")
        pair = L.op(f"cmpxchg ptr {p}, i{bits} {expected}, i{bits} {new} seq_cst seq_cst, align {bits // 8}")
        old = L.op(f"extractvalue {{ i{bits}, i1 }} {pair}, 0")
    else:
        old = L.read(ins, 0)
        eq = L.op(f"icmp eq i{bits} {old}, {expected}")
        L.write(ins, 0, L.op(f"select i1 {eq}, i{bits} {new}, i{bits} {old}"), bits)
    res = L.op(f"sub i{bits} {expected}, {old}")
    L.flags_sub(expected, old, res, bits)
    eq = L.op(f"icmp eq i{bits} {old}, {expected}")
    # the accumulator is written only on failure; for r/m32 a write would zero bits 32..63
    if bits == 32:
        rax = L.get64(RAX)
        L.set64(RAX, L.op(f"select i1 {eq}, i64 {rax}, i64 {L.op(f'zext i32 {old} to i64')}"))
    else:
        L.write_reg(acc, L.op(f"select i1 {eq}, i{bits} {expected}, i{bits} {old}"), bits)


@sem(M.XADD)
def _xadd(L, ins, b):
    bits = L.op_bits(ins, 0)
    src = L.read(ins, 1, bits)
    if ins.op_kind(0) == OK.MEMORY:
        p = L.op(f"inttoptr i64 {L.ea(ins)} to ptr")
        old = L.op(f"atomicrmw add ptr {p}, i{bits} {src} seq_cst, align {bits // 8}")
        res = L.op(f"add i{bits} {old}, {src}")
    else:
        old = L.read(ins, 0)
        res = L.op(f"add i{bits} {old}, {src}")
        L.flags_add(old, src, res, bits)
        L.write(ins, 1, old, bits)
        L.write(ins, 0, res, bits)
        return
    L.flags_add(old, src, res, bits)
    L.write(ins, 1, old, bits)


@sem(M.PUSH)
def _push(L, ins, b):
    if ins.stack_pointer_increment != -8:
        raise Unsupported("16-bit push")
    L.push(L.read(ins, 0, 64))


@sem(M.POP)
def _pop(L, ins, b):
    if ins.op_kind(0) == OK.MEMORY:
        raise Unsupported("pop mem")
    L.write(ins, 0, L.pop(), 64)


@sem(M.LEAVE)
def _leave(L, ins, b):
    L.set64(RSP, L.get64(RBP))
    L.set64(RBP, L.pop())


@sem(M.JMP)
def _jmp(L, ins, b):
    if ins.op_kind(0) in (OK.NEAR_BRANCH64, OK.NEAR_BRANCH32, OK.NEAR_BRANCH16):
        L.e(f"br label {L.target_label(ins.near_branch_target)}")
        return True
    target = L.read(ins, 0, 64)
    if b.table is not None:
        cases = " ".join(f"i64 {sconst(t, 64)}, label {L.target_label(t)}" for t in sorted(set(b.table.targets)))
        link = L.op(f"sub i64 {target}, %slide")
        dflt = L.uniq(f"jt_miss_{ins.ip:x}")
        L.e(f"switch i64 {link}, label %{dflt} [ {cases} ]")
        L.label(dflt)
    L.exit_dynamic(target)
    return True


@sem(M.JO, M.JNO, M.JB, M.JAE, M.JE, M.JNE, M.JBE, M.JA, M.JS, M.JNS, M.JP, M.JNP,
     M.JL, M.JGE, M.JLE, M.JG)
def _jcc(L, ins, b):
    c = L.cond(ins.condition_code)
    L.e(f"br i1 {c}, label {L.target_label(ins.near_branch_target)}, label {L.target_label(ins.next_ip)}")
    return True


@sem(M.CALL)
def _call(L, ins, b):
    direct = ins.op_kind(0) in (OK.NEAR_BRANCH64, OK.NEAR_BRANCH32)
    target = None if direct else L.read(ins, 0, 64)  # read before the push changes rsp
    L.push(L.code_addr(ins.next_ip))
    L.spill()
    if direct:
        L.call_target(ins.near_branch_target)
    else:
        L.e(f"call void @__recomp_dispatch(ptr %st, i64 {target})")
    if direct and L.mod.is_noreturn(ins.near_branch_target):
        L.trap(6)  # a function that never returns did
        return True
    L.reload()
    if ins.next_ip not in L.cfg.blocks and not any(i.ip == ins.next_ip for i in b.insns):
        # call at the end of the recovered code (noreturn callee, or falls into another function)
        L.e(f"br label {L.target_label(ins.next_ip)}")
        return True


@sem(M.RET)
def _ret(L, ins, b):
    sp = L.get64(RSP)
    extra = ins.immediate(0) if ins.op_count else 0
    L.set64(RSP, L.op(f"add i64 {sp}, {8 + extra}"))
    L.spill("ret")
    L.e("ret void")
    return True


def _rep_loop(L, ins, body):
    """rep prefix: while (rcx) { body; rcx--; } (DF = 0 assumed, as the ABI requires)."""
    if not ins.has_rep_prefix:
        body()
        return
    head, loop, done = (L.uniq(f"rep_{k}_{ins.ip:x}") for k in "hbd")
    L.e(f"br label %{head}")
    L.label(head)
    c = L.get64(RCX)
    z = L.op(f"icmp eq i64 {c}, 0")
    L.e(f"br i1 {z}, label %{done}, label %{loop}")
    L.label(loop)
    body()
    c2 = L.get64(RCX)
    L.set64(RCX, L.op(f"sub i64 {c2}, 1"))
    L.e(f"br label %{head}")
    L.label(done)


STR_BITS = {M.MOVSB: 8, M.MOVSW: 16, M.MOVSD: 32, M.MOVSQ: 64, M.STOSB: 8, M.STOSW: 16, M.STOSD: 32, M.STOSQ: 64}


@sem(M.MOVSB, M.MOVSW, M.MOVSQ, M.STOSB, M.STOSW, M.STOSD, M.STOSQ)
def _string(L, ins, b):
    bits = STR_BITS[ins.mnemonic]
    store_only = ins.mnemonic in (M.STOSB, M.STOSW, M.STOSD, M.STOSQ)

    def body():
        di = L.get64(RDI)
        if store_only:
            v, _ = L.read_reg({8: R.AL, 16: R.AX, 32: R.EAX, 64: R.RAX}[bits])
        else:
            si = L.get64(RSI)
            v = L.load(si, bits)
            L.set64(RSI, L.op(f"add i64 {si}, {bits // 8}"))
        L.store(di, v, bits)
        L.set64(RDI, L.op(f"add i64 {di}, {bits // 8}"))
    _rep_loop(L, ins, body)


# ---- SSE / AVX: moves and bitwise ops (enough for compiler-generated copies and zeroing) ----

def _vec_src(L, ins, i, bits):
    """Operand i as an integer of `bits` (register lanes truncated, memory loaded at that size)."""
    k = ins.op_kind(i)
    if k == OK.MEMORY:
        return L.load(L.ea(ins), bits, L.stack_relative(ins))
    v, b = L.read_reg(ins.op_register(i))
    return L.resize(v, b, bits) if b != bits else v


@sem(M.MOVAPS, M.MOVUPS, M.MOVAPD, M.MOVUPD, M.MOVDQA, M.MOVDQU, M.VMOVAPS, M.VMOVUPS, M.VMOVAPD,
     M.VMOVUPD, M.VMOVDQA, M.VMOVDQU, M.LDDQU, M.VLDDQU)
def _vmov(L, ins, b):
    bits = L.op_bits(ins, 0)
    L.write(ins, 0, _vec_src(L, ins, 1, bits), bits)


@sem(M.MOVD, M.MOVQ, M.VMOVD, M.VMOVQ)
def _movdq(L, ins, b):
    dbits = L.op_bits(ins, 0)
    sbits = L.op_bits(ins, 1)
    n = 32 if ins.mnemonic in (M.MOVD, M.VMOVD) else 64
    v = _vec_src(L, ins, 1, n) if sbits >= n else L.read(ins, 1, n)
    if ins.op_kind(0) == OK.REGISTER and xmm_index(ins.op_register(0))[0] is not None:
        idx, _ = xmm_index(ins.op_register(0))
        L.write_vec(idx, L.resize(v, n, 128), 128, 128)  # zero-extends to 128 bits
    else:
        L.write(ins, 0, v, n if dbits is None else min(n, dbits))


def _scalar_move(L, ins, n):
    """movss/movsd: load zeroes the rest; reg-reg keeps the destination's upper lanes; store writes n bits."""
    k0, k1 = ins.op_kind(0), ins.op_kind(1)
    if k0 == OK.MEMORY:
        v = _vec_src(L, ins, 1, n)
        L.write(ins, 0, v, n)
        return
    idx, _ = xmm_index(ins.op_register(0))
    if k1 == OK.MEMORY and ins.op_count == 2:
        v = L.load(L.ea(ins), n, L.stack_relative(ins))
        L.write_vec(idx, L.resize(v, n, 128), 128, 128)
        return
    # register form: dst = (upper of op1 or dst) | low n bits of src
    if ins.op_count == 3:
        upper_src, low_src = 1, 2
    else:
        upper_src, low_src = 0, 1
    up = _vec_src(L, ins, upper_src, 128)
    lo = _vec_src(L, ins, low_src, n)
    mask = L.op(f"and i128 {up}, {sconst(~((1 << n) - 1), 128)}")
    L.write_vec(idx, L.op(f"or i128 {mask}, {L.resize(lo, n, 128)}"), 128, 128)


@sem(M.MOVSS, M.VMOVSS)
def _movss(L, ins, b):
    _scalar_move(L, ins, 32)


@sem(M.MOVSD, M.VMOVSD)
def _movsd(L, ins, b):
    if ins.is_string_instruction:  # movsd (string): same mnemonic as the SSE move
        return _string_movsd(L, ins, b)
    _scalar_move(L, ins, 64)


def _string_movsd(L, ins, b):
    def body():
        di, si = L.get64(RDI), L.get64(RSI)
        L.store(di, L.load(si, 32), 32)
        L.set64(RSI, L.op(f"add i64 {si}, 4"))
        L.set64(RDI, L.op(f"add i64 {di}, 4"))
    _rep_loop(L, ins, body)


VBIT = {M.PXOR: "xor", M.XORPS: "xor", M.XORPD: "xor", M.VPXOR: "xor", M.VXORPS: "xor", M.VXORPD: "xor",
        M.PAND: "and", M.ANDPS: "and", M.ANDPD: "and", M.VPAND: "and", M.VANDPS: "and", M.VANDPD: "and",
        M.POR: "or", M.ORPS: "or", M.ORPD: "or", M.VPOR: "or", M.VORPS: "or", M.VORPD: "or",
        M.PANDN: "andn", M.ANDNPS: "andn", M.ANDNPD: "andn", M.VPANDN: "andn", M.VANDNPS: "andn",
        M.VANDNPD: "andn"}


@sem(*VBIT)
def _vbit(L, ins, b):
    bits = L.op_bits(ins, 0)
    kind = VBIT[ins.mnemonic]
    a_i, b_i = (1, 2) if ins.op_count == 3 else (0, 1)
    if kind == "xor" and ins.op_kind(a_i) == OK.REGISTER and ins.op_kind(b_i) == OK.REGISTER \
            and ins.op_register(a_i) == ins.op_register(b_i):
        L.write(ins, 0, "0", bits)
        return
    x = _vec_src(L, ins, a_i, bits)
    y = _vec_src(L, ins, b_i, bits)
    if kind == "andn":
        nx = L.op(f"xor i{bits} {x}, -1")
        r = L.op(f"and i{bits} {nx}, {y}")
    else:
        r = L.op(f"{kind} i{bits} {x}, {y}")
    L.write(ins, 0, r, bits)


@sem(M.VZEROUPPER)
def _vzu(L, ins, b):
    for i in range(16):
        L.e(f"store i128 0, ptr %h{i}")


class ModuleLifter:
    handlers = H

    def __init__(self, image, tso=False, abi=False, local_frames=True, rcpc=False, volatile=False):
        from . import simd  # noqa: F401  (registers the SSE/AVX handlers)
        self.image = image
        self.tso = tso
        # volatile: guest memory accesses (stack ones aside) as LLVM volatile loads and stores, for
        # x86-64 targets running next to the original code (hybrid execution): the hardware keeps
        # x86 ordering, and LLVM may no longer drop, merge or hoist them out of loops (a wait on a
        # flag another thread sets kept reading a stale register: the game hung).
        self.volatile = volatile
        self.abi = abi
        self.local_frames = local_frames
        self.rcpc = rcpc
        self.unsupported = {}
        self.unresolved = set()
        self.intrinsics = {"declare i8 @llvm.ctpop.i8(i8)"}
        self.lifted_names = set()
        self.cfgs = {}
        self._stubs = {}

    def import_stub(self, addr):
        """The GOT slot (link address) if addr starts with `jmp qword [rip+slot]`, else None."""
        if addr not in self._stubs:
            data = self.image.code_window(addr, 16)
            got = None
            if data:
                i = X.Decoder(64, data, ip=addr).decode()
                if i.mnemonic == M.JMP and i.op_count == 1 and i.op0_kind == OK.MEMORY \
                        and i.is_ip_rel_memory_operand and X.MemorySizeInfo(i.memory_size).size == 8:
                    got = i.ip_rel_memory_address
            self._stubs[addr] = got
        return self._stubs[addr]

    def is_noreturn(self, addr):
        if addr in getattr(self.image, "noreturn_funcs", ()):  # a linked module's own (exit, longjmp)
            return True
        got = self.import_stub(addr)
        return got is not None and got in getattr(self.image, "noreturn_got", ())

    def discover(self, roots=None):
        from . import cfg as cfgmod
        starts = set(self.image.functions)
        work = list(roots if roots is not None else sorted(starts))
        while work:
            a = work.pop()
            if a in self.cfgs:
                continue
            fn = self.image.functions.get(a)
            if fn is None:
                from .image import Function
                fn = Function(a, 0, f"sub_{a:x}")
                self.image.functions[a] = fn
                starts.add(a)
            c = cfgmod.recover(self.image, fn, starts, self.is_noreturn)
            self.cfgs[a] = c
            for t in c.call_targets | c.tail_targets:
                if self.image.is_code(t) and (t in starts or t in c.call_targets):
                    work.append(t)
        self.lifted_names = set(self.cfgs)

    def lift(self):
        bodies = []
        errors = []
        for a in sorted(self.cfgs):
            c = self.cfgs[a]
            if not c.blocks:
                errors.append((a, "no code"))
                self.lifted_names.discard(a)
        for a in sorted(self.cfgs):
            c = self.cfgs[a]
            if not c.blocks:
                continue
            errors += [(a, e) for e in c.errors]
            bodies.append(FunctionLifter(self, c).lift())
        names = sorted(self.lifted_names)
        table = ", ".join(f"{{ i64, ptr }} {{ i64 {sconst(a, 64)}, ptr {fname(a)} }}" for a in names)
        head = [
            "; generated by recompiler/recomp (x86-64 -> LLVM IR)",
            STATE_TYPE,
            "@__recomp_slide = external global i64",
            f"@__recomp_table = constant [{len(names)} x {{ i64, ptr }}] [{table}]",
            f"@__recomp_table_size = constant i64 {len(names)}",
            "declare void @__recomp_dispatch(ptr, i64)",
            "declare void @__recomp_trap(ptr, i64, i32)",
            "declare void @__recomp_dispatch_jump(ptr, i64)",
        ] + sorted(self.intrinsics)
        return "\n".join(head) + "\n\n" + "\n\n".join(bodies) + "\n", errors


# ---- BMI1 and MOVBE (both on the PS4's Jaguar cores) ----------------------------------------------

@sem(M.ANDN)
def _andn(L, ins, b):
    bits = L.op_bits(ins, 0)
    x = L.read(ins, 1, bits)
    y = L.read(ins, 2, bits)
    r = L.op(f"and i{bits} {L.op(f'xor i{bits} {x}, -1')}, {y}")
    L.flags_logic(r, bits)
    L.write(ins, 0, r, bits)


@sem(M.BEXTR)
def _bextr(L, ins, b):
    bits = L.op_bits(ins, 0)
    v = L.read(ins, 1, bits)
    ctl = L.read(ins, 2, bits)
    start = L.op(f"and i{bits} {ctl}, 255")
    s2 = L.op(f"lshr i{bits} {ctl}, 8")
    length = L.op(f"and i{bits} {s2}, 255")
    w = L.resize(v, bits, 128)
    st = L.resize(start, bits, 128)
    ln = L.resize(length, bits, 128)
    sh = L.op(f"lshr i128 {w}, {st}")           # start < 256: defined for i128 only below 128
    big_s = L.op(f"icmp uge i128 {st}, {bits}")
    sh = L.op(f"select i1 {big_s}, i128 0, i128 {sh}")
    big_l = L.op(f"icmp uge i128 {ln}, {bits}")
    ln2 = L.op(f"select i1 {big_l}, i128 {bits}, i128 {ln}")
    one = L.op(f"shl i128 1, {ln2}")
    mask = L.op(f"sub i128 {one}, 1")
    r = L.resize(L.op(f"and i128 {sh}, {mask}"), 128, bits)
    L.flags_logic(r, bits)
    L.write(ins, 0, r, bits)


@sem(M.BLSI, M.BLSMSK, M.BLSR)
def _bls(L, ins, b):
    bits = L.op_bits(ins, 0)
    v = L.read(ins, 1, bits)
    if ins.mnemonic == M.BLSI:
        r = L.op(f"and i{bits} {v}, {L.op(f'sub i{bits} 0, {v}')}")
        cf = L.op(f"icmp ne i{bits} {v}, 0")
    else:
        m1 = L.op(f"sub i{bits} {v}, 1")
        r = L.op(f"{'xor' if ins.mnemonic == M.BLSMSK else 'and'} i{bits} {v}, {m1}")
        cf = L.op(f"icmp eq i{bits} {v}, 0")
    L.flags_logic(r, bits)
    L.setf("cf", cf)
    L.write(ins, 0, r, bits)


@sem(M.MOVBE)
def _movbe(L, ins, b):
    bits = L.op_bits(ins, 0)
    v = L.read(ins, 1, bits)
    L.mod.intrinsics.add(f"declare i{bits} @llvm.bswap.i{bits}(i{bits})")
    L.write(ins, 0, L.op(f"call i{bits} @llvm.bswap.i{bits}(i{bits} {v})"), bits)


# ---- pushfq / popfq (RFLAGS from the lifted flags; IF and bit 1 read as set, DF as clear) -------

RFLAGS_BITS = (("cf", 0), ("pf", 2), ("af", 4), ("zf", 6), ("sf", 7), ("of", 11))


@sem(M.PUSHFQ)
def _pushfq(L, ins, b):
    v = "514"  # bit 1 (always set) | IF (bit 9)
    for f, bit in RFLAGS_BITS:
        z = L.op(f"zext i1 {L.getf(f)} to i64")
        v = L.op(f"or i64 {v}, {L.op(f'shl i64 {z}, {bit}')}")
    L.push(v)


@sem(M.POPFQ)
def _popfq(L, ins, b):
    v = L.pop()
    for f, bit in RFLAGS_BITS:
        t = L.op(f"lshr i64 {v}, {bit}")
        L.setf(f, L.op(f"trunc i64 {t} to i1"))


# ---- time stamp counter: the runtime supplies it (bbport: the PS4 TSC at sceKernelGetTscFrequency) ----

@sem(M.RDTSC, M.RDTSCP)
def _rdtsc(L, ins, b):
    L.mod.intrinsics.add("declare i64 @__recomp_rdtsc()")
    t = L.op("call i64 @__recomp_rdtsc()")
    L.write_reg(R.EAX, L.resize(t, 64, 32), 32)
    L.write_reg(R.EDX, L.resize(L.op(f"lshr i64 {t}, 32"), 64, 32), 32)
    if ins.mnemonic == M.RDTSCP:
        L.write_reg(R.ECX, "0", 32)  # IA32_TSC_AUX: processor id 0



# ---- MXCSR, cpuid, interrupts ---------------------------------------------------------------------

@sem(M.STMXCSR, M.VSTMXCSR)
def _stmxcsr(L, ins, b):
    p = L.op(f"getelementptr inbounds %State, ptr %st, i32 0, i32 {ST_MXCSR}")
    L.write(ins, 0, L.op(f"load i32, ptr {p}"), 32)


@sem(M.LDMXCSR, M.VLDMXCSR)
def _ldmxcsr(L, ins, b):
    """Kept in the state (and read back by stmxcsr). Rounding-mode and DAZ/FTZ changes are not
    applied to the recompiled float code yet (recompiler/README.md, known deviations)."""
    p = L.op(f"getelementptr inbounds %State, ptr %st, i32 0, i32 {ST_MXCSR}")
    L.e(f"store i32 {L.read(ins, 0, 32)}, ptr {p}")


@sem(M.CPUID)
def _cpuid(L, ins, b):
    """The runtime answers (bbport: the PS4's Jaguar)."""
    L.mod.intrinsics.add("declare void @__recomp_cpuid(i32, i32, ptr)")
    leaf, _ = L.read_reg(R.EAX)
    sub, _ = L.read_reg(R.ECX)
    buf = L.op("alloca [4 x i32]")
    L.e(f"call void @__recomp_cpuid(i32 {leaf}, i32 {sub}, ptr {buf})")
    for k, reg in enumerate((R.EAX, R.EBX, R.ECX, R.EDX)):
        p = L.op(f"getelementptr inbounds [4 x i32], ptr {buf}, i32 0, i32 {k}")
        L.write_reg(reg, L.op(f"load i32, ptr {p}"), 32)


@sem(M.INT, M.INT1, M.INTO, M.SYSCALL, M.HLT, M.CLI, M.STI, M.IN, M.OUT)
def _privileged(L, ins, b):
    """Faults when a PS4 game runs it natively too (int 41h: the SDK's debug hook on assert
    paths): a trap the runtime reports, kind 0x100 + vector for int n, 8 otherwise."""
    kind = 0x100 + (ins.immediate(0) & 0xff) if ins.mnemonic == M.INT else 8
    L.trap(kind)
    return True


@sem(M.STC, M.CLC, M.CMC)
def _carry(L, ins, b):
    if ins.mnemonic == M.STC:
        L.setf("cf", "true")
    elif ins.mnemonic == M.CLC:
        L.setf("cf", "false")
    else:
        L.setf("cf", L.op(f"xor i1 {L.getf('cf')}, true"))
