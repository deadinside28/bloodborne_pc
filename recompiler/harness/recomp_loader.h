// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once
#include <setjmp.h>
#include <stdint.h>

#include "recomp_rt.h"

typedef struct {
    uint8_t *file;  // the image file, mapped read-only
    uint8_t *base;  // the image's link address 0 in host memory (= __recomp_slide)
    void *eh;       // Elf64_Ehdr of the file
} RecompImage;

// A host function called from recompiled code: arguments from the guest registers, result to them.
typedef void (*RecompBridge)(RecompState *st, uint64_t host_fn);

extern RecompImage recomp_image;
extern _Thread_local sigjmp_buf recomp_trap_jmp;
extern _Thread_local int recomp_trap_armed;
extern _Atomic uint64_t recomp_dispatch_misses, recomp_import_calls;

void recomp_load(const char *path);                       // map, relocate, bind imports
uint64_t recomp_symbol(const char *name);                  // exported symbol -> link address (0: none)
RecompFn recomp_lookup(uint64_t link);                     // link address -> recompiled function
void recomp_set_bridge(const char *import, RecompBridge b);
// Call recompiled fn with SysV arguments: integers (more than six go on the guest stack) and up to
// eight float/double bit patterns for xmm0-7. Checks the stack pointer and the callee-saved
// registers afterwards. 0, or -1 with `why` filled in. Results: st->gpr[G_RAX], st->xmm[0].
int recomp_call(RecompFn fn, const uint64_t *args, int nargs, const uint64_t *fargs, int nfargs,
                RecompState *st, char *why);
