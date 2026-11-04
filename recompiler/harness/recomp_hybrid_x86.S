// SPDX-License-Identifier: GPL-2.0-or-later
// Hybrid execution thunks (see recomp_hybrid.h). x86-64 only.
//
// RecompState (recomp_rt.h): gpr[16] at 0 (rax rcx rdx rbx rsp rbp rsi rdi r8..r15), rip 128,
// fs_base 136, gs_base 144, mxcsr 152, xmm[16] at 160, ymmh[16] at 416; 672 bytes.
// HybridThread: host_sp 0, depth 8, states 16, max_depth 24, native_frame 32, entries 40.
//
// No guest register is clobbered in either direction. Scratch space for the thunks is the guest's
// red zone below rsp, which belongs to the function being entered (or, after a return, to the
// caller, which cannot hold anything there across a call).
        .intel_syntax noprefix
        .text

#define ST_SIZE 672

        .macro SAVE_VEC base
        stmxcsr dword ptr [\base + 152]
        .irp i, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
        vmovdqu xmmword ptr [\base + 160 + 16 * \i], xmm\i
        vextractf128 xmmword ptr [\base + 416 + 16 * \i], ymm\i, 1
        .endr
        .endm

        .macro LOAD_VEC base
        ldmxcsr dword ptr [\base + 152]
        .irp i, 0, 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13, 14, 15
        vmovdqu xmm\i, xmmword ptr [\base + 160 + 16 * \i]
        vinsertf128 ymm\i, ymm\i, xmmword ptr [\base + 416 + 16 * \i], 1
        .endr
        .endm

        // reg := this thread's HybridThread*
        .macro SELF reg
        mov \reg, qword ptr [rip + recomp_hybrid_self@GOTTPOFF]
        mov \reg, qword ptr fs:[\reg]
        .endm

// Restores the guest registers from the state in rax and returns to the address at
// [state.rsp - 8], leaving rsp = state.rsp: after a recompiled function's `ret` (which popped its
// return address) and to enter a native target placed in that slot.
        .p2align 4
        .type recomp_hybrid_resume, @function
recomp_hybrid_resume:
        LOAD_VEC rax
        mov rcx, [rax + 8]
        mov rdx, [rax + 16]
        mov rbx, [rax + 24]
        mov rbp, [rax + 40]
        mov rsi, [rax + 48]
        mov rdi, [rax + 56]
        mov r8,  [rax + 64]
        mov r9,  [rax + 72]
        mov r10, [rax + 80]
        mov r11, [rax + 88]
        mov r12, [rax + 96]
        mov r13, [rax + 104]
        mov r14, [rax + 112]
        mov r15, [rax + 120]
        mov rsp, [rax + 32]
        sub rsp, 8
        mov rax, [rax]
        ret
        .size recomp_hybrid_resume, . - recomp_hybrid_resume

// Original code -> recompiled function. Reached from a function's stub with the guest's
// registers live, [rsp] = return address, [rsp - 0x18] = the recompiled function.
        .globl recomp_hybrid_enter
        .p2align 4
        .type recomp_hybrid_enter, @function
recomp_hybrid_enter:
        mov [rsp - 0x10], rax
        mov [rsp - 0x20], rcx
        SELF rax
        test rax, rax
        jz 9f                           // thread not initialised
        mov rcx, [rax + 8]
        cmp rcx, [rax + 24]
        jae 9f                          // nested too deep
        inc qword ptr [rax + 8]
        inc qword ptr [rax + 40]
        imul rcx, rcx, ST_SIZE
        add rcx, [rax + 16]             // rcx = state
        mov [rcx + 16], rdx
        mov [rcx + 24], rbx
        mov [rcx + 32], rsp
        mov [rcx + 40], rbp
        mov [rcx + 48], rsi
        mov [rcx + 56], rdi
        mov [rcx + 64], r8
        mov [rcx + 72], r9
        mov [rcx + 80], r10
        mov [rcx + 88], r11
        mov [rcx + 96], r12
        mov [rcx + 104], r13
        mov [rcx + 112], r14
        mov [rcx + 120], r15
        mov rdx, [rsp - 0x10]
        mov [rcx], rdx                  // rax
        mov rdx, [rsp - 0x20]
        mov [rcx + 8], rdx              // rcx
        mov rdx, [rsp]
        mov [rcx + 128], rdx            // rip: the return address (for reports)
        rdfsbase rdx
        rdgsbase rbx
        mov [rcx + 144], rbx
        mov r8, [rip + recomp_hybrid_fs_from_gs@GOTPCREL]
        cmp dword ptr [r8], 0
        cmovne rdx, rbx
        mov [rcx + 136], rdx
        SAVE_VEC rcx
        mov r11, [rsp - 0x18]           // the recompiled function
        mov rbx, rax                    // callee-saved across the call: thread, state
        mov r12, rcx
        mov rsp, [rax]                  // the host stack
        cld
        mov rdi, rcx
        call r11
        dec qword ptr [rbx + 8]
        mov rax, r12
        jmp recomp_hybrid_resume
9:      ud2
        .size recomp_hybrid_enter, . - recomp_hybrid_enter

