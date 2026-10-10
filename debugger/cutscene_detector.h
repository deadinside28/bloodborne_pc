// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <atomic>

namespace Debugger {

class CutsceneDetector {
public:
    static CutsceneDetector& Get();

    bool IsEnabled() const { return enabled.load(std::memory_order_relaxed); }
    void SetEnabled(bool value) { enabled.store(value, std::memory_order_relaxed); }

    uintptr_t GetAddress() const { return target_address.load(std::memory_order_relaxed); }
    void SetAddress(uintptr_t addr) { target_address.store(addr, std::memory_order_relaxed); }

    uint8_t GetCutsceneValue() const { return cutscene_value.load(std::memory_order_relaxed); }
    void SetCutsceneValue(uint8_t val) { cutscene_value.store(val, std::memory_order_relaxed); }

    /// Returns true if a cutscene is currently playing according to the monitored memory flag.
    /// Thread-safe and crash-safe (never segfaults on unmapped memory).
    bool IsCutsceneActive();

private:
    CutsceneDetector() = default;

    std::atomic<bool> enabled{false};
    std::atomic<uintptr_t> target_address{0};
    std::atomic<uint8_t> cutscene_value{1};
};

} // namespace Debugger
