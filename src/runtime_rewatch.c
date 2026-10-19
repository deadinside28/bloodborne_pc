/* Reverse engineering aids, off unless their variables are set: a hardware watchpoint on the
 * game's memory, on every thread, through perf_event breakpoints of this process (no ptrace),
 * counting the instructions that touch it by address, every 5 s. Set from the pad reads
 * (runtime_pad.c; BB_RE_WATCH=<offset from the game's ScePadData buffer>: + 0x9c the right
 * stick's X; BB_RE_WATCH_ADDR=<address>) or asked for by the GPU library's probes once they know an
 * address (runtime_re_watch; gpu/shim/bbport_guest_hooks.cpp, BB_RE_*). The native mouse look's
 * camera was found so. */
#define _GNU_SOURCE
#include <stdint.h>
#ifdef _WIN32
/* perf_event breakpoints are Linux's: none of these aids on Windows. */
void runtime_re_watch_now(uint64_t address) { (void)address; }
void runtime_re_watch_report(void) {}
void runtime_re_watch(uint64_t address) { (void)address; }
void runtime_re_watch_tick(const void *pad_buffer, uint64_t reads) { (void)pad_buffer; (void)reads; }
#else
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>
#include <dirent.h>
#include <linux/perf_event.h>
#include <linux/hw_breakpoint.h>
#include <sys/ioctl.h>
#include <sys/mman.h>
#include <sys/syscall.h>
#include <sys/types.h>

/* BB_RE_WATCH=<offset> (reverse engineering): a hardware read/write watchpoint, on every thread,
 * on the 4 bytes at the game's ScePadData buffer + offset (its pad device keeps the sticks as
 * floats past it: + 0x9c the right stick's X), set at the 3000th read. Each access leaves a
 * sample (the instruction after it) in the thread's ring buffer, drained at every pad read:
 * counts by address every 5 s. */
#define WATCH_SLOTS 256
#define WATCH_RING_PAGES 8
static uint64_t watch_rip[WATCH_SLOTS], watch_count[WATCH_SLOTS], watch_samples;
static uint64_t watch_last[WATCH_SLOTS][13]; /* per address: rdx rsi rdi rbp rsp, 8 stack qwords */
static struct perf_event_mmap_page *watch_rings[512];
static int watch_fds[512];
static int watch_ring_count;
static uint64_t watch_address, watch_follow_to;
static void watch_note(uint64_t rip, const uint64_t *last) {
    ++watch_samples;
    for (unsigned i=0;i<WATCH_SLOTS;++i) {
        const unsigned k=((unsigned)((rip*0x9E3779B97F4A7C15ull)>>56)+i) & (WATCH_SLOTS-1);
        if (watch_rip[k]==rip || !watch_rip[k]) {
            watch_rip[k]=rip; ++watch_count[k]; memcpy(watch_last[k],last,sizeof watch_last[k]);
            /* BB_RE_WATCH_FOLLOW=1: a host copy (libc's memcpy for the game) reading the watched
               bytes moves the watch to where they are copied (the same distance from rdi). */
            static int follow=-1;
            if (follow<0) { const char *e=getenv("BB_RE_WATCH_FOLLOW"); follow=e ? atoi(e) : 0; }
            if (follow && (rip>>40) && !watch_follow_to && last[1]!=last[2])
                watch_follow_to=watch_address+(last[2]-last[1]);
            return;
        }
    }
}
static void watch_stop(void) {
    for (int r=0;r<watch_ring_count;++r) { munmap(watch_rings[r],(1+WATCH_RING_PAGES)*4096); close(watch_fds[r]); }
    watch_ring_count=0;
    memset(watch_rip,0,sizeof watch_rip); memset(watch_count,0,sizeof watch_count);
}
static void watch_start(uint64_t address) {
    watch_address=address;
    struct perf_event_attr a; memset(&a,0,sizeof a);
    a.type=PERF_TYPE_BREAKPOINT; a.size=sizeof a;
    a.bp_type=HW_BREAKPOINT_RW; a.bp_addr=address; a.bp_len=HW_BREAKPOINT_LEN_4;
    a.sample_period=1; a.exclude_kernel=1; a.exclude_hv=1;
    a.sample_type=PERF_SAMPLE_IP|PERF_SAMPLE_REGS_USER|PERF_SAMPLE_STACK_USER;
    a.sample_regs_user=(1ull<<3)|(1ull<<4)|(1ull<<5)|(1ull<<6)|(1ull<<7); /* dx si di bp sp */
    a.sample_stack_user=64;
    DIR *dir=opendir("/proc/self/task");
    int threads=0;
    for (struct dirent *e; dir && (e=readdir(dir));) {
        if (e->d_name[0]=='.') continue;
        ++threads;
        const int fd=(int)syscall(SYS_perf_event_open,&a,(pid_t)atoi(e->d_name),-1,-1,0);
        if (fd<0 || watch_ring_count>=512) continue;
        void *ring=mmap(NULL,(1+WATCH_RING_PAGES)*4096,PROT_READ|PROT_WRITE,MAP_SHARED,fd,0);
        if (ring!=MAP_FAILED) { watch_fds[watch_ring_count]=fd; watch_rings[watch_ring_count++]=ring; }
        else close(fd);
    }
    if (dir) closedir(dir);
    printf("RE watch: %#llx on %d of %d threads\n",(unsigned long long)address,watch_ring_count,threads);
}
static void watch_drain(int force) {
    for (int r=0;r<watch_ring_count;++r) {
        struct perf_event_mmap_page *page=watch_rings[r];
        const uint8_t *data=(const uint8_t*)page+4096;
        const uint64_t size=WATCH_RING_PAGES*4096, head=__atomic_load_n(&page->data_head,__ATOMIC_ACQUIRE);
        uint64_t tail=page->data_tail;
        while (tail<head) {
            struct perf_event_header h;
            for (unsigned i=0;i<sizeof h;++i) ((uint8_t*)&h)[i]=data[(tail+i)%size];
            if (h.size==0) break;
            if (h.type==PERF_RECORD_SAMPLE) {
                /* ip, abi, 5 registers, stack size, 64 bytes, dynamic size */
                uint64_t w[17]={0};
                for (unsigned j=0;j<17;++j)
                    for (unsigned i=0;i<8;++i) ((uint8_t*)&w[j])[i]=data[(tail+sizeof h+8*j+i)%size];
                uint64_t last[13]={w[2],w[3],w[4],w[5],w[6],w[8],w[9],w[10],w[11],w[12],w[13],w[14],w[15]};
                watch_note(w[0],last);
            }
            tail+=h.size;
        }
        __atomic_store_n(&page->data_tail,head,__ATOMIC_RELEASE);
    }
    if (watch_follow_to && watch_follow_to!=watch_address) {
        static int followed, hops=-1;
        if (hops<0) { const char *e=getenv("BB_RE_WATCH_FOLLOW"); hops=e ? atoi(e) : 0; }
        if (followed++<hops) {
            printf("RE watch: copied from %#llx to %#llx: watching there\n",
                   (unsigned long long)watch_address,(unsigned long long)watch_follow_to);
            const uint64_t to=watch_follow_to;
            watch_stop(); watch_start(to); watch_follow_to=0;
            return;
        }
    }
    static time_t printed;
    const time_t now=time(NULL);
    if (!force && now-printed<5) return;
    printed=now;
    printf("RE watch %#llx: %llu samples; by the instruction after the access:",(unsigned long long)watch_address,(unsigned long long)watch_samples);
    for (unsigned i=0;i<WATCH_SLOTS;++i)
        if (watch_rip[i]) {
            printf(" %#llx:%llu",(unsigned long long)watch_rip[i],(unsigned long long)watch_count[i]); watch_count[i]=0;
            if (watch_rip[i]>>40) /* host code: its arguments and the return address */
            {
                printf(" [rdx %#llx rsi %#llx rdi %#llx rbp %#llx rsp %#llx; stack",
                       (unsigned long long)watch_last[i][0],(unsigned long long)watch_last[i][1],
                       (unsigned long long)watch_last[i][2],(unsigned long long)watch_last[i][3],
                       (unsigned long long)watch_last[i][4]);
                for (unsigned j=5;j<13;++j) printf(" %#llx",(unsigned long long)watch_last[i][j]);
                printf("]");
            }
        }
    printf("\n");
}


