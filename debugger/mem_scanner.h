// SPDX-License-Identifier: GPL-2.0-or-later
#pragma once

#include <cstdint>
#include <cstddef>
#include <vector>
#include <string>
#include <mutex>
#include <atomic>
#include <thread>

namespace Debugger {

enum class DataType {
    U8,
    U16,
    U32,
    U64,
    I8,
    I16,
    I32,
    I64,
    Float,
    Double
};

enum class ScanComparison {
    Exact,
    Changed,
    Unchanged,
    Increased,
    Decreased
};

enum class ScanScope {
    ExecutableOnly,  // eboot .data/.bss and code area (< 256 MB, very fast)
    GuestHeap,       // PS4 user ranges below 1 TiB (heap + direct memory)
    FullProcess      // All readable and writable memory maps
};

struct ScanMatch {
    uintptr_t address;
    uint64_t prev_raw;
    uint64_t current_raw;
};

struct MemoryRegion {
    uintptr_t start;
    uintptr_t end;
};

class MemoryScanner {
public:
    static MemoryScanner& Get();
    ~MemoryScanner();

    void StartFirstScan(DataType type, ScanComparison comp, double value, ScanScope scope);
    void NextScan(ScanComparison comp, double value);
    void CancelScan();
    void Reset();

    size_t GetMatchCount() const;
    std::vector<ScanMatch> GetMatches(size_t offset, size_t limit);

    bool IsScanning() const { return scanning.load(std::memory_order_relaxed); }
    float GetProgress() const { return progress.load(std::memory_order_relaxed); }
    DataType GetCurrentType() const;

    static bool ReadMemory(uintptr_t addr, void* dest, size_t size);
    static bool WriteMemory(uintptr_t addr, const void* src, size_t size);

    static bool ReadFormatted(uintptr_t addr, DataType type, double& out_num, std::string& out_str);
    static bool WriteFormatted(uintptr_t addr, DataType type, double num);

    static size_t GetTypeSize(DataType type);
    static const char* GetTypeName(DataType type);

private:
    MemoryScanner() = default;

    void DoFirstScan(DataType type, ScanComparison comp, double value, ScanScope scope);
    void DoNextScan(ScanComparison comp, double value);

    std::vector<MemoryRegion> QueryRegions(ScanScope scope);

    mutable std::mutex mutex;
    std::thread scan_thread;
    std::atomic<bool> scanning{false};
    std::atomic<bool> cancel_requested{false};
    std::atomic<float> progress{0.0f};

    DataType current_type{DataType::U32};
    std::vector<ScanMatch> matches;
};

} // namespace Debugger
