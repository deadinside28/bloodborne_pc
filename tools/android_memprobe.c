/* SPDX-FileCopyrightText: Copyright 2026 bbport contributors
 * SPDX-License-Identifier: GPL-2.0-or-later
 * Can the game's memory live in Android hardware buffers? bbport lets the GPU use the game's
 * memory in place; on PC through dma-buf export or VK_EXT_external_memory_host, which Adreno's
 * driver lacks. This checks the Android way, step by step, for a few sizes:
 *   1. AHardwareBuffer_allocate (BLOB, CPU read/write often, GPU data buffer)
 *   2. its dma-buf fd (AHardwareBuffer_sendHandleToUnixSocket over a socketpair)
 *   3. mmap of that fd at a fixed address in the game's range (MAP_SHARED | MAP_FIXED_NOREPLACE)
 *   4. Vulkan: the buffer imported (VK_ANDROID_external_memory_android_hardware_buffer) as a
 *      VkBuffer
 *   5. GPU writes (vkCmdFillBuffer) seen by the CPU through the fixed mapping, CPU writes seen by
 *      the GPU (vkCmdCopyBuffer into a second region, read back the same way)
 * Build and run: tools/android_memprobe.sh (NDK, adb). */
#define _GNU_SOURCE
#include <android/hardware_buffer.h>
#include <errno.h>
#include <inttypes.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/mman.h>
#include <sys/socket.h>
#include <unistd.h>
#define VK_USE_PLATFORM_ANDROID_KHR
#include <vulkan/vulkan.h>

#define CHECK(x, ...) do { if (!(x)) { printf("  FAIL " __VA_ARGS__); printf("\n"); return -1; } } while (0)

static VkInstance instance;
static VkPhysicalDevice gpu;
static VkDevice device;
static VkQueue queue;
static uint32_t queue_family;
static PFN_vkGetAndroidHardwareBufferPropertiesANDROID get_ahb_properties;

static int has_extension(const VkExtensionProperties *list, uint32_t n, const char *name) {
    for (uint32_t i = 0; i < n; ++i)
        if (!strcmp(list[i].extensionName, name)) return 1;
    return 0;
}

static int vulkan_init(void) {
    const VkApplicationInfo app = { VK_STRUCTURE_TYPE_APPLICATION_INFO, NULL, "memprobe", 1, NULL, 0, VK_API_VERSION_1_3 };
    const VkInstanceCreateInfo ici = { VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO, NULL, 0, &app, 0, NULL, 0, NULL };
    CHECK(vkCreateInstance(&ici, NULL, &instance) == VK_SUCCESS, "vkCreateInstance");
    uint32_t n = 1;
    vkEnumeratePhysicalDevices(instance, &n, &gpu);
    CHECK(n == 1, "no GPU");
    VkPhysicalDeviceProperties props;
    vkGetPhysicalDeviceProperties(gpu, &props);
    printf("GPU: %s, driver %#x, Vulkan %u.%u.%u\n", props.deviceName, props.driverVersion,
           VK_API_VERSION_MAJOR(props.apiVersion), VK_API_VERSION_MINOR(props.apiVersion), VK_API_VERSION_PATCH(props.apiVersion));
    uint32_t ne = 0;
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &ne, NULL);
    VkExtensionProperties *ext = calloc(ne, sizeof(*ext));
    vkEnumerateDeviceExtensionProperties(gpu, NULL, &ne, ext);
    static const char *interesting[] = {
        "VK_ANDROID_external_memory_android_hardware_buffer", "VK_EXT_external_memory_host",
        "VK_EXT_external_memory_dma_buf", "VK_KHR_external_memory_fd", "VK_EXT_queue_family_foreign",
        "VK_EXT_image_drm_format_modifier", "VK_KHR_buffer_device_address", "VK_EXT_descriptor_buffer",
        "VK_KHR_external_memory", "VK_EXT_memory_budget", "VK_EXT_pageable_device_local_memory" };
    for (size_t i = 0; i < sizeof(interesting) / sizeof(*interesting); ++i)
        printf("  %-55s %s\n", interesting[i], has_extension(ext, ne, interesting[i]) ? "yes" : "no");
    CHECK(has_extension(ext, ne, "VK_ANDROID_external_memory_android_hardware_buffer"), "no AHB import");
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    for (uint32_t i = 0; i < mp.memoryTypeCount; ++i)
        printf("  memory type %u: flags %#x, heap %u (%" PRIu64 " MiB)\n", i, mp.memoryTypes[i].propertyFlags,
               mp.memoryTypes[i].heapIndex, mp.memoryHeaps[mp.memoryTypes[i].heapIndex].size >> 20);
    uint32_t nq = 0;
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, NULL);
    VkQueueFamilyProperties *qf = calloc(nq, sizeof(*qf));
    vkGetPhysicalDeviceQueueFamilyProperties(gpu, &nq, qf);
    for (queue_family = 0; queue_family < nq && !(qf[queue_family].queueFlags & VK_QUEUE_TRANSFER_BIT); ++queue_family) {}
    const float priority = 1;
    const VkDeviceQueueCreateInfo qci = { VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO, NULL, 0, queue_family, 1, &priority };
    const char *enable[3];
    uint32_t n_enable = 0;
    enable[n_enable++] = "VK_ANDROID_external_memory_android_hardware_buffer";
    if (has_extension(ext, ne, "VK_KHR_external_memory_fd")) enable[n_enable++] = "VK_KHR_external_memory_fd";
    if (has_extension(ext, ne, "VK_EXT_queue_family_foreign")) enable[n_enable++] = "VK_EXT_queue_family_foreign";
    const VkDeviceCreateInfo dci = { VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, NULL, 0, 1, &qci, 0, NULL,
                                     n_enable, enable, NULL };
    CHECK(vkCreateDevice(gpu, &dci, NULL, &device) == VK_SUCCESS, "vkCreateDevice");
    vkGetDeviceQueue(device, queue_family, 0, &queue);
    get_ahb_properties = (PFN_vkGetAndroidHardwareBufferPropertiesANDROID)vkGetDeviceProcAddr(device, "vkGetAndroidHardwareBufferPropertiesANDROID");
    CHECK(get_ahb_properties, "vkGetAndroidHardwareBufferPropertiesANDROID");
    return 0;
}

