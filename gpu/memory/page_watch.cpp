// SPDX-License-Identifier: GPL-2.0-or-later
#include "memory/page_watch.h"

#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <unordered_set>

#ifndef _WIN32
#include <fcntl.h>
#include <linux/userfaultfd.h>
#include <poll.h>
#include <sys/ioctl.h>
#include <sys/syscall.h>
#endif
#include <sys/mman.h>
#include <ucontext.h>
#include <unistd.h>

#include "bblayer_write_traps.h"
#include "bbport_toggles.h"
#include "common/assert.h"
#include "common/error.h"
#include "common/logging/log.h"
#include "common/range_lock.h"
#include "common/signal_context.h"
#include "common/thread.h"
#include "core/memory.h"
#include "core/signals.h"
#include "memory/fault_sites.h"
#include "memory/page_lock.h"
#include "buffers/buffer.h"
#include "render/rasterizer.h"

extern "C" void runtime_memory_set_write_watch(uintptr_t address, uint64_t size, int watch);
extern "C" int runtime_memory_vma_info(uintptr_t address, int* prot, int* type, uintptr_t* end);

namespace BbMem {

namespace {

using Core::MemoryPermission;

/// Zero-filled memory for a table over the address space, committed where it is touched.
template <typename T>
T* ReserveTable(size_t count, const char* name) {
    void* mem = mmap(nullptr, count * sizeof(T), PROT_READ | PROT_WRITE,
                     MAP_PRIVATE | MAP_ANONYMOUS | MAP_NORESERVE, -1, 0);
    ASSERT_MSG(mem != MAP_FAILED, "page watch table {}", name);
    return static_cast<T*>(mem);
}

/// Watchers of a page: writes in the low 7 bits, a read watcher in the top bit (only the buffer
/// tracker watches reads, and buffers do not overlap).
constexpr u8 ReadWatcher = 0x80;
constexpr u8 WriteWatchers = 0x7f;

constexpr MemoryPermission Perms(u8 watchers) {
    MemoryPermission perms = MemoryPermission::None;
    if (!(watchers & ReadWatcher)) {
        perms |= MemoryPermission::Read;
    }
    if (!(watchers & WriteWatchers)) {
        perms |= MemoryPermission::Write;
    }
    return perms;
}

/// With the game's memory in place (no write tracking), the pages under images are trapped for
/// writes (WriteTraps::Image): a write nobody announced (the game's own copy loops, its heap over
/// an old texture) still reaches the texture cache, on any game and GPU, without hooks in the
/// game's code; the hooks and the libc wrappers only make it sooner. BB_IMAGE_TRAPS=0: announced
/// writes only. Latched at the first use: the counts must match their traps for the whole run.
bool ImageTraps() {
    static const bool on = [] {
        const char* env = std::getenv("BB_IMAGE_TRAPS");
        const bool enabled = !(env && env[0] == '0') && VideoCore::GuestInPlace() &&
                             !VideoCore::WriteTracking() && !VideoCore::WriteVerify();
        if (enabled) {
            std::printf("Guest memory: writes under images are trapped (BB_IMAGE_TRAPS=0: "
                        "announced writes only)\n");
        }
        return enabled;
    }();
    return on;
}

/// Coalesces page protection changes into runs of one permission.
class Runs {
public:
    explicit Runs(PageWatch::Impl& impl_, MemoryPermission perms_) : impl{impl_}, perms{perms_} {}
    ~Runs() {
        Flush();
    }
    /// The next page (in ascending order), its permission now and whether it changed.
    void Page(size_t page, MemoryPermission now, bool changed);

private:
    void Flush();

    PageWatch::Impl& impl;
    MemoryPermission perms;
    size_t begin = 0;
    size_t pages = 0;     ///< the run to apply
    size_t potential = 0; ///< the run with the unchanged pages of the same permission after it
};

} // namespace

struct PageWatch::Impl {
    static inline Vulkan::Rasterizer* rasterizer = nullptr;

    u8* watchers = ReserveTable<u8>(NumPages, "watchers");
    PageLock* locks = ReserveTable<PageLock>(NumRegions, "locks");
    /// ImageTraps: the images over each page; a page is trapped while one is there.
    u16* image_watchers = nullptr;
    std::once_flag image_watchers_once;

