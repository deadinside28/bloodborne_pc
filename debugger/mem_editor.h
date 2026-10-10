// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include "mem_scanner.h"

#include <vector>
#include <string>
#include <mutex>

namespace Debugger {

struct WatchEntry {
    std::string label;
    uintptr_t address{0};
    DataType type{DataType::U32};
    bool frozen{false};
    double freeze_value{0.0};
};

class MemoryEditor {
public:
    static MemoryEditor& Get();

    void AddWatch(const std::string& label, uintptr_t address, DataType type);
    void RemoveWatch(size_t index);
    void Clear();

    size_t GetCount() const;
    std::vector<WatchEntry> GetEntries() const;

    void SetFrozen(size_t index, bool frozen);
    void SetFreezeValue(size_t index, double value);
    void SetLabel(size_t index, const std::string& label);
    void SetType(size_t index, DataType type);

    bool WriteValue(size_t index, double value);

    /// Ticks every frame to enforce frozen values
    void TickFreeze();

private:
    MemoryEditor() = default;

    mutable std::mutex mutex;
    std::vector<WatchEntry> entries;
};

} // namespace Debugger