/* The dma-buf fd behind a hardware buffer: its native handle sent over a socket. */
static int buffer_fd(AHardwareBuffer *ahb) {
    int sv[2];
    if (socketpair(AF_UNIX, SOCK_SEQPACKET, 0, sv)) return -1;
    if (AHardwareBuffer_sendHandleToUnixSocket(ahb, sv[0])) { close(sv[0]); close(sv[1]); return -2; }
    char data[1024];
    char control[CMSG_SPACE(sizeof(int) * 32)];
    struct iovec iov = { data, sizeof(data) };
    struct msghdr msg = { NULL, 0, &iov, 1, control, sizeof(control), 0 };
    const ssize_t got = recvmsg(sv[1], &msg, 0);
    close(sv[0]); close(sv[1]);
    if (got < 0) return -3;
    int first = -4;
    for (struct cmsghdr *c = CMSG_FIRSTHDR(&msg); c; c = CMSG_NXTHDR(&msg, c)) {
        if (c->cmsg_level != SOL_SOCKET || c->cmsg_type != SCM_RIGHTS) continue;
        const int n = (int)((c->cmsg_len - CMSG_LEN(0)) / sizeof(int));
        const int *fds = (const int *)CMSG_DATA(c);
        printf("  native handle: %d fd(s), %zd bytes of data\n", n, got);
        for (int i = 0; i < n; ++i) {
            if (first < 0) first = fds[i];
            else close(fds[i]);
        }
    }
    return first;
}

/* The GPU fills the first quarter of `buffer` and copies a pattern the CPU wrote in the second
 * half to the third quarter; both checked through the CPU's mapping `cpu`. */
