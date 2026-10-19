// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: a lock that spins instead of sleeping, for a few instructions' worth of work. Waiters
// only read the word until it looks free, so they do not bounce its cache line.
#pragma once

#include <atomic>

#include <immintrin.h>

namespace Common {

class SpinLock {
public:
    SpinLock() = default;
    SpinLock(const SpinLock&) = delete;
    SpinLock& operator=(const SpinLock&) = delete;

    void lock() {
        while (locked.exchange(true, std::memory_order_acquire)) {
            while (locked.load(std::memory_order_relaxed)) {
                _mm_pause();
            }
        }
    }
    void unlock() {
        locked.store(false, std::memory_order_release);
    }
    [[nodiscard]] bool try_lock() {
        return !locked.load(std::memory_order_relaxed) &&
               !locked.exchange(true, std::memory_order_acquire);
    }

private:
    std::atomic<bool> locked{false};
};

} // namespace Common
