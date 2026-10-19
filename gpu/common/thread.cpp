// SPDX-License-Identifier: GPL-2.0-or-later
#include <cerrno>
#include <cstring>
#include <ctime>

#include <pthread.h>
#include <sched.h>

#include "common/logging/log.h"
#include "common/thread.h"

namespace Common {

void SetCurrentThreadName(const char* name) {
    SetThreadName(reinterpret_cast<void*>(pthread_self()), name);
}

void SetThreadName(void* thread, const char* name) {
    // Linux takes at most 15 characters (and refuses longer names outright).
    char truncated[16];
    std::strncpy(truncated, name, sizeof(truncated) - 1);
    truncated[sizeof(truncated) - 1] = '\0';
    pthread_setname_np(reinterpret_cast<pthread_t>(thread), truncated);
}

std::string GetCurrentThreadName() {
    char name[64];
    if (pthread_getname_np(pthread_self(), name, sizeof(name)) != 0) {
        return "<unknown name>";
    }
    return name;
}

void SetCurrentThreadPriority(ThreadPriority priority) {
    // SCHED_OTHER has one static priority; the niceness of a thread is what can change, and
    // only downwards without privileges. Low priority work gets a higher nice value.
    if (priority == ThreadPriority::Low) {
        sched_param param{};
        pthread_setschedparam(pthread_self(), SCHED_OTHER, &param);
    }
}

void SetCurrentThreadRealtime(std::chrono::nanoseconds) {}

bool AccurateSleep(std::chrono::nanoseconds duration, std::chrono::nanoseconds* remaining,
                   bool interruptible) {
    timespec request{.tv_sec = time_t(duration.count() / 1'000'000'000),
                     .tv_nsec = long(duration.count() % 1'000'000'000)};
    timespec left{};
    int result;
    while ((result = nanosleep(&request, &left)) != 0 && errno == EINTR && !interruptible) {
        request = left;
    }
    if (remaining) {
        *remaining = result == 0 ? std::chrono::nanoseconds{0}
                                 : std::chrono::seconds{left.tv_sec} +
                                       std::chrono::nanoseconds{left.tv_nsec};
    }
    return result == 0;
}

AccurateTimer::AccurateTimer(std::chrono::nanoseconds period_) : period{period_} {}

void AccurateTimer::Start() {
    const auto now = std::chrono::steady_clock::now();
    if (deadline.time_since_epoch().count() == 0) {
        deadline = now;
    }
    if (deadline > now) {
        const auto since_epoch = deadline.time_since_epoch();
        const timespec at{
            .tv_sec = time_t(std::chrono::duration_cast<std::chrono::seconds>(since_epoch).count()),
            .tv_nsec = long((since_epoch % std::chrono::seconds{1}).count())};
        while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &at, nullptr) == EINTR) {
        }
    }
}

void AccurateTimer::End() {
    const auto now = std::chrono::steady_clock::now();
    deadline += period;
    if (deadline + period < now) {
        deadline = now; // far behind: start over
    }
    total_wait = std::chrono::duration_cast<std::chrono::nanoseconds>(deadline - now);
}

} // namespace Common
