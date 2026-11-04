// SPDX-License-Identifier: GPL-2.0-or-later
// Flag-exact tests in assembly. Each t_fl_* function (same signature as int_basic.c: a=rdi,
// b=rsi, c=rdx, buf=rcx) runs one instruction form and returns its result with the defined flags
// folded in: rax ^= (rflags & MASK) << 52. MASK leaves out flags the SDM calls undefined for
// that instruction. Many cases first set the flags from the inputs (cmp), so "count 0: flags
// unaffected" paths are checked too.
    .intel_syntax noprefix
    .text

#define CF 0x1
#define PF 0x4
#define AF 0x10
#define ZF 0x40
#define SF 0x80
#define OF 0x800
#define ARITH (CF|PF|AF|ZF|SF|OF)
#define LOGIC (CF|PF|ZF|SF|OF)

.macro T name
    .globl t_fl_\name
    .type t_fl_\name, @function
    .p2align 4
t_fl_\name:
.endm

.macro FL name, mask
    pushfq
    pop r11
    and r11, \mask
    shl r11, 52
    xor rax, r11
    ret
    .size t_fl_\name, . - t_fl_\name
.endm

// ---- arithmetic ------------------------------------------------------------------------------
T add64
    mov rax, rdi
    add rax, rsi
FL add64, ARITH
T add32
    mov eax, edi
    add eax, esi
FL add32, ARITH
T add8
    mov rax, rdi
    add al, sil
FL add8, ARITH
T adc64
    bt rdx, 0
    mov rax, rdi
    adc rax, rsi
FL adc64, ARITH
T sbb32
    bt edx, 3
    mov eax, edi
    sbb eax, esi
FL sbb32, ARITH
T sbb_self
    cmp rdi, rsi
    sbb rax, rax
FL sbb_self, ARITH
T sub16
    mov rax, rdi
    sub ax, si
FL sub16, ARITH
T cmp_ah
    mov rax, rdi
    mov ecx, esi
    cmp ah, cl
    mov al, ah
FL cmp_ah, ARITH
T neg32
    mov rax, rdi
    neg eax
FL neg32, ARITH
T inc64
    cmp rdx, rsi
    mov rax, rdi
    inc rax
FL inc64, ARITH
T dec8
    cmp rdx, rsi
    mov rax, rdi
    dec al
FL dec8, ARITH
T and64
    mov rax, rdi
    and rax, rsi
FL and64, LOGIC
T xor32
    mov eax, edi
    xor eax, esi
FL xor32, LOGIC
T test16
    mov rax, rdi
    test ax, si
FL test16, LOGIC
T imul64
    mov rax, rdi
    imul rax, rsi
FL imul64, CF|OF
T imul32_imm
    imul eax, esi, 12345
FL imul32_imm, CF|OF
T imul8
    mov rax, rdi
    imul sil
FL imul8, CF|OF
T mul64
    mov rax, rdi
    mul rsi
    xor rax, rdx
FL mul64, CF|OF
T mul8
    mov rax, rdi
    mul sil
FL mul8, CF|OF

// ---- shifts and rotates ------------------------------------------------------------------------
T shl1
    mov rax, rdi
    shl rax, 1
FL shl1, LOGIC
T shl_cl
    cmp rdi, rdx
    mov ecx, esi
    mov rax, rdi
    shl rax, cl
FL shl_cl, CF|PF|ZF|SF
T shr32_cl
    cmp rdx, rdi
    mov ecx, esi
    mov eax, edi
    shr eax, cl
FL shr32_cl, CF|PF|ZF|SF
T sar8_cl
    cmp rsi, rdx
    mov ecx, esi
    and ecx, 7
    mov rax, rdi
    sar al, cl
FL sar8_cl, CF|PF|ZF|SF
T sar1
    mov rax, rdi
    sar eax, 1
FL sar1, LOGIC
T shr16_1
    mov rax, rdi
    shr ax, 1
FL shr16_1, LOGIC
T rol1
    cmp rdx, rsi
    mov rax, rdi
    rol rax, 1
FL rol1, CF|OF|ZF|SF|PF
T ror1
    cmp rdx, rsi
    mov rax, rdi
    ror eax, 1
FL ror1, CF|OF|ZF|SF|PF
T rol_cl
    cmp rdi, rsi
    mov ecx, edx
    mov rax, rdi
    rol rax, cl
FL rol_cl, CF|ZF|SF|PF
T ror8_cl
    cmp rdi, rsi
    mov ecx, edx
    mov rax, rdi
    ror al, cl
FL ror8_cl, CF|ZF|SF|PF
T shld1
    mov rax, rdi
    shld rax, rsi, 1
FL shld1, CF|PF|ZF|SF|OF
T shld_cl
    cmp rsi, rdi
    mov ecx, edx
    mov rax, rdi
    shld rax, rsi, cl