    explicit Impl(Vulkan::Rasterizer* rasterizer_) {
        rasterizer = rasterizer_;
    }
    virtual ~Impl() {
        munmap(watchers, NumPages * sizeof(u8));
        munmap(locks, NumRegions * sizeof(PageLock));
        if (image_watchers) {
            munmap(image_watchers, NumPages * sizeof(u16));
        }
    }

    virtual void OnMap(VAddr, u64) {}
    virtual void OnUnmap(VAddr, u64) {}
    virtual void Protect(VAddr address, u64 size, MemoryPermission perms) = 0;

    static bool OnFault(void* context, void* fault_address);

    template <bool watch, bool reads>
    static u8 Change(u8& count) {
        if constexpr (reads && watch) {
            count |= ReadWatcher;
        } else if constexpr (reads) {
            ASSERT_MSG(count & ReadWatcher, "no read watcher");
            count &= WriteWatchers;
        } else if constexpr (watch) {
            ASSERT_MSG((count & WriteWatchers) != WriteWatchers, "too many write watchers");
            ++count;
        } else {
            ASSERT_MSG(count & WriteWatchers, "not enough write watchers");
            --count;
        }
        return count;
    }
    /// Whether the change of a page's watchers changes its permission.
    template <bool watch, bool reads>
    static bool Toggles(u8 count) {
        const u8 n = reads ? (count >> 7) : (count & WriteWatchers);
        return watch ? n == 1 : n == 0;
    }

    template <bool watch>
    void WatchWrites(VAddr address, u64 size) {
        size_t page = address >> PageShift;
        const size_t end = (address + size + PageSize - 1) >> PageShift;
        Common::RangeLockGuard lk(locks + page / PagesPerRegion,
                                  locks + (end + PagesPerRegion - 1) / PagesPerRegion);
        Runs runs{*this, Perms(watchers[page])};
        for (; page != end; ++page) {
            const u8 count = Change<watch, false>(watchers[page]);
            runs.Page(page, Perms(count), Toggles<watch, false>(count));
        }
    }

    template <bool watch, bool reads>
    void WatchRegion(VAddr region, const RegionPages& mask) {
        const auto first = mask.FirstRange();
        const auto last = mask.LastRange();
        if (first.first >= first.second) {
            return;
        }
        if (first == last) {
            // One run of pages.
            if constexpr (!reads) {
                WatchWrites<watch>(region + (first.first << PageShift),
                                   (first.second - first.first) << PageShift);
                return;
            }
        }
        const size_t base = region >> PageShift;
        ASSERT(base % PagesPerRegion == 0);
        std::scoped_lock lk{locks[base / PagesPerRegion]};
        Runs runs{*this, Perms(watchers[base + first.first])};
        for (size_t page = first.first; page < last.second; ++page) {
            u8& count = watchers[base + page];
            if (!mask.Get(page)) {
                runs.Page(base + page, Perms(count), false);
                continue;
            }
            Change<watch, reads>(count);
            runs.Page(base + page, Perms(count), Toggles<watch, reads>(count));
        }
    }

