/* Regression: Bloodborne's hybrid locks pack ScePthread handles into 40 bits.
 * Force ordinary calloc into a high address, as glibc's worker arenas can do. */
#define _GNU_SOURCE
#include <pthread.h>
#include <sys/mman.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <assert.h>

static pthread_mutex_t map_lock = PTHREAD_MUTEX_INITIALIZER;
static uintptr_t high_next = UINT64_C(0x700000000000);
static uintptr_t low_next_test = UINT64_C(0x800000000);
static void *map_next(uintptr_t *next, size_t size, int prot) {
    size = (size + 4095) & ~(size_t)4095;
    pthread_mutex_lock(&map_lock);
    void *p = mmap((void *)*next, size, prot,
                  MAP_PRIVATE | MAP_ANONYMOUS | MAP_FIXED_NOREPLACE, -1, 0);
    *next += size + 4096;
    pthread_mutex_unlock(&map_lock);
    assert(p != MAP_FAILED);
    return p;
}
static __attribute__((unused)) void *high_calloc(size_t n, size_t size) {
    return map_next(&high_next, n * size, PROT_READ | PROT_WRITE);
}
void *runtime_low_map(size_t size, int prot) {
    return map_next(&low_next_test, size, prot);
}
void runtime_thread_keys_cleanup(void) {}
#define calloc high_calloc
#include "../src/runtime_thread.c"
#undef calloc

static ABI void *guest_worker(void *unused) {
    (void)unused;
    uintptr_t self = (uintptr_t)thread_self();
    /* This is the exact owner-mask/full-handle comparison in the game. */
    assert((self & UINT64_C(0xffffffffff)) == self);
    return (void *)self;
}
static void *creator(void *unused) {
    (void)unused;
    GuestThread *self = runtime_thread_current();
    assert((uintptr_t)self < (UINT64_C(1) << 40));
    for (int i = 0; i < 32; ++i) {
        GuestThread *child = NULL;
        assert(thread_create(&child, NULL, guest_worker, NULL, "handle-test") == 0);
        assert((uintptr_t)child < (UINT64_C(1) << 40));
        void *result = NULL;
        assert(thread_join(child, &result) == 0);
        assert(result == child);
    }
    return NULL;
}
int main(void) {
    pthread_t workers[8];
    for (unsigned i = 0; i < 8; ++i)
        assert(pthread_create(&workers[i], NULL, creator, NULL) == 0);
    for (unsigned i = 0; i < 8; ++i)
        assert(pthread_join(workers[i], NULL) == 0);
    puts("PASS: 256 guest thread handles preserve identity in 40-bit locks");
}
