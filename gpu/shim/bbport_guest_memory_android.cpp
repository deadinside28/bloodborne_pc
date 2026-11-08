// SPDX-FileCopyrightText: Copyright 2026 bbport contributors
// SPDX-License-Identifier: GPL-2.0-or-later
// bbport guest memory on Android: the game's direct memory in hardware buffers.
//
// Android's GPU drivers (Adreno's, checked with tools/android_memprobe.c) export no dma-buf of
// system memory and import no host memory, the PC's two ways of letting the GPU use the game's
// memory in place. A hardware buffer (AHardwareBuffer, BLOB) gives both sides one memory: the GPU
// imports it (VK_ANDROID_external_memory_android_hardware_buffer), the runtime maps its dma-buf
// (the native handle's fd, sent over a socket) where the game maps that memory. One buffer holds
// at most 256 MiB (larger ones are refused), so the runtime asks for its 256 MiB grid of chunks
// (runtime_memory_set_guest_chunk_whole(0)); a binding across two chunks takes the memory
// module's other path (a copy), as a range lazy imports could not hold.
#include "bbport_guest_memory_internal.h"

#if defined(__ANDROID__)
#include <android/hardware_buffer.h>
#include <sys/socket.h>

namespace BbGuestMemory::detail {
namespace {

constexpr u64 MaxBuffer = u64{256} << 20;

/// MemAvailable (/proc/meminfo), bytes; 0 when unknown.
u64 Available() {
    FILE* f = std::fopen("/proc/meminfo", "r");
    char line[256];
    unsigned long long kib = 0;
    while (f && std::fgets(line, sizeof(line), f)) {
        if (std::sscanf(line, "MemAvailable: %llu kB", &kib) == 1) {
            break;
        }
    }
    if (f) {
        std::fclose(f);
    }
    return u64{kib} << 10;
}

/// What must stay available to the rest of the phone: hardware buffers come from the system's
/// allocator, outside what killing this app gives back at once, and an allocation loop that ran
/// the memory out restarted a phone (tools/android_memprobe.c). BB_AHB_RESERVE_MB (2048).
u64 Reserve() {
    static const u64 reserve = [] {
        const char* env = bb_env("BB_AHB_RESERVE_MB");
        return (env && *env ? u64{std::strtoull(env, nullptr, 10)} : u64{2048}) << 20;
    }();
    return reserve;
}

AHardwareBuffer* Allocate(u64 size) {
    if (size == 0 || size > MaxBuffer) {
        return nullptr;
    }
    const AHardwareBuffer_Desc desc{
        .width = static_cast<std::uint32_t>(size),
        .height = 1,
        .layers = 1,
        .format = AHARDWAREBUFFER_FORMAT_BLOB,
        .usage = AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
                 AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER,
    };
    AHardwareBuffer* buffer = nullptr;
    return AHardwareBuffer_allocate(&desc, &buffer) == 0 ? buffer : nullptr;
}

/// The dma-buf behind a hardware buffer (its native handle's first fd, received over a socket
/// pair: the public NDK way to the fd), or -1. The caller owns it.
int BufferFd(AHardwareBuffer* ahb) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET | SOCK_CLOEXEC, 0, sv)) {
        return -1;
    }
    int fd = -1;
    if (AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0]) == 0) {
        char data[1024];
        alignas(cmsghdr) char control[CMSG_SPACE(sizeof(int) * 32)];
        iovec iov{data, sizeof(data)};
        msghdr msg{};
        msg.msg_iov = &iov;
        msg.msg_iovlen = 1;
        msg.msg_control = control;
        msg.msg_controllen = sizeof(control);
        if (recvmsg(sv[1], &msg, MSG_CMSG_CLOEXEC) > 0) {
            for (cmsghdr* c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
                if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) {
                    continue;
                }
                const int n = int((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
                const int* fds = reinterpret_cast<const int*>(CMSG_DATA(c));
                for (int i = 0; i < n; ++i) {
                    if (fd < 0) {
                        fd = fds[i];
                    } else {
                        close(fds[i]);
                    }
                }
            }
        }
    }
    close(sv[0]);
    close(sv[1]);
    return fd;
}

/// The GPU's import of `ahb` with a buffer of `size` over it (ChunkBufferUsage, device addresses
/// for the layer's memory module).
bool ImportAhb(AHardwareBuffer* ahb, u64 size, vk::Buffer& buffer, vk::DeviceMemory& memory,
               vk::Result& error) {
    const auto [props_result, props] = device.getAndroidHardwareBufferPropertiesANDROID(*ahb);
    if (props_result != vk::Result::eSuccess) {
        error = props_result;
        return false;
    }
    const vk::ExternalMemoryBufferCreateInfo external{
        .handleTypes = vk::ExternalMemoryHandleTypeFlagBits::eAndroidHardwareBufferANDROID,
    };
    const auto [buffer_result, created] = device.createBuffer({
        .pNext = &external,
        .size = size,
        .usage = ChunkBufferUsage(),
        .sharingMode = vk::SharingMode::eExclusive,
    });
    if (buffer_result != vk::Result::eSuccess) {
        error = buffer_result;
        return false;
    }
    std::uint32_t type = ~0u;
    for (std::uint32_t i = 0; i < 32 && type == ~0u; ++i) {
        if (props.memoryTypeBits & (1u << i)) {
            type = i;
        }
    }
    const vk::MemoryAllocateFlagsInfo address_flags{
        .flags = vk::MemoryAllocateFlagBits::eDeviceAddress,
    };
    const vk::ImportAndroidHardwareBufferInfoANDROID import{
        .pNext = LayerMemory() ? &address_flags : nullptr,
        .buffer = ahb,
    };
    const vk::MemoryDedicatedAllocateInfo dedicated{
        .pNext = &import,
        .buffer = created,
    };
    auto [memory_result, allocated] = device.allocateMemory({
        .pNext = &dedicated,
        .allocationSize = props.allocationSize,
        .memoryTypeIndex = type,
    });
    if (memory_result != vk::Result::eSuccess) {
        std::tie(memory_result, allocated) = device.allocateMemory({
            .pNext = &import,
            .allocationSize = props.allocationSize,
            .memoryTypeIndex = type,
        });
    }
    if (memory_result != vk::Result::eSuccess) {
        device.destroyBuffer(created);
        error = memory_result;
        return false;
    }
    if (const auto result = device.bindBufferMemory(created, allocated, 0);
        result != vk::Result::eSuccess) {
        device.freeMemory(allocated);
        device.destroyBuffer(created);
        error = result;
        return false;
    }
    buffer = created;
    memory = allocated;
    return true;
}

} // namespace

