// bbport: GPU-side assertion failures stop the port with exit code 23.
#include <cstdio>
#include <cstdlib>
#include <stdexcept>
#include "common/assert.h"
#include "common/logging/log.h"

// bbport_write_log.cpp: with BB_WRITE_LOG, the latest guest memory writes (no-op otherwise).
extern "C" void bbgpu_dump_guest_writes(void* ucontext);

void assert_fail_impl() {
    std::fflush(stdout);
    bbgpu_dump_guest_writes(nullptr);
    std::fputs("STOP: GPU library assertion failed (see GPU log above)\n", stderr);
    std::_Exit(23);
}

[[noreturn]] void unreachable_impl() {
    assert_fail_impl();
    throw std::runtime_error("Unreachable code");
}

void assert_fail_debug_msg(const char* msg) {
    LOG_CRITICAL(Debug, "Assertion Failed!\n{}", msg);
    assert_fail_impl();
}