// Recompiled code -> original code: void recomp_hybrid_call_native(RecompState *st, uint64_t target).
// The guest has pushed its return address at [st.rsp] (call) or holds its caller's there (tail
// call); the target's `ret` comes back here instead, and the slot is restored afterwards.
// Host frame (rsp after the pushes): [0] state, [8] previous native_frame, [16] the guest's
// return slot value, [24] previous host_sp.
        .globl recomp_hybrid_call_native
        .p2align 4
        .type recomp_hybrid_call_native, @function
recomp_hybrid_call_native:
        push rbp
        push rbx
        push r12
        push r13
        push r14
        push r15
        sub rsp, 40
        mov [rsp], rdi
        SELF rax
        mov rcx, [rax + 32]
        mov [rsp + 8], rcx
        mov rcx, [rax]
        mov [rsp + 24], rcx
        mov [rax + 32], rsp
        lea rcx, [rsp - 256]            // entries nested in the target continue below this frame
        and rcx, -16
        mov [rax], rcx
        mov rdx, [rdi + 32]             // guest rsp
        mov rcx, [rdx]
        mov [rsp + 16], rcx
        lea rcx, [rip + 1f]
        mov [rdx], rcx                  // the target returns to 1:
        mov [rdx - 8], rsi              // recomp_hybrid_resume "returns" into the target
        mov rax, rdi
        jmp recomp_hybrid_resume
1:      mov [rsp - 8], rax              // guest registers live, rsp = guest rsp after the ret
        SELF rax
        mov rax, [rax + 32]
        mov rax, [rax]                  // state
        mov [rax + 8], rcx
        mov [rax + 16], rdx
        mov [rax + 24], rbx
        mov [rax + 32], rsp
        mov [rax + 40], rbp
        mov [rax + 48], rsi
        mov [rax + 56], rdi
        mov [rax + 64], r8
        mov [rax + 72], r9
        mov [rax + 80], r10
        mov [rax + 88], r11
        mov [rax + 96], r12
        mov [rax + 104], r13
        mov [rax + 112], r14
        mov [rax + 120], r15
        mov rcx, [rsp - 8]
        mov [rax], rcx
        SAVE_VEC rax
        SELF rbx
        mov rsp, [rbx + 32]             // back on the host frame
        mov rcx, [rsp + 8]
        mov [rbx + 32], rcx
        mov rcx, [rsp + 24]
        mov [rbx], rcx
        mov rdx, [rsp]
        mov rdx, [rdx + 32]
        mov rcx, [rsp + 16]
        mov [rdx - 8], rcx              // the guest's return slot as it was
        cld
        add rsp, 40
        pop r15
        pop r14
        pop r13
        pop r12
        pop rbx
        pop rbp
        ret
        .size recomp_hybrid_call_native, . - recomp_hybrid_call_native

// uint64_t recomp_hybrid_checked_call(fn, a, b, c, d, fa, fb, uint64_t *xmm0, int *bad)
// rdi fn, rsi a, rdx b, rcx c, r8 d, r9 fa; stack: fb, xmm0, bad.
        .globl recomp_hybrid_checked_call
        .p2align 4
        .type recomp_hybrid_checked_call, @function
recomp_hybrid_checked_call:
        push rbp
        push rbx
        push r12
        push r13
        push r14
        push r15
        sub rsp, 24                     // stack arguments now at rsp + 80, 88, 96
        mov [rsp], rsp
        mov r11, rdi
        mov rdi, rsi
        mov rsi, rdx
        mov rdx, rcx
        mov rcx, r8
        vmovq xmm0, r9
        vmovq xmm1, qword ptr [rsp + 80]
        movabs rbx, 0x5a5a0000c0de0003
        movabs rbp, 0x5a5a0000c0de0005
        movabs r12, 0x5a5a0000c0de000c
        movabs r13, 0x5a5a0000c0de000d
        movabs r14, 0x5a5a0000c0de000e
        movabs r15, 0x5a5a0000c0de000f
        call r11
        movabs r11, 0x5a5a0000c0de0003
        mov r10, rbx
        xor r10, r11
        movabs r11, 0x5a5a0000c0de0005
        mov rcx, rbp
        xor rcx, r11
        or r10, rcx
        movabs r11, 0x5a5a0000c0de000c
        mov rcx, r12
        xor rcx, r11
        or r10, rcx
        movabs r11, 0x5a5a0000c0de000d
        mov rcx, r13
        xor rcx, r11
        or r10, rcx
        movabs r11, 0x5a5a0000c0de000e
        mov rcx, r14
        xor rcx, r11
        or r10, rcx
        movabs r11, 0x5a5a0000c0de000f
        mov rcx, r15
        xor rcx, r11
        or r10, rcx
        mov rcx, rsp
        xor rcx, [rsp]
        or r10, rcx
        mov rcx, [rsp + 88]
        vmovq qword ptr [rcx], xmm0
        mov rcx, [rsp + 96]
        test r10, r10
        setne r11b
        movzx r11d, r11b
        mov [rcx], r11d
        add rsp, 24
        pop r15
        pop r14
        pop r13
        pop r12
        pop rbx
        pop rbp
        ret
        .size recomp_hybrid_checked_call, . - recomp_hybrid_checked_call

        .section .note.GNU-stack, "", @progbits