int AllocAhbChunk(u64 phys, u64 size) {
    {
        std::scoped_lock lk{mutex};
        if (chunk_count == MaxChunks) {
            return -1;
        }
    }
    static std::atomic<int> failures{0};
    const auto fail = [&](const char* what) {
        if (failures.fetch_add(1) < 4) {
            std::fprintf(stderr, "Guest memory: %s for %#llx+%llu MiB; that chunk stays in the memfd\n",
                         what, (unsigned long long)phys, (unsigned long long)(size >> 20));
        }
        return -1;
    };
    if (const u64 free_now = Available(); free_now < size + Reserve()) {
        static std::atomic<int> told{0};
        if (told.fetch_add(1) < 4) {
            std::printf("Guest memory: %llu MiB available, %llu MiB must stay so: %#llx+%llu MiB "
                        "stays in ordinary memory (the GPU copies it)\n",
                        (unsigned long long)(free_now >> 20), (unsigned long long)(Reserve() >> 20),
                        (unsigned long long)phys, (unsigned long long)(size >> 20));
        }
        return -1;
    }
    AHardwareBuffer* ahb = Allocate(size);
    if (!ahb) {
        return fail("no hardware buffer");
    }
    vk::Buffer buffer;
    vk::DeviceMemory memory;
    vk::Result error{};
    if (!ImportAhb(ahb, size, buffer, memory, error)) {
        AHardwareBuffer_release(ahb);
        return fail("the GPU did not import the hardware buffer");
    }
    const int fd = BufferFd(ahb);
    AHardwareBuffer_release(ahb); // the import and the fd keep it
    if (fd < 0) {
        device.freeMemory(memory);
        device.destroyBuffer(buffer);
        return fail("no dma-buf from the hardware buffer");
    }
    {
        std::scoped_lock lk{mutex};
        std::size_t i = chunk_count++;
        for (; i > 0 && chunks[i - 1]->phys > phys; --i) {
            chunks[i] = chunks[i - 1];
        }
        chunks[i] = new Chunk{phys, size, buffer, memory, next_index++};
        RegisterChunk(chunks[i]);
    }
    const u64 total = chunk_bytes.fetch_add(size) + size;
    std::printf("Guest memory: direct memory %#llx+%llu MiB in a hardware buffer, %llu MiB so far\n",
                (unsigned long long)phys, (unsigned long long)(size >> 20),
                (unsigned long long)(total >> 20));
    return fd;
}

bool AhbWorks(const Vulkan::Instance& instance, const char*& why) {
    if (!instance.IsAndroidHardwareBufferSupported()) {
        why = "no VK_ANDROID_external_memory_android_hardware_buffer";
        return false;
    }
    constexpr u64 Size = 2 << 20, Offset = 1 << 20;
    AHardwareBuffer* ahb = Allocate(Size);
    if (!ahb) {
        why = "no hardware buffer (AHardwareBuffer_allocate)";
        return false;
    }
    vk::Buffer buffer;
    vk::DeviceMemory memory;
    vk::Result error{};
    const bool imported = ImportAhb(ahb, Size, buffer, memory, error);
    const int fd = BufferFd(ahb);
    AHardwareBuffer_release(ahb);
    bool ok = false;
    if (!imported) {
        why = "the GPU does not import hardware buffers";
    } else if (fd < 0) {
        why = "no dma-buf from a hardware buffer";
    } else {
        // the whole buffer and one page inside it (the runtime maps chunks at offsets), written
        // through one, read through the other
        void* whole = mmap(nullptr, Size, PROT_READ | PROT_WRITE, MAP_SHARED, fd, 0);
        void* page = mmap(nullptr, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, Offset);
        if (whole == MAP_FAILED || page == MAP_FAILED) {
            why = "its dma-buf does not map (whole and at an offset)";
        } else {
            static_cast<volatile std::uint32_t*>(whole)[Offset / 4] = 0x5eedf00du;
            ok = static_cast<volatile std::uint32_t*>(page)[0] == 0x5eedf00du;
            why = ok ? "" : "its mappings do not see each other's writes";
        }
        if (whole != MAP_FAILED) munmap(whole, Size);
        if (page != MAP_FAILED) munmap(page, 4096);
    }
    if (fd >= 0) close(fd);
    if (imported) {
        device.freeMemory(memory);
        device.destroyBuffer(buffer);
    }
    return ok;
}

} // namespace BbGuestMemory::detail

#else

namespace BbGuestMemory::detail {
int AllocAhbChunk(u64, u64) {
    return -1;
}
bool AhbWorks(const Vulkan::Instance&, const char*& why) {
    why = "hardware buffers are Android's";
    return false;
}
} // namespace BbGuestMemory::detail

#endif