    template <bool watch>
    void WatchImageTraps(VAddr address, u64 size) {
        std::call_once(image_watchers_once,
                       [this] { image_watchers = ReserveTable<u16>(NumPages, "images"); });
        size_t page = address >> PageShift;
        const size_t end = (address + size + PageSize - 1) >> PageShift;
        Common::RangeLockGuard lk(locks + page / PagesPerRegion,
                                  locks + (end + PagesPerRegion - 1) / PagesPerRegion);
        // Runs of pages whose first image came or last one went: one trap change each.
        size_t run_begin = 0, run_pages = 0;
        const auto flush = [&] {
            if (run_pages != 0) {
                BbLayer::WriteTraps::Set(run_begin << PageShift, run_pages << PageShift,
                                         BbLayer::WriteTraps::Image, watch);
                run_pages = 0;
            }
        };
        for (; page != end; ++page) {
            u16& count = image_watchers[page];
            if constexpr (watch) {
                if (count++ != 0) {
                    continue;
                }
            } else {
                ASSERT_MSG(count > 0, "image watchers below zero");
                if (--count != 0) {
                    continue;
                }
            }
            if (run_pages != 0 && run_begin + run_pages == page) {
                ++run_pages;
            } else {
                flush();
                run_begin = page;
                run_pages = 1;
            }
        }
        flush();
    }
};

namespace {

void Runs::Page(size_t page, MemoryPermission now, bool changed) {
    if (now != perms) {
        Flush();
        perms = now;
    } else if (pages != 0) {
        // Same permission: the run may cover it if a changed page follows.
        ++potential;
    }
    if (changed) {
        if (pages == 0) {
            begin = page;
            potential = 1;
        }
        pages = potential;
    }
}

void Runs::Flush() {
    if (pages != 0) {
        impl.Protect(begin << PageShift, pages << PageShift, perms);
        pages = potential = 0;
    }
}

/// The game's memory in place: the runtime's write watch (BB_WRITE_VERIFY=1: protected too, to
/// see writes nothing announced); page protection with BB_WRITE_TRACKING=1.
struct SignalWatch final : PageWatch::Impl {
    explicit SignalWatch(Vulkan::Rasterizer* rasterizer_) : Impl{rasterizer_} {
        // After BB_VRAM_ACCESS_TRAP (priority 0, diagnostics).
        Core::Signals::Instance()->RegisterAccessViolationHandler(OnFault, 1);
    }

    void Protect(VAddr address, u64 size, MemoryPermission perms) override {
        if (!VideoCore::WriteTracking()) {
            runtime_memory_set_write_watch(address, size, !True(perms & MemoryPermission::Write));
            if (!VideoCore::WriteVerify()) {
                return;
            }
        }
        ASSERT_MSG(perms != MemoryPermission::Write, "write-only pages");
        Core::Memory::Instance()->GetAddressSpace().Protect(address, size, perms);
    }
};

#ifndef _WIN32 // userfaultfd: Linux only
/// BB_UFFD=1: write-protection with userfaultfd instead of mprotect. mprotect takes the address
/// space lock for writing and splits mappings; while guest threads fault on per-frame buffers and
/// the GPU thread re-protects uploaded pages, every page fault in the process waits for it.
/// UFFDIO_WRITEPROTECT changes page table bits under the lock for reading. Read protection
/// (readbacks) still uses mprotect: userfaultfd write-protection cannot deny reads.
struct UffdWatch final : PageWatch::Impl {
    explicit UffdWatch(Vulkan::Rasterizer* rasterizer_) : Impl{rasterizer_} {
        uffd = int(syscall(SYS_userfaultfd, O_CLOEXEC | O_NONBLOCK | UFFD_USER_MODE_ONLY));
        if (uffd == -1) {
            LOG_ERROR(Common_Memory, "userfaultfd failed: {}, page protection instead",
                      Common::GetLastErrorMsg());
            throw std::runtime_error("userfaultfd");
        }
        // Guest memory is a shared memfd mapping: write-protection there needs the shmem
        // feature, and unpopulated pages must be protectable too.
        uffdio_api api{};
        api.api = UFFD_API;
        api.features =
            UFFD_FEATURE_THREAD_ID | UFFD_FEATURE_WP_HUGETLBFS_SHMEM | UFFD_FEATURE_WP_UNPOPULATED;
        if (ioctl(uffd, UFFDIO_API, &api) != 0) {
            LOG_ERROR(Common_Memory, "UFFDIO_API failed: {}, page protection instead",
                      Common::GetLastErrorMsg());
            close(uffd);
            throw std::runtime_error("uffdio_api");
        }
        // Read faults (readbacks) still arrive as signals.
        Core::Signals::Instance()->RegisterAccessViolationHandler(OnFault, 1);
        thread = std::jthread([this](std::stop_token stop) { Handler(stop); });
        std::printf("GPU: memory tracking with userfaultfd write-protection\n");
    }
    ~UffdWatch() override {
        thread = {};
        close(uffd);
    }

