// SPDX-License-Identifier: GPL-2.0-or-later
#include "mem_scanner.h"

#include <cstdio>
#include <cstring>
#include <cmath>
#include <algorithm>
#include <csetjmp>
#include <csignal>
#include <unistd.h>
#include "gpu/bbgpu.h"

extern "C" __thread sigjmp_buf* runtime_fault_recover;
extern "C" int runtime_memory_write_backing(uintptr_t address, const void* data, uint64_t size);

namespace Debugger {

MemoryScanner& MemoryScanner::Get() {
    static MemoryScanner instance;
    return instance;
}

size_t MemoryScanner::GetTypeSize(DataType type) {
    switch (type) {
    case DataType::U8:
    case DataType::I8: return 1;
    case DataType::U16:
    case DataType::I16: return 2;
    case DataType::U32:
    case DataType::I32:
    case DataType::Float: return 4;
    case DataType::U64:
    case DataType::I64:
    case DataType::Double: return 8;
    }
    return 4;
}

const char* MemoryScanner::GetTypeName(DataType type) {
    switch (type) {
    case DataType::U8: return "1 Byte (u8)";
    case DataType::I8: return "1 Byte (i8)";
    case DataType::U16: return "2 Bytes (u16)";
    case DataType::I16: return "2 Bytes (i16)";
    case DataType::U32: return "4 Bytes (u32)";
    case DataType::I32: return "4 Bytes (i32)";
    case DataType::U64: return "8 Bytes (u64)";
    case DataType::I64: return "8 Bytes (i64)";
    case DataType::Float: return "Float";
    case DataType::Double: return "Double";
    }
    return "Unknown";
}

bool MemoryScanner::ReadMemory(uintptr_t addr, void* dest, size_t size) {
    if (!addr || !dest || size == 0) return false;
    sigjmp_buf recover;
    if (sigsetjmp(recover, 1) != 0) {
        runtime_fault_recover = nullptr;
        return false;
    }
    runtime_fault_recover = &recover;
    std::memcpy(dest, reinterpret_cast<const void*>(addr), size);
    runtime_fault_recover = nullptr;
    return true;
}

bool MemoryScanner::WriteMemory(uintptr_t addr, const void* src, size_t size) {
    if (!addr || !src || size == 0) return false;
    if (runtime_memory_write_backing(addr, src, size)) {
        return true;
    }
    sigjmp_buf recover;
    if (sigsetjmp(recover, 1) != 0) {
        runtime_fault_recover = nullptr;
        return false;
    }
    runtime_fault_recover = &recover;
    std::memcpy(reinterpret_cast<void*>(addr), src, size);
    runtime_fault_recover = nullptr;
    return true;
}

static double DecodeValue(DataType type, const void* ptr) {
    switch (type) {
    case DataType::U8: return *reinterpret_cast<const uint8_t*>(ptr);
    case DataType::I8: return *reinterpret_cast<const int8_t*>(ptr);
    case DataType::U16: return *reinterpret_cast<const uint16_t*>(ptr);
    case DataType::I16: return *reinterpret_cast<const int16_t*>(ptr);
    case DataType::U32: return *reinterpret_cast<const uint32_t*>(ptr);
    case DataType::I32: return *reinterpret_cast<const int32_t*>(ptr);
    case DataType::U64: return static_cast<double>(*reinterpret_cast<const uint64_t*>(ptr));
    case DataType::I64: return static_cast<double>(*reinterpret_cast<const int64_t*>(ptr));
    case DataType::Float: return *reinterpret_cast<const float*>(ptr);
    case DataType::Double: return *reinterpret_cast<const double*>(ptr);
    }
    return 0.0;
}

static uint64_t EncodeRaw(DataType type, const void* ptr) {
    uint64_t raw = 0;
    std::memcpy(&raw, ptr, std::min(sizeof(raw), MemoryScanner::GetTypeSize(type)));
    return raw;
}

bool MemoryScanner::ReadFormatted(uintptr_t addr, DataType type, double& out_num, std::string& out_str) {
    uint8_t buffer[8] = {0};
    const size_t sz = GetTypeSize(type);
    if (!ReadMemory(addr, buffer, sz)) {
        out_str = "???";
        return false;
    }
    out_num = DecodeValue(type, buffer);
    char tmp[64];
    switch (type) {
    case DataType::U8: std::snprintf(tmp, sizeof(tmp), "%u", *reinterpret_cast<uint8_t*>(buffer)); break;
    case DataType::I8: std::snprintf(tmp, sizeof(tmp), "%d", *reinterpret_cast<int8_t*>(buffer)); break;
    case DataType::U16: std::snprintf(tmp, sizeof(tmp), "%u", *reinterpret_cast<uint16_t*>(buffer)); break;
    case DataType::I16: std::snprintf(tmp, sizeof(tmp), "%d", *reinterpret_cast<int16_t*>(buffer)); break;
    case DataType::U32: std::snprintf(tmp, sizeof(tmp), "%u", *reinterpret_cast<uint32_t*>(buffer)); break;
    case DataType::I32: std::snprintf(tmp, sizeof(tmp), "%d", *reinterpret_cast<int32_t*>(buffer)); break;
    case DataType::U64: std::snprintf(tmp, sizeof(tmp), "%llu", static_cast<unsigned long long>(*reinterpret_cast<uint64_t*>(buffer))); break;
    case DataType::I64: std::snprintf(tmp, sizeof(tmp), "%lld", static_cast<long long>(*reinterpret_cast<int64_t*>(buffer))); break;
    case DataType::Float: std::snprintf(tmp, sizeof(tmp), "%.3f", *reinterpret_cast<float*>(buffer)); break;
    case DataType::Double: std::snprintf(tmp, sizeof(tmp), "%.4f", *reinterpret_cast<double*>(buffer)); break;
    }
    out_str = tmp;
    return true;
}

bool MemoryScanner::WriteFormatted(uintptr_t addr, DataType type, double num) {
    uint8_t buffer[8] = {0};
    const size_t sz = GetTypeSize(type);
    switch (type) {
    case DataType::U8: *reinterpret_cast<uint8_t*>(buffer) = static_cast<uint8_t>(num); break;
    case DataType::I8: *reinterpret_cast<int8_t*>(buffer) = static_cast<int8_t>(num); break;
    case DataType::U16: *reinterpret_cast<uint16_t*>(buffer) = static_cast<uint16_t>(num); break;
    case DataType::I16: *reinterpret_cast<int16_t*>(buffer) = static_cast<int16_t>(num); break;
    case DataType::U32: *reinterpret_cast<uint32_t*>(buffer) = static_cast<uint32_t>(num); break;
    case DataType::I32: *reinterpret_cast<int32_t*>(buffer) = static_cast<int32_t>(num); break;
    case DataType::U64: *reinterpret_cast<uint64_t*>(buffer) = static_cast<uint64_t>(num); break;
    case DataType::I64: *reinterpret_cast<int64_t*>(buffer) = static_cast<int64_t>(num); break;
    case DataType::Float: *reinterpret_cast<float*>(buffer) = static_cast<float>(num); break;
    case DataType::Double: *reinterpret_cast<double*>(buffer) = num; break;
    }
    return WriteMemory(addr, buffer, sz);
}

std::vector<MemoryRegion> MemoryScanner::QueryRegions(ScanScope scope) {
    std::vector<MemoryRegion> regions;
    FILE* f = std::fopen("/proc/self/maps", "r");
    if (!f) return regions;

    const uintptr_t img_base = bbgpu_get_guest_image_base();
    const uint64_t img_size = bbgpu_get_guest_image_size();
    const uintptr_t img_end = (img_base && img_size) ? (img_base + img_size) : (img_base + 0x20000000ULL);

    char line[512];
    while (std::fgets(line, sizeof(line), f)) {
        uintptr_t start = 0, end = 0;
        char perms[5] = {0};
        if (std::sscanf(line, "%lx-%lx %4s", &start, &end, perms) == 3) {
            // Must be readable. GPU write-tracked guest memory is r--p, so do not require 'w'
            if (perms[0] != 'r') {
                continue;
            }
            if (scope == ScanScope::ExecutableOnly) {
                if (img_base != 0) {
                    if (start >= img_base && end <= img_end) {
                        regions.push_back({start, end});
                    }
                } else if (start >= 0x400000 && end <= 0x20000000) {
                    regions.push_back({start, end});
                }
            } else if (scope == ScanScope::GuestHeap) {
                // PS4 user / guest space is [min_addr, 0x100000000000)
                const uintptr_t min_addr = img_base ? img_base : 0x400000;
                if (start >= min_addr && end <= 0x100000000000ULL) {
                    regions.push_back({start, end});
                }
            } else {
                regions.push_back({start, end});
            }
        }
    }
    std::fclose(f);
    return regions;
}

static bool MatchesCondition(ScanComparison comp, DataType type, double current, double prev, double target) {
    const double epsilon = (type == DataType::Float || type == DataType::Double) ? 0.001 : 0.0;
    switch (comp) {
    case ScanComparison::Exact:
        return std::fabs(current - target) <= epsilon;
    case ScanComparison::Changed:
        return std::fabs(current - prev) > epsilon;
    case ScanComparison::Unchanged:
        return std::fabs(current - prev) <= epsilon;
    case ScanComparison::Increased:
        return current > (prev + epsilon);
    case ScanComparison::Decreased:
        return current < (prev - epsilon);
    }
    return false;
}

MemoryScanner::~MemoryScanner() {
    CancelScan();
    if (scan_thread.joinable()) {
        scan_thread.join();
    }
}

void MemoryScanner::CancelScan() {
    cancel_requested.store(true);
}

DataType MemoryScanner::GetCurrentType() const {
    std::lock_guard<std::mutex> lock(mutex);
    return current_type;
}

void MemoryScanner::StartFirstScan(DataType type, ScanComparison comp, double value, ScanScope scope) {
    if (scanning.load()) return;
    if (scan_thread.joinable()) {
        scan_thread.join();
    }

    cancel_requested.store(false);
    scanning.store(true);
    progress.store(0.0f);

    scan_thread = std::thread([this, type, comp, value, scope]() {
        DoFirstScan(type, comp, value, scope);
    });
}

void MemoryScanner::DoFirstScan(DataType type, ScanComparison comp, double value, ScanScope scope) {
    const auto regions = QueryRegions(scope);
    const size_t type_size = GetTypeSize(type);
    constexpr size_t CHUNK_SIZE = 256 * 1024; // 256 KB chunks
    std::vector<uint8_t> chunk(CHUNK_SIZE);

    constexpr size_t MAX_MATCHES = 300000;
    std::vector<ScanMatch> temp_matches;
    temp_matches.reserve(10000);

    size_t total_bytes = 0;
    for (const auto& r : regions) total_bytes += (r.end - r.start);
    size_t processed_bytes = 0;

    for (const auto& r : regions) {
        if (cancel_requested.load()) break;
        for (uintptr_t cur = r.start; cur < r.end; cur += CHUNK_SIZE) {
            if (cancel_requested.load()) break;
            const size_t to_read = std::min(CHUNK_SIZE, static_cast<size_t>(r.end - cur));
            bool ok = ReadMemory(cur, chunk.data(), to_read);
            if (!ok && to_read > 4096) {
                // Page-by-page fallback so a single guarded page doesn't drop 256 KB
                constexpr size_t PAGE_SIZE = 4096;
                bool any_page_read = false;
                for (size_t poff = 0; poff < to_read; poff += PAGE_SIZE) {
                    size_t psz = std::min(PAGE_SIZE, to_read - poff);
                    if (!ReadMemory(cur + poff, chunk.data() + poff, psz)) {
                        std::memset(chunk.data() + poff, 0, psz);
                    } else {
                        any_page_read = true;
                    }
                }
                ok = any_page_read;
            }
            if (ok) {
                for (size_t off = 0; off + type_size <= to_read; off += type_size) {
                    const double val = DecodeValue(type, chunk.data() + off);
                    if (MatchesCondition(comp, type, val, val, value)) {
                        const uint64_t raw = EncodeRaw(type, chunk.data() + off);
                        temp_matches.push_back({cur + off, raw, raw});
                        if (temp_matches.size() >= MAX_MATCHES) {
                            goto done;
                        }
                    }
                }
            }
            processed_bytes += to_read;
            if (total_bytes > 0) {
                progress.store(static_cast<float>(processed_bytes) / static_cast<float>(total_bytes));
            }
        }
    }

done:
    if (!cancel_requested.load()) {
        std::lock_guard<std::mutex> lock(mutex);
        current_type = type;
        matches = std::move(temp_matches);
    }
    scanning.store(false);
    progress.store(1.0f);
}

void MemoryScanner::NextScan(ScanComparison comp, double value) {
    if (scanning.load()) return;
    if (scan_thread.joinable()) {
        scan_thread.join();
    }

    cancel_requested.store(false);
    scanning.store(true);
    progress.store(0.0f);

    scan_thread = std::thread([this, comp, value]() {
        DoNextScan(comp, value);
    });
}

void MemoryScanner::DoNextScan(ScanComparison comp, double value) {
    std::vector<ScanMatch> prev_list;
    DataType type;
    {
        std::lock_guard<std::mutex> lock(mutex);
        prev_list = matches;
        type = current_type;
    }

    if (prev_list.empty()) {
        scanning.store(false);
        progress.store(1.0f);
        return;
    }

    const size_t type_size = GetTypeSize(type);
    std::vector<ScanMatch> next_matches;
    next_matches.reserve(std::min(prev_list.size(), static_cast<size_t>(50000)));

    uint8_t buffer[8];
    for (size_t i = 0; i < prev_list.size(); ++i) {
        if (cancel_requested.load()) break;

        auto& m = prev_list[i];
        if (ReadMemory(m.address, buffer, type_size)) {
            const double current_val = DecodeValue(type, buffer);
            const double prev_val = DecodeValue(type, &m.current_raw);
            if (MatchesCondition(comp, type, current_val, prev_val, value)) {
                m.prev_raw = m.current_raw;
                m.current_raw = EncodeRaw(type, buffer);
                next_matches.push_back(m);
            }
        }
        if ((i % 1000) == 0) {
            progress.store(static_cast<float>(i) / static_cast<float>(prev_list.size()));
        }
    }

    if (!cancel_requested.load()) {
        std::lock_guard<std::mutex> lock(mutex);
        matches = std::move(next_matches);
    }
    scanning.store(false);
    progress.store(1.0f);
}

void MemoryScanner::Reset() {
    CancelScan();
    if (scan_thread.joinable()) {
        scan_thread.join();
    }
    std::lock_guard<std::mutex> lock(mutex);
    matches.clear();
    scanning.store(false);
    progress.store(0.0f);
}

size_t MemoryScanner::GetMatchCount() const {
    std::lock_guard<std::mutex> lock(mutex);
    return matches.size();
}

std::vector<ScanMatch> MemoryScanner::GetMatches(size_t offset, size_t limit) {
    std::lock_guard<std::mutex> lock(mutex);
    std::vector<ScanMatch> result;
    if (offset >= matches.size()) return result;
    const size_t count = std::min(limit, matches.size() - offset);
    result.insert(result.end(), matches.begin() + offset, matches.begin() + offset + count);
    return result;
}

} // namespace Debugger
