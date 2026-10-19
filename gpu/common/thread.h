// SPDX-License-Identifier: GPL-2.0-or-later
// bbport: thread names, priorities and precise sleeps.
#pragma once

#include <chrono>
#include <string>

#include "common/types.h"

namespace Common {

enum class ThreadPriority : u32 { Low, Normal, High, VeryHigh, Critical };

/// Names the calling thread (as top and debuggers show it; 15 characters at most).
void SetCurrentThreadName(const char* name);
/// Names a thread by its pthread handle.
void SetThreadName(void* thread, const char* name);
[[nodiscard]] std::string GetCurrentThreadName();

void SetCurrentThreadPriority(ThreadPriority priority);
/// Real-time scheduling for periodic threads: not used on Linux (no-op).
void SetCurrentThreadRealtime(std::chrono::nanoseconds period);

/// Sleeps `duration`; with `interruptible` a signal ends it early. `remaining` gets what is left.
bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   bool interruptible);

/// Paces a loop to a fixed period: Start() at the top waits for the period's deadline, End() at
/// the bottom moves it on by one period. Deadlines are absolute, so wake-up latency does not
/// accumulate; a loop late by more than a period starts over instead of catching up.
class AccurateTimer {
public:
    explicit AccurateTimer(std::chrono::nanoseconds period);
    void Start();
    void End();
    [[nodiscard]] std::chrono::nanoseconds GetTotalWait() const {
        return total_wait;
    }

private:
    std::chrono::nanoseconds period;
    std::chrono::nanoseconds total_wait{};
    std::chrono::steady_clock::time_point deadline{}; ///< when the current period ends
};

} // namespace Common
