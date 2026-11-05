// SPDX-License-Identifier: GPL-2.0-or-later
// Guest sites (gpu/shim/bbport_guest_sites.h): the instruction at each gsite_<kind>_<n> label is a
// site; the harness's handler (recomp_loader.c __recomp_guest_site) does that instruction's work
// itself and goes on past it, so the recompiled code (which calls the handler) and the original
// (which runs the instruction) agree. Covers the registers going to the handler and back, the
// flags both ways, the vector registers (a VEX.128 result clears bits 128-255) and the
// continuation past the site. run.sh passes the labels to the lifter (--sites). Signature as
// int_basic.c: a=rdi, b=rsi, c=rdx, buf=rcx (256 bytes).
    .intel_syntax noprefix
    .text

    .globl t_site_add
    .type t_site_add, @function
    .p2align 4
t_site_add:
    mov rax, rsi
    stc                             // a flag going in that the add replaces
    .globl gsite_add_1
gsite_add_1:
    add rax, rdi                    // 3 bytes: the site
    pushfq
    pop rdx
    and edx, 0x8d5                  // CF PF AF ZF SF OF after the add
    rol rdx, 48
    xor rax, rdx
    adc rax, 0                      // CF read again after the handler
    ret
    .size t_site_add, . - t_site_add

    .globl t_site_vec
    .type t_site_vec, @function
    .p2align 4
t_site_vec:
    vmovq xmm0, rdi
    vpinsrq xmm0, xmm0, rsi, 1
    vmovq xmm1, rdx
    vpinsrq xmm1, xmm1, rdi, 1
    vxorps ymm2, ymm2, ymm2
    vcmpps ymm2, ymm2, ymm2, 15     // all ones (AVX)
    vinsertf128 ymm0, ymm2, xmm0, 0 // ymm0: the inputs below, ones above
    .globl gsite_vec_1
gsite_vec_1:
    vpaddd xmm0, xmm0, xmm1         // 4 bytes: the site (bits 128-255 of ymm0 cleared)
    vextractf128 xmm3, ymm0, 1
    vmovq rax, xmm0
    vpextrq r8, xmm0, 1
    rol r8, 17
    xor rax, r8
    vmovq r8, xmm3
    xor rax, r8
    vpextrq r8, xmm3, 1
    xor rax, r8
    vzeroupper
    ret
    .size t_site_vec, . - t_site_vec

// A handler that goes on elsewhere in the function (as the port's patches that jump over code):
// at gsite_jmp_1, a short jmp; the handler returns its target.
    .globl t_site_jmp
    .type t_site_jmp, @function
    .p2align 4
t_site_jmp:
    mov rax, rdi
    .globl gsite_jmp_1
gsite_jmp_1:
    jmp 1f                          // 2 bytes: the site (a local label: no PLT)
    add rax, 7
    imul rax, rdx
    .globl gtarget_jmp_1            // the same place, for the handler (the bytes above may be patched)
gtarget_jmp_1:
1:  add rax, rsi
    ret
    .size t_site_jmp, . - t_site_jmp

// A variant (a byte patch the port switches): `add rax, rdi` at gvariant_sub_1 becomes `sub rax,
// rdi`. The runner writes the replacement into the code before the tests (the original code runs
// it; the recompiled code, lifted both ways, finds it there).
    .globl t_site_variant
    .type t_site_variant, @function
    .p2align 4
t_site_variant:
    mov rax, rsi
    xor rdx, rdx                    // past the first 5 bytes (the hybrid entry jump)
    .globl gvariant_sub_1
gvariant_sub_1:
    add rax, rdi                    // 3 bytes: 48 01 f8 -> 48 29 f8
    setc dl
    shl rdx, 40
    xor rax, rdx
    ret
    .size t_site_variant, . - t_site_variant

// A handler that returns from the function (as a hook at a function's entry that replaces it):
// at gsite_ret_1 it does the function's work (rax ^= 0x55) and returns to the caller.
    .globl t_site_ret
    .type t_site_ret, @function
    .p2align 4
t_site_ret:
    push rbx
    mov rbx, rsi                    // callee-saved across the call
    mov rax, rdi
    call site_ret_inner
    add rax, rbx
    pop rbx
    ret
    .size t_site_ret, . - t_site_ret

    .type site_ret_inner, @function     // local: a direct call, no PLT
    .p2align 4
site_ret_inner:
    .globl gsite_ret_1
gsite_ret_1:
    xor rax, 0x55                   // 4 bytes: the site; the handler returns past the whole function
    ret
    .size site_ret_inner, . - site_ret_inner

    .section .note.GNU-stack, "", @progbits