FL shld_cl, CF|PF|ZF|SF
T shrd32_cl
    cmp rdi, rsi
    mov ecx, edx
    mov eax, edi
    shrd eax, esi, cl
FL shrd32_cl, CF|PF|ZF|SF
T shrd1
    mov rax, rdi
    shrd rax, rsi, 1
FL shrd1, CF|PF|ZF|SF|OF

// ---- bit scans and counts ---------------------------------------------------------------------
T bsf64
    cmp rdi, rsi
    mov rax, rdx
    bsf rax, rdi
FL bsf64, ZF
T bsf32_keep
    mov rax, rdx
    bsf eax, edi
FL bsf32_keep, ZF
T bsr32
    mov rax, rdx
    bsr eax, esi
FL bsr32, ZF
T popcnt64
    cmp rdi, rsi
    popcnt rax, rdi
FL popcnt64, ARITH
T lzcnt32
    lzcnt eax, edi
FL lzcnt32, CF|ZF
T tzcnt64
    tzcnt rax, rsi
FL tzcnt64, CF|ZF

// ---- bit tests --------------------------------------------------------------------------------
T bt_reg
    mov rax, rdi
    bt rax, rsi
FL bt_reg, CF
T bts_imm
    mov rax, rdi
    bts eax, 17
FL bts_imm, CF
T btr_reg
    mov rax, rdi
    btr rax, rsi
FL btr_reg, CF
T btc16_reg
    mov rax, rdi
    btc ax, si
FL btc16_reg, CF
// bit strings: a signed bit offset from the operand address, within buf (-1024..1023 bits from buf+128)
T bt_string
    mov rax, rsi
    and rax, 0x7ff
    sub rax, 0x400
    bt dword ptr [rcx + 128], eax
FL bt_string, CF
T bts_string_lock
    mov rax, rsi
    and rax, 0x7ff
    sub rax, 0x400
    lock bts qword ptr [rcx + 128], rax
FL bts_string_lock, CF
T btr_string
    mov rax, rdi
    and rax, 0x7ff
    sub rax, 0x400
    btr word ptr [rcx + 128], ax
FL btr_string, CF
T btc_mem_lock_imm
    mov rax, rdi
    lock btc dword ptr [rcx + 8], 29
FL btc_mem_lock_imm, CF

// ---- atomics ----------------------------------------------------------------------------------
T cmpxchg32
    mov rax, rdi
    mov r9, rdx
    test dl, 1
    cmovnz r9, rdi
    mov dword ptr [rcx], r9d
    lock cmpxchg dword ptr [rcx], esi
FL cmpxchg32, ARITH
T cmpxchg64
    mov rax, rdi
    mov r9, rdx
    test dl, 2
    cmovnz r9, rdi
    mov qword ptr [rcx + 16], r9
    lock cmpxchg qword ptr [rcx + 16], rsi
FL cmpxchg64, ARITH
T xadd_mem
    mov rax, rdi
    lock xadd qword ptr [rcx], rax
FL xadd_mem, ARITH
T xadd_reg
    mov rax, rdi
    mov r8, rsi
    xadd r8, rax
    xor rax, r8
FL xadd_reg, ARITH
T lock_not
    lock not dword ptr [rcx + 4]
    mov eax, dword ptr [rcx + 4]
FL lock_not, 0
T lock_neg
    lock neg qword ptr [rcx + 24]
    mov rax, qword ptr [rcx + 24]
FL lock_neg, ARITH

// ---- BMI1 -------------------------------------------------------------------------------------
T bextr64
    bextr rax, rdi, rsi
FL bextr64, CF|ZF|OF
T andn32
    andn eax, edi, esi
FL andn32, CF|ZF|SF|OF
T blsi64
    blsi rax, rdi
FL blsi64, CF|ZF|SF|OF
T blsr32
    blsr eax, esi
FL blsr32, CF|ZF|SF|OF
T blsmsk64
    blsmsk rax, rdx
FL blsmsk64, CF|ZF|SF|OF

// ---- conditions, cmov, string ops ---------------------------------------------------------------
T setcc_all
    xor eax, eax
    cmp rdi, rsi
    .irp cc, o, no, b, ae, e, ne, be, a, s, ns, p, np, l, ge, le, g
    set\cc r8b
    shl rax, 1
    or al, r8b
    .endr
FL setcc_all, 0
T setcc_all8
    xor eax, eax
    cmp dil, sil
    .irp cc, o, no, b, ae, e, ne, be, a, s, ns, p, np, l, ge, le, g
    set\cc r8b
    shl rax, 1
    or al, r8b
    .endr
FL setcc_all8, 0
T cmov32_false
    mov rax, rdi
    cmp rsi, rdx
    cmovl eax, esi