/* BB_TLS_WATCH (runtime_thread.c): a watchpoint at once, before anything reads the pad; and what
 * touched it, printed when the game faults. */
static int watch_started;
void runtime_re_watch_now(uint64_t address) {
    if (!watch_started) { watch_started=1; watch_start(address); }
}
void runtime_re_watch_report(void) { if (watch_started) watch_drain(1); }

/* The GPU library's probes ask for a watchpoint once they know an address. */
static uint64_t watch_requested;
void runtime_re_watch(uint64_t address) { if (!watch_requested) watch_requested=address; }

uint64_t bbgpu_camera_state(void);

void runtime_re_watch_tick(const void *pad_buffer, uint64_t reads) {
    /* BB_RE_WATCH_CAMSTATE=1: the current camera's aspect ratio (+0x54): who reads it. */
    static int camstate=-1;
    if (camstate<0) { const char *e=getenv("BB_RE_WATCH_CAMSTATE"); camstate=e && e[0]=='1'; }
    if (camstate==1 && !watch_requested && bbgpu_camera_state()) runtime_re_watch(bbgpu_camera_state()+0x54);
    /* BB_RE_WATCH_AT=<reads>: not before that many pad reads (threads made later, entering the
       world, are not watched). */
    static long watch_at=-1;
    if (watch_at<0) { const char *e=getenv("BB_RE_WATCH_AT"); watch_at=e ? atol(e) : 3000; }
    if (watch_requested && !watch_started && (long)reads>=watch_at) { watch_started=1; watch_start(watch_requested); }
    if (watch_started) { watch_drain(0); return; }
    static const char *watch_env, *watch_addr; static int watch_checked;
    if (!watch_checked) { watch_env=getenv("BB_RE_WATCH"); watch_addr=getenv("BB_RE_WATCH_ADDR"); watch_checked=1; }
    if (!watch_env && !watch_addr) return;
    if ((long)reads==watch_at) {
        watch_start(watch_addr ? strtoull(watch_addr,NULL,0)
                               : (uint64_t)(uintptr_t)pad_buffer+strtoull(watch_env,NULL,0));
        watch_started=1;
    }
}
#endif