static int exercise(VkBuffer buffer, uint64_t size, volatile uint32_t *cpu) {
    const VkCommandPoolCreateInfo pci = { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO, NULL, 0, queue_family };
    VkCommandPool pool;
    vkCreateCommandPool(device, &pci, NULL, &pool);
    const VkCommandBufferAllocateInfo cai = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO, NULL, pool, VK_COMMAND_BUFFER_LEVEL_PRIMARY, 1 };
    VkCommandBuffer cmd;
    vkAllocateCommandBuffers(device, &cai, &cmd);
    // the CPU writes a pattern in the second half; the GPU fills the first quarter and copies the
    // pattern to the third quarter
    const uint64_t q = size / 4;
    for (uint64_t i = 0; i < 1024; ++i) cpu[(2 * q) / 4 + i] = (uint32_t)(0xc0de0000u + i);
    const VkCommandBufferBeginInfo bi = { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO, NULL, VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT, NULL };
    vkBeginCommandBuffer(cmd, &bi);
    const VkMemoryBarrier host_to_gpu = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_HOST_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_HOST_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &host_to_gpu, 0, NULL, 0, NULL);
    vkCmdFillBuffer(cmd, buffer, 0, q, 0x600dbeefu);
    const VkBufferCopy copy = { 2 * q, 3 * q, 4096 };
    vkCmdCopyBuffer(cmd, buffer, buffer, 1, &copy);
    const VkMemoryBarrier gpu_to_host = { VK_STRUCTURE_TYPE_MEMORY_BARRIER, NULL, VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_HOST_READ_BIT };
    vkCmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 1, &gpu_to_host, 0, NULL, 0, NULL);
    vkEndCommandBuffer(cmd);
    const VkSubmitInfo si = { VK_STRUCTURE_TYPE_SUBMIT_INFO, NULL, 0, NULL, NULL, 1, &cmd, 0, NULL };
    CHECK(vkQueueSubmit(queue, 1, &si, VK_NULL_HANDLE) == VK_SUCCESS, "submit");
    vkQueueWaitIdle(queue);
    uint64_t bad_fill = 0, bad_copy = 0;
    for (uint64_t i = 0; i < q / 4; i += 997) bad_fill += cpu[i] != 0x600dbeefu;
    for (uint64_t i = 0; i < 1024; ++i) bad_copy += cpu[(3 * q) / 4 + i] != (uint32_t)(0xc0de0000u + i);
    printf("  GPU fill seen by the CPU through the fixed mapping: %s; CPU writes seen by the GPU: %s\n",
           bad_fill ? "NO" : "yes", bad_copy ? "NO" : "yes");
    vkDestroyCommandPool(device, pool, NULL);
    return bad_fill || bad_copy ? -1 : 0;
}

static int gpu_round_trip(AHardwareBuffer *ahb, uint64_t size, volatile uint32_t *cpu) {
    VkAndroidHardwareBufferPropertiesANDROID ap = { .sType = VK_STRUCTURE_TYPE_ANDROID_HARDWARE_BUFFER_PROPERTIES_ANDROID };
    CHECK(get_ahb_properties(device, ahb, &ap) == VK_SUCCESS, "AHB properties");
    printf("  Vulkan: allocation %" PRIu64 " bytes, memory types %#x\n", ap.allocationSize, ap.memoryTypeBits);
    const VkExternalMemoryBufferCreateInfo emb = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_ANDROID_HARDWARE_BUFFER_BIT_ANDROID };
    const VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &emb, 0, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer) == VK_SUCCESS, "vkCreateBuffer (external)");
    const VkImportAndroidHardwareBufferInfoANDROID import = { VK_STRUCTURE_TYPE_IMPORT_ANDROID_HARDWARE_BUFFER_INFO_ANDROID, NULL, ahb };
    const VkMemoryDedicatedAllocateInfo dedicated = { VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO, &import, VK_NULL_HANDLE, buffer };
    uint32_t type = 0;
    while (type < 32 && !(ap.memoryTypeBits & (1u << type))) ++type;
    const VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated, ap.allocationSize, type };
    VkDeviceMemory memory;
    VkResult r = vkAllocateMemory(device, &mai, NULL, &memory);
    if (r != VK_SUCCESS) {  // without the dedicated allocation
        const VkMemoryAllocateInfo plain = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &import, ap.allocationSize, type };
        r = vkAllocateMemory(device, &plain, NULL, &memory);
    }
    CHECK(r == VK_SUCCESS, "import (vkAllocateMemory %d)", r);
    CHECK(vkBindBufferMemory(device, buffer, memory, 0) == VK_SUCCESS, "vkBindBufferMemory");
    const int checked = exercise(buffer, size, cpu);
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    return checked;
}