FL cmov32_false, 0
T rep_stosb
    mov r8, rcx
    mov rdi, rcx
    mov al, dl
    mov rcx, rsi
    and rcx, 127
    rep stosb
    mov rax, rdi
    sub rax, r8
FL rep_stosb, 0
T rep_movsq
    mov r8, rsi
    lea rsi, [rcx + 128]
    mov rdi, rcx
    mov rcx, r8
    and rcx, 15
    rep movsq
    mov rax, rsi
    sub rax, rdi
FL rep_movsq, 0
T pushf_popf
    cmp rdi, rsi
    pushfq
    pop r8
    xor r8, rdx
    and r8, 0x8d5
    or r8, 2
    push r8
    popfq
    mov rax, rdi
    adc rax, rsi
FL pushf_popf, ARITH

// ---- carry instructions, newer vector forms, MXCSR -------------------------------------------
T carry_ops
    cmp rdi, rsi
    cmc
    mov rax, rdx
    adc rax, 0
    stc
    adc rax, rdi
    clc
    adc rax, rsi
FL carry_ops, ARITH
// finite positive doubles from the inputs: the mantissa, and an exponent from 9 input bits
// (2^-256 .. 2^255), so products and sums never overflow to inf or become NaN
.macro FINITE reg, tmp
    mov \tmp, \reg
    shr \tmp, 52
    and \tmp, 0x1ff
    add \tmp, 0x2ff
    shl \tmp, 52
    shl \reg, 12
    shr \reg, 12
    or \reg, \tmp
.endm
T dppd
    FINITE rdi, r8
    FINITE rsi, r8
    FINITE rdx, r8
    movq xmm0, rdi
    movq xmm1, rsi
    pinsrq xmm0, rdx, 1
    pinsrq xmm1, rdi, 1
    dppd xmm0, xmm1, 0x31
    movq rax, xmm0
FL dppd, 0
T vdppd
    FINITE rdi, r8
    FINITE rsi, r8
    FINITE rdx, r8
    vmovq xmm2, rdi
    vpinsrq xmm2, xmm2, rsi, 1
    vmovq xmm3, rdx
    vpinsrq xmm3, xmm3, rdi, 1
    vdppd xmm0, xmm2, xmm3, 0x33
    vpextrq rax, xmm0, 1
FL vdppd, 0
T pabs
    movq xmm0, rdi
    pinsrq xmm0, rsi, 1
    pabsb xmm1, xmm0
    pabsw xmm2, xmm0
    vpabsd xmm3, xmm0
    pxor xmm1, xmm2
    pxor xmm1, xmm3
    movq rax, xmm1
    pextrq rdx, xmm1, 1
    xor rax, rdx
FL pabs, 0
T addsubps
    mov r8, 0xbfffffffbfffffff  // bit 30 of each 32-bit lane clear: finite floats and doubles
    and rdi, r8
    and rsi, r8
    and rdx, r8
    movq xmm0, rdi
    pinsrq xmm0, rsi, 1
    movq xmm1, rdx
    pinsrq xmm1, rdi, 1
    addsubps xmm0, xmm1
    vaddsubpd xmm2, xmm1, xmm0
    pxor xmm0, xmm2
    movq rax, xmm0
FL addsubps, 0
T movnt
    movq xmm0, rdi
    pinsrq xmm0, rsi, 1
    movntdq [rcx + 64], xmm0
    movnti [rcx + 96], rdx
    movntdqa xmm1, [rcx + 64]
    movq rax, xmm1
    add rax, [rcx + 96]
FL movnt, 0
T maskmovdqu
    movq xmm0, rsi
    pinsrq xmm0, rdx, 1
    movq xmm1, rdi
    pinsrq xmm1, rsi, 1
    mov rdi, rcx
    add rdi, 32
    maskmovdqu xmm0, xmm1
    mov rax, [rcx + 32]
    xor rax, [rcx + 40]
FL maskmovdqu, 0
T mxcsr
    stmxcsr dword ptr [rcx]
    mov eax, dword ptr [rcx]
    mov r8d, eax
    xor r8d, 0x40
    mov dword ptr [rcx + 4], r8d
    ldmxcsr dword ptr [rcx + 4]
    vstmxcsr dword ptr [rcx + 8]
    ldmxcsr dword ptr [rcx]
    mov r9d, dword ptr [rcx + 8]
    shl rax, 32
    or rax, r9
    mov r9, 0xffffffc0ffffffc0  // control bits only: the sticky exception flags (bits 0-5) are not modelled
    and rax, r9
    mov dword ptr [rcx], 0  // nor written back to buf
    mov dword ptr [rcx + 4], 0
    mov dword ptr [rcx + 8], 0
FL mxcsr, 0

    .section .note.GNU-stack, "", @progbits