    void OnMap(VAddr address, u64 size) override {
        uffdio_register reg{};
        reg.range.start = address;
        reg.range.len = size;
        reg.mode = UFFDIO_REGISTER_MODE_WP;
        if (ioctl(uffd, UFFDIO_REGISTER, &reg) == -1) {
            LOG_ERROR(Common_Memory, "UFFDIO_REGISTER {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }
    void OnUnmap(VAddr address, u64 size) override {
        uffdio_range range{};
        range.start = address;
        range.len = size;
        ioctl(uffd, UFFDIO_UNREGISTER, &range);
    }

    void Protect(VAddr address, u64 size, MemoryPermission perms) override {
        auto& address_space = Core::Memory::Instance()->GetAddressSpace();
        const u64 first = address >> PageShift, last = (address + size + PageSize - 1) >> PageShift;
        if (!True(perms & MemoryPermission::Read)) {
            address_space.Protect(address, size, MemoryPermission::None);
            std::scoped_lock lk{revoked_mutex};
            for (u64 page = first; page < last; ++page) {
                read_revoked.insert(page);
            }
            num_read_revoked.store(read_revoked.size(), std::memory_order_release);
            return;
        }
        if (num_read_revoked.load(std::memory_order_acquire) != 0) {
            std::scoped_lock lk{revoked_mutex};
            bool restore = false;
            for (u64 page = first; page < last; ++page) {
                restore |= read_revoked.erase(page) != 0;
            }
            num_read_revoked.store(read_revoked.size(), std::memory_order_release);
            if (restore) {
                address_space.Protect(address, size, MemoryPermission::ReadWrite);
            }
        }
        const bool writable = True(perms & MemoryPermission::Write);
        // Counted like AddressSpace::Protect (BB_FRAME_LOG protect_ms).
        BbStats::Timer timer{BbStats::t_protect};
        BbStats::protect_calls.fetch_add(1, std::memory_order_relaxed);
        BbStats::protect_pages.fetch_add(last - first, std::memory_order_relaxed);
        if (!writable) {
            BbStats::protect_revoke_calls.fetch_add(1, std::memory_order_relaxed);
            BbStats::protect_revoke_pages.fetch_add(last - first, std::memory_order_relaxed);
        }
        uffdio_writeprotect wp{};
        wp.range.start = address;
        wp.range.len = size;
        wp.mode = writable ? UFFDIO_WRITEPROTECT_MODE_DONTWAKE : UFFDIO_WRITEPROTECT_MODE_WP;
        if (ioctl(uffd, UFFDIO_WRITEPROTECT, &wp) == -1) {
            LOG_ERROR(Common_Memory, "UFFDIO_WRITEPROTECT {:#x}+{:#x} failed: {}", address, size,
                      Common::GetLastErrorMsg());
        }
    }

    void Handler(std::stop_token stop) {
        Common::SetCurrentThreadName("bb:Uffd");
        while (!stop.stop_requested()) {
            pollfd fd{uffd, POLLIN, 0};
            // A short timeout, so the stop request is seen.
            if (poll(&fd, 1, 100) <= 0 || !(fd.revents & POLLIN)) {
                continue;
            }
            uffd_msg msg;
            if (read(uffd, &msg, sizeof(msg)) != sizeof(msg) || msg.event != UFFD_EVENT_PAGEFAULT) {
                continue;
            }
            const VAddr address = msg.arg.pagefault.address;
            {
                BbStats::Timer timer{BbStats::t_write_faults};
                rasterizer->OnWriteFault(address,
                                         rasterizer->IsGpuSideThreadId(msg.arg.pagefault.feat.ptid));
            }
            // Protect() lifts protection with DONTWAKE (it may run for pages nobody waits on).
            uffdio_range wake{PageStart(address), PageSize};
            ioctl(uffd, UFFDIO_WAKE, &wake);
        }
    }

    int uffd = -1;
    std::jthread thread;
    std::mutex revoked_mutex;
    std::unordered_set<u64> read_revoked; ///< pages denied reads with mprotect
    std::atomic<size_t> num_read_revoked{0};
};
#endif

} // namespace

bool PageWatch::Impl::OnFault(void* context, void* fault_address) {
    const auto address = reinterpret_cast<VAddr>(fault_address);
    const u64 rip = u64(static_cast<const ucontext_t*>(context)->uc_mcontext.gregs[REG_RIP]);
    FaultSites::Note(context, address);
    // The draw recording thread handles its faults inline too.
    const bool gpu_thread = rasterizer->IsGpuSideThread();
    if (gpu_thread) {
        BbStats::gpu_signal_faults.fetch_add(1, std::memory_order_relaxed);
    }
    const bool write = Common::IsWriteError(context);
    // Traps on reads too (no access): their owner first.
    const unsigned reasons = BbLayer::WriteTraps::Reasons(address);
    if (reasons & BbLayer::WriteTraps::VramData) {
        return rasterizer->OnVramDataAccess(address, gpu_thread);
    }
    if (reasons & BbLayer::WriteTraps::Probe) {
        return rasterizer->OnProbeAccess(address, rip, write, gpu_thread);
    }
    if (reasons & BbLayer::WriteTraps::QueryReads) {
        return rasterizer->OnOcclusionPageAccess(address, rip, write, gpu_thread);
    }
    if (!write) {
        BbStats::read_faults.fetch_add(1, std::memory_order_relaxed);
        BbStats::Timer timer{BbStats::t_read_faults};
        return rasterizer->ReadMemory(address, 8, gpu_thread);
    }
    if (ImageTraps() && !(reasons & BbLayer::WriteTraps::Image)) {
        // Not an image's trap: another owner's being lifted by another thread, or one lifted
        // meanwhile (the write runs again), or a write the game may not do.
        int prot = 0, type = -1;
        uintptr_t end = 0;
        return runtime_memory_vma_info(address, &prot, &type, &end) && (prot & 0x2);
    }
    BbStats::Timer timer{BbStats::t_write_faults};
    const bool handled = rasterizer->OnWriteFault(address, gpu_thread, rip);
    FaultSites::Done();
    return handled;
}

PageWatch::PageWatch(Vulkan::Rasterizer* rasterizer) {
    // dma-buf guest memory (BB_GUEST_GPU_MEMORY=1) cannot be write-protected by userfaultfd
    // (anonymous and shmem only): its registration would fail and writes go unseen.
    const char* uffd = std::getenv("BB_UFFD");
    const char* gpu_memory = std::getenv("BB_GUEST_GPU_MEMORY");
#ifdef _WIN32
    (void)gpu_memory;
    if (uffd && uffd[0] == '1') {
        std::printf("GPU: no userfaultfd on Windows (BB_UFFD ignored)\n");
    }
#else
    if (uffd && uffd[0] == '1') {
        if (gpu_memory && gpu_memory[0] == '1') {
            std::printf("GPU: userfaultfd off with BB_GUEST_GPU_MEMORY=1 (dma-buf memory)\n");
        } else {
            try {
                impl = std::make_unique<UffdWatch>(rasterizer);
                return;
            } catch (const std::runtime_error&) {
            }
        }
    }
#endif
    impl = std::make_unique<SignalWatch>(rasterizer);
}

PageWatch::~PageWatch() = default;

void PageWatch::OnGpuMap(VAddr address, u64 size) {
    impl->OnMap(address, size);
}

void PageWatch::OnGpuUnmap(VAddr address, u64 size) {
    impl->OnUnmap(address, size);
}

template <bool watch>
void PageWatch::WatchImage(VAddr address, u64 size) {
    impl->WatchWrites<watch>(address, size);
    if (ImageTraps()) {
        impl->WatchImageTraps<watch>(address, size);
    }
}

template <bool watch, bool reads>
void PageWatch::WatchRegion(VAddr region, const RegionPages& mask) {
    impl->WatchRegion<watch, reads>(region, mask);
}

template void PageWatch::WatchImage<true>(VAddr, u64);
template void PageWatch::WatchImage<false>(VAddr, u64);
template void PageWatch::WatchRegion<true, true>(VAddr, const RegionPages&);
template void PageWatch::WatchRegion<true, false>(VAddr, const RegionPages&);
template void PageWatch::WatchRegion<false, true>(VAddr, const RegionPages&);
template void PageWatch::WatchRegion<false, false>(VAddr, const RegionPages&);

} // namespace BbMem
