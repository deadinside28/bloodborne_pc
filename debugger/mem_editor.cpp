// SPDX-License-Identifier: GPL-2.0-or-later
#include "mem_editor.h"

namespace Debugger {

MemoryEditor& MemoryEditor::Get() {
    static MemoryEditor instance;
    return instance;
}

void MemoryEditor::AddWatch(const std::string& label, uintptr_t address, DataType type) {
    std::lock_guard<std::mutex> lock(mutex);
    double cur_val = 0;
    std::string str;
    MemoryScanner::ReadFormatted(address, type, cur_val, str);
    entries.push_back({label, address, type, false, cur_val});
}

void MemoryEditor::RemoveWatch(size_t index) {
    std::lock_guard<std::mutex> lock(mutex);
    if (index < entries.size()) {
        entries.erase(entries.begin() + index);
    }
}

void MemoryEditor::Clear() {
    std::lock_guard<std::mutex> lock(mutex);
    entries.clear();
}

size_t MemoryEditor::GetCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return entries.size();
}

std::vector<WatchEntry> MemoryEditor::GetEntries() const {
    std::lock_guard<std::mutex> lock(mutex);
    return entries;
}

void MemoryEditor::SetFrozen(size_t index, bool frozen) {
    std::lock_guard<std::mutex> lock(mutex);
    if (index < entries.size()) {
        entries[index].frozen = frozen;
    }
}

void MemoryEditor::SetFreezeValue(size_t index, double value) {
    std::lock_guard<std::mutex> lock(mutex);
    if (index < entries.size()) {
        entries[index].freeze_value = value;
    }
}

void MemoryEditor::SetLabel(size_t index, const std::string& label) {
    std::lock_guard<std::mutex> lock(mutex);
    if (index < entries.size()) {
        entries[index].label = label;
    }
}

void MemoryEditor::SetType(size_t index, DataType type) {
    std::lock_guard<std::mutex> lock(mutex);
    if (index < entries.size()) {
        entries[index].type = type;
    }
}

bool MemoryEditor::WriteValue(size_t index, double value) {
    std::lock_guard<std::mutex> lock(mutex);
    if (index >= entries.size()) return false;
    entries[index].freeze_value = value;
    return MemoryScanner::WriteFormatted(entries[index].address, entries[index].type, value);
}

void MemoryEditor::TickFreeze() {
    std::lock_guard<std::mutex> lock(mutex);
    for (const auto& entry : entries) {
        if (entry.frozen && entry.address != 0) {
            MemoryScanner::WriteFormatted(entry.address, entry.type, entry.freeze_value);
        }
    }
}

} // namespace Debugger
