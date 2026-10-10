// SPDX-License-Identifier: GPL-2.0-or-later
#include "cutscene_detector.h"

#include <sys/uio.h>
#include <unistd.h>

namespace Debugger {

CutsceneDetector& CutsceneDetector::Get() {
    static CutsceneDetector instance;
    return instance;
}

bool CutsceneDetector::IsCutsceneActive() {
    if (!enabled.load(std::memory_order_relaxed)) {
        return false;
    }
    const uintptr_t addr = target_address.load(std::memory_order_relaxed);
    if (!addr) {
        return false;
    }

    uint8_t val = 0;
    struct iovec local_iov {
        &val, sizeof(val)
    };
    struct iovec remote_iov {
        reinterpret_cast<void*>(addr), sizeof(val)
    };
    const ssize_t n = process_vm_readv(getpid(), &local_iov, 1, &remote_iov, 1, 0);
    if (n == sizeof(val)) {
        return val == cutscene_value.load(std::memory_order_relaxed);
    }
    return false;
}

} // namespace Debugger
