// SPDX-License-Identifier: GPL-2.0-or-later
// bbport (ray-traced AO): the guest ranges whose layer mirrors must outlive this frame's RTAO
// pass. The pass reads the frame's index buffers by device address when the game's AO pass
// ends, draws after the draws that bound them; a mirror retired meanwhile must not be freed
// before that. Only mirrors over such a range are held, nothing else (tests/test_rtao_geometry).

#pragma once

#include <cstdint>
#include <mutex>
#include <utility>
#include <vector>

namespace VideoCore {

class RetireHold {
public:
    /// A frame with the RTAO on: ranges are collected until End.
    void Begin() {
        std::scoped_lock lock{mutex};
        active = true;
        ranges.clear();
    }
    void End() {
        std::scoped_lock lock{mutex};
        active = false;
        ranges.clear();
    }
    /// An index range the RTAO reads by address this frame.
    void Add(std::uint64_t address, std::uint64_t size) {
        std::scoped_lock lock{mutex};
        if (active && size) {
            ranges.emplace_back(address, address + size);
        }
    }
    /// Must a buffer over [base, base + size) retired now wait for the RTAO pass?
    bool Holds(std::uint64_t base, std::uint64_t size) const {
        std::scoped_lock lock{mutex};
        if (!active) {
            return false;
        }
        for (const auto& [lo, hi] : ranges) {
            if (lo < base + size && base < hi) {
                return true;
            }
        }
        return false;
    }
    std::size_t Ranges() const {
        std::scoped_lock lock{mutex};
        return ranges.size();
    }
    bool Active() const {
        std::scoped_lock lock{mutex};
        return active;
    }

private:
    mutable std::mutex mutex;
    bool active = false;
    std::vector<std::pair<std::uint64_t, std::uint64_t>> ranges;
};

} // namespace VideoCore