static int probe(uint64_t size, uint64_t at) {
    printf("%" PRIu64 " MiB at %#" PRIx64 ":\n", size >> 20, at);
    const AHardwareBuffer_Desc desc = { (uint32_t)size, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
        AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, 0, 0, 0 };
    CHECK(size <= UINT32_MAX, "a BLOB's width is 32 bits: %" PRIu64 " MiB is too large for one buffer", size >> 20);
    AHardwareBuffer *ahb = NULL;
    const int a = AHardwareBuffer_allocate(&desc, &ahb);
    CHECK(a == 0 && ahb, "AHardwareBuffer_allocate (%d)", a);
    const int fd = buffer_fd(ahb);
    CHECK(fd >= 0, "no fd from the native handle (%d)", fd);
    void *p = mmap((void *)(uintptr_t)at, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
    CHECK(p == (void *)(uintptr_t)at, "mmap at the fixed address (%s)", p == MAP_FAILED ? strerror(errno) : "elsewhere");
    printf("  mapped at the game's address\n");
    ((volatile uint32_t *)p)[0] = 1;
    const int r = gpu_round_trip(ahb, size, p);
    munmap(p, size);
    close(fd);
    AHardwareBuffer_release(ahb);
    return r;
}

/* MemAvailable from /proc/meminfo, in bytes (0: unknown). */
static uint64_t available(void) {
    FILE *f = fopen("/proc/meminfo", "r");
    char line[256];
    uint64_t kib = 0;
    while (f && fgets(line, sizeof(line), f))
        if (sscanf(line, "MemAvailable: %" SCNu64 " kB", &kib) == 1) break;
    if (f) fclose(f);
    return kib << 10;
}

/* How much: buffers of `size` allocated (and kept), at most 9 GiB, and never while fewer than
 * 2 GiB would stay available to the rest of the phone (an uncapped run of this exhausted a phone's
 * memory and restarted it). */
static void total(uint64_t size) {
    const uint64_t margin = 2ull << 30;
    const AHardwareBuffer_Desc desc = { (uint32_t)size, 1, 1, AHARDWAREBUFFER_FORMAT_BLOB,
        AHARDWAREBUFFER_USAGE_CPU_READ_OFTEN | AHARDWAREBUFFER_USAGE_CPU_WRITE_OFTEN |
        AHARDWAREBUFFER_USAGE_GPU_DATA_BUFFER, 0, 0, 0 };
    static AHardwareBuffer *kept[256];
    const int most = (int)((9ull << 30) / size);  /* the game's direct memory at most (BB_DMEM_MB 9152) */
    int n = 0;
    const char *stop = "all asked for";
    while (n < most && n < 256) {
        const uint64_t free_now = available();
        if (!free_now || free_now < margin + size) { stop = "stopped: 2 GiB must stay available"; break; }
        if (AHardwareBuffer_allocate(&desc, &kept[n])) { stop = "then one was refused"; break; }
        ++n;
    }
    printf("available afterwards: %" PRIu64 " MiB\n", available() >> 20);
    printf("total: %d buffers of %" PRIu64 " MiB (%" PRIu64 " MiB), %s\n", n, size >> 20, (uint64_t)n * (size >> 20), stop);
    while (n) AHardwareBuffer_release(kept[--n]);
}

/* Vulkan memory exported as an opaque fd (VK_KHR_external_memory_fd): can that fd be mapped at the
 * game's address (and at an offset), and is it shared with the GPU? Any size: the PC's dma-buf
 * model with one export per direct allocation. Never while fewer than size + 2 GiB are available. */
static int opaque(uint64_t size, uint64_t at) {
    printf("opaque fd, %" PRIu64 " MiB at %#" PRIx64 ":\n", size >> 20, at);
    const uint64_t free_now = available();
    CHECK(free_now >= size + (2ull << 30), "skipped: %" PRIu64 " MiB available, %" PRIu64 " MiB + 2 GiB needed",
          free_now >> 20, size >> 20);
    PFN_vkGetMemoryFdKHR get_fd = (PFN_vkGetMemoryFdKHR)vkGetDeviceProcAddr(device, "vkGetMemoryFdKHR");
    CHECK(get_fd, "no vkGetMemoryFdKHR");
    const VkExternalMemoryBufferCreateInfo emb = { VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_BUFFER_CREATE_INFO, NULL,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    const VkBufferCreateInfo bci = { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &emb, 0, size,
        VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT | VK_BUFFER_USAGE_STORAGE_BUFFER_BIT,
        VK_SHARING_MODE_EXCLUSIVE, 0, NULL };
    VkBuffer buffer;
    CHECK(vkCreateBuffer(device, &bci, NULL, &buffer) == VK_SUCCESS, "vkCreateBuffer (opaque fd)");
    VkMemoryRequirements req;
    vkGetBufferMemoryRequirements(device, buffer, &req);
    VkPhysicalDeviceMemoryProperties mp;
    vkGetPhysicalDeviceMemoryProperties(gpu, &mp);
    uint32_t type = ~0u;
    for (int cached = 1; cached >= 0 && type == ~0u; --cached)
        for (uint32_t i = 0; i < mp.memoryTypeCount && type == ~0u; ++i) {
            const VkMemoryPropertyFlags f = mp.memoryTypes[i].propertyFlags;
            if ((req.memoryTypeBits & (1u << i)) && (f & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT) &&
                (!cached || (f & VK_MEMORY_PROPERTY_HOST_CACHED_BIT)))
                type = i;
        }
    CHECK(type != ~0u, "no host-visible type for an exportable buffer (bits %#x)", req.memoryTypeBits);
    const VkExportMemoryAllocateInfo export_info = { VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO, NULL,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    const VkMemoryAllocateInfo mai = { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &export_info, req.size, type };
    VkDeviceMemory memory;
    const VkResult ar = vkAllocateMemory(device, &mai, NULL, &memory);
    CHECK(ar == VK_SUCCESS, "vkAllocateMemory (exportable, type %u: %d)", type, ar);
    vkBindBufferMemory(device, buffer, memory, 0);
    const VkMemoryGetFdInfoKHR gfi = { VK_STRUCTURE_TYPE_MEMORY_GET_FD_INFO_KHR, NULL, memory,
        VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT };
    int fd = -1;
    const VkResult fr = get_fd(device, &gfi, &fd);
    int result = -1;
    if (fr != VK_SUCCESS || fd < 0) {
        printf("  FAIL vkGetMemoryFdKHR (%d)\n", fr);
    } else {
        char link[256] = "", target[256];
        snprintf(link, sizeof(link), "/proc/self/fd/%d", fd);
        const ssize_t tl = readlink(link, target, sizeof(target) - 1);
        target[tl > 0 ? tl : 0] = 0;
        printf("  exported (type %u): fd -> %s\n", type, target);
        void *p = mmap((void *)(uintptr_t)at, size, PROT_READ | PROT_WRITE, MAP_SHARED | MAP_FIXED_NOREPLACE, fd, 0);
        void *page = mmap(NULL, 4096, PROT_READ | PROT_WRITE, MAP_SHARED, fd, (off_t)(size / 2));
        printf("  mapped at the game's address: %s; a page at an offset: %s\n",
               p == (void *)(uintptr_t)at ? "yes" : strerror(errno), page != MAP_FAILED ? "yes" : "no");
        if (p == (void *)(uintptr_t)at) {
            result = exercise(buffer, size, p);
            if (page != MAP_FAILED) {
                ((volatile uint32_t *)p)[size / 8] = 0x5eedf00du;
                printf("  the offset page sees writes through the other mapping: %s\n",
                       ((volatile uint32_t *)page)[0] == 0x5eedf00du ? "yes" : "NO");
            }
            munmap(p, size);
        }
        if (page != MAP_FAILED) munmap(page, 4096);
        close(fd);
    }
    vkDestroyBuffer(device, buffer, NULL);
    vkFreeMemory(device, memory, NULL);
    return result;
}

/* memprobe [MiB...]: each size end to end; memprobe total MiB: how many fit; memprobe opaque MiB...:
 * exported Vulkan memory instead (at most 3 GiB each) */
int main(int argc, char **argv) {
    if (argc == 3 && !strcmp(argv[1], "total")) { total(strtoull(argv[2], NULL, 0) << 20); return 0; }
    if (vulkan_init()) return 1;
    int failures = 0;
    if (argc > 2 && !strcmp(argv[1], "opaque")) {
        for (int i = 2; i < argc; ++i) {
            const uint64_t mib = strtoull(argv[i], NULL, 0);
            if (mib > 3072) { printf("opaque: %" PRIu64 " MiB is over the 3 GiB limit\n", mib); continue; }
            failures += opaque(mib << 20, 0x1000000000ull) != 0;
        }
        return failures != 0;
    }
    uint64_t sizes[16] = { 64ull << 20, 1024ull << 20, 4095ull << 20 };
    int count = 3;
    if (argc > 1) for (count = 0; count < argc - 1 && count < 16; ++count) sizes[count] = strtoull(argv[count + 1], NULL, 0) << 20;
    for (int i = 0; i < count; ++i) failures += probe(sizes[i], 0x1000000000ull) != 0;
    printf("%s\n", failures ? "memprobe: some steps failed" : "memprobe: hardware buffers can hold the game's memory in place");
    return failures != 0;
}
