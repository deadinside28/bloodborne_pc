/* What other games need from the system beyond Bloodborne's set (first: another game).
 *   - kernel event flags (64-bit patterns, AND/OR waits, clear on wake, timeouts);
 *   - time zone conversions, UUIDs, memory locking hints;
 *   - the online services Bloodborne does not use (TUS, word filter, in-game messages, SNS,
 *     bandwidth test): signed out, as the rest of PSN here;
 *   - the common dialogs it does not use: they finish at once, cancelled by the user.
 * Every entry is an explicit contract; unknown functions still stop. */
#define _GNU_SOURCE
#include "runtime.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <errno.h>
#include <pthread.h>
#include <sys/random.h>
#include <time.h>

#define ERR(n) ((int32_t)(UINT32_C(0x80020000) | (n)))
#define KERNEL_EINVAL ERR(22)
#define KERNEL_ESRCH ERR(3)
#define KERNEL_EBUSY ERR(16)
#define KERNEL_ETIMEDOUT ERR(60)
#define KERNEL_ECANCELED ERR(85)
#define KERNEL_EACCES ERR(13)
#define NP_SIGNED_OUT ((int32_t)0x80550006)
#define NET_CTL_NOT_CONNECTED ((int32_t)0x80412108)
#define NET_ENOTCONN ((int32_t)0x80410139)
#define HTTP_NETWORK ((int32_t)0x80431063)
#define APPCONTENT_NOT_FOUND ((int32_t)0x80D90005)
#define SSL_NOT_FOUND ((int32_t)0x8095F00B)
#define RTC_INVALID ((int32_t)0x80B50002)
#define TROPHY_INVALID ((int32_t)0x80551604)

static ABI int32_t ok(void) { return 0; }

/* ---- Event flags ---------------------------------------------------------------------- */

enum { WAIT_AND = 0x01, WAIT_OR = 0x02, CLEAR_ALL = 0x10, CLEAR_PAT = 0x20 };

typedef struct EventFlag {
    pthread_mutex_t lock;
    pthread_cond_t changed;
    uint64_t pattern;
    uint32_t attr;
    unsigned waiters;
    uint64_t generation; ///< bumped by cancel and delete: waiters leave with an error
    int deleted;
    char name[32];
} EventFlag;

static size_t flags_created, flag_waits;

static int matches(uint64_t pattern, uint64_t bits, uint32_t mode) {
    return (mode & WAIT_AND) ? (pattern & bits) == bits : (pattern & bits) != 0;
}

static ABI int32_t evf_create(EventFlag **out, const char *name, uint32_t attr, uint64_t init,
                              const void *options) {
    (void)options;
    if (!out) return KERNEL_EINVAL;
    EventFlag *f = calloc(1, sizeof(*f));
    if (!f) return ERR(12);
    pthread_mutex_init(&f->lock, NULL);
    pthread_condattr_t ca;
    pthread_condattr_init(&ca);
    pthread_condattr_setclock(&ca, RUNTIME_COND_CLOCK);
    pthread_cond_init(&f->changed, &ca);
    pthread_condattr_destroy(&ca);
    f->pattern = init;
    f->attr = attr;
    if (name) snprintf(f->name, sizeof(f->name), "%s", name);
    *out = f;
    __atomic_add_fetch(&flags_created, 1, __ATOMIC_RELAXED);
    return 0;
}

static ABI int32_t evf_delete(EventFlag *f) {
    if (!f) return KERNEL_ESRCH;
    pthread_mutex_lock(&f->lock);
    f->deleted = 1;
    ++f->generation;
    const unsigned waiting = f->waiters;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    if (!waiting) { /* waiters free nothing; a flag deleted under them is leaked (rare) */
        pthread_cond_destroy(&f->changed);
        pthread_mutex_destroy(&f->lock);
        free(f);
    }
    return 0;
}

static ABI int32_t evf_set(EventFlag *f, uint64_t bits) {
    if (!f) return KERNEL_ESRCH;
    pthread_mutex_lock(&f->lock);
    f->pattern |= bits;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    return 0;
}

static ABI int32_t evf_clear(EventFlag *f, uint64_t bits) {
    if (!f) return KERNEL_ESRCH;
    pthread_mutex_lock(&f->lock);
    f->pattern &= bits; /* the bits kept, as the kernel's clear takes them */
    pthread_mutex_unlock(&f->lock);
    return 0;
}

static int32_t evf_take(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result) {
    if (result) *result = f->pattern;
    if (mode & CLEAR_ALL) f->pattern = 0;
    else if (mode & CLEAR_PAT) f->pattern &= ~bits;
    return 0;
}

static ABI int32_t evf_wait(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result,
                            uint32_t *timeout_us) {
    if (!f) return KERNEL_ESRCH;
    if (!bits || !(mode & (WAIT_AND | WAIT_OR)) || (mode & WAIT_AND && mode & WAIT_OR))
        return KERNEL_EINVAL;
    pthread_mutex_lock(&f->lock);
    struct timespec deadline;
    if (timeout_us) {
        clock_gettime(RUNTIME_COND_CLOCK, &deadline);
        const uint64_t ns = (uint64_t)deadline.tv_nsec + (uint64_t)*timeout_us * 1000;
        deadline.tv_sec += (time_t)(ns / 1000000000);
        deadline.tv_nsec = (long)(ns % 1000000000);
    }
    const uint64_t generation = f->generation;
    const uint64_t start = runtime_wait_clock();
    int32_t status = 0;
    ++f->waiters;
    while (!matches(f->pattern, bits, mode)) {
        if (f->generation != generation) {
            status = f->deleted ? KERNEL_EACCES : KERNEL_ECANCELED;
            break;
        }
        const int e = timeout_us ? pthread_cond_timedwait(&f->changed, &f->lock, &deadline)
                                 : pthread_cond_wait(&f->changed, &f->lock);
        if (e == ETIMEDOUT && !matches(f->pattern, bits, mode)) {
            status = KERNEL_ETIMEDOUT;
            break;
        }
    }
    --f->waiters;
    if (status == 0) evf_take(f, bits, mode, result);
    else if (result) *result = f->pattern;
    if (timeout_us) {
        const uint64_t waited_us = (runtime_wait_clock() - start) / 1000;
        *timeout_us = status == KERNEL_ETIMEDOUT || waited_us >= *timeout_us
                          ? 0 : *timeout_us - (uint32_t)waited_us;
    }
    const int free_it = f->deleted && !f->waiters;
    pthread_mutex_unlock(&f->lock);
    if (free_it) {
        pthread_cond_destroy(&f->changed);
        pthread_mutex_destroy(&f->lock);
        free(f);
    }
    __atomic_add_fetch(&flag_waits, 1, __ATOMIC_RELAXED);
    runtime_wait_note(2, runtime_wait_clock() - start);
    return status;
}

static ABI int32_t evf_poll(EventFlag *f, uint64_t bits, uint32_t mode, uint64_t *result) {
    if (!f) return KERNEL_ESRCH;
    if (!bits || !(mode & (WAIT_AND | WAIT_OR))) return KERNEL_EINVAL;
    pthread_mutex_lock(&f->lock);
    int32_t status = matches(f->pattern, bits, mode) ? evf_take(f, bits, mode, result) : KERNEL_EBUSY;
    if (status && result) *result = f->pattern;
    pthread_mutex_unlock(&f->lock);
    return status;
}

static ABI int32_t evf_cancel(EventFlag *f, uint64_t pattern, int32_t *waiting) {
    if (!f) return KERNEL_ESRCH;
    pthread_mutex_lock(&f->lock);
    if (waiting) *waiting = (int32_t)f->waiters;
    f->pattern = pattern;
    ++f->generation;
    pthread_cond_broadcast(&f->changed);
    pthread_mutex_unlock(&f->lock);
    return 0;
}

/* ---- Time zones, UUIDs, memory hints ---------------------------------------------------- */

typedef struct { int64_t t; uint32_t west_sec, dst_sec; } Timesec;
typedef struct { int32_t minutes_west, dst_time; } Timezone;

static long host_offset(time_t at, int *dst) {
    struct tm local;
    localtime_r(&at, &local);
    if (dst) *dst = local.tm_isdst > 0;
    return RUNTIME_TM_GMTOFF(local); /* east of UTC, seconds */
}

static ABI int32_t utc_to_local(int64_t utc, int64_t *local, Timesec *st, uint64_t *dst_sec) {
    int dst = 0;
    const long offset = host_offset((time_t)utc, &dst);
    if (local) *local = utc + offset;
    if (st) { st->t = utc; st->west_sec = (uint32_t)(offset - (dst ? 3600 : 0)); st->dst_sec = dst ? 3600 : 0; }
    if (dst_sec) *dst_sec = dst ? 3600 : 0;
    return 0;
}

static ABI int32_t local_to_utc(int64_t local, int64_t unused, int64_t *utc, Timezone *tz,
                                int32_t *dst_sec) {
    (void)unused;
    int dst = 0;
    const long offset = host_offset((time_t)local, &dst);
    if (utc) *utc = local - offset;
    if (tz) { tz->minutes_west = (int32_t)(-offset / 60); tz->dst_time = dst; }
    if (dst_sec) *dst_sec = dst ? 3600 : 0;
    return 0;
}

static ABI int32_t uuid_create(uint8_t *uuid) {
    if (!uuid) return KERNEL_EINVAL;
    if (getrandom(uuid, 16, 0) != 16) for (int i = 0; i < 16; ++i) uuid[i] = (uint8_t)rand();
    uuid[6] = (uint8_t)((uuid[6] & 0x0f) | 0x40); /* version 4 */
    uuid[8] = (uint8_t)((uuid[8] & 0x3f) | 0x80);
    return 0;
}

/* Condition variable attributes: the runtime's conditions take none (CLOCK_REALTIME waits). */
static ABI int32_t condattr_init(void **attr) {
    if (!attr) return KERNEL_EINVAL;
    *attr = calloc(1, 16);
    return *attr ? 0 : ERR(12);
}
static ABI int32_t condattr_destroy(void **attr) {
    if (!attr) return KERNEL_EINVAL;
    free(*attr);
    *attr = NULL;
    return 0;
}

/* ---- Dialogs that finish at once (cancelled) ------------------------------------------------ */

enum { DIALOG_FINISHED = 3, DIALOG_RESULT_CANCELED = 1 };
static ABI int32_t dialog_finished(void) { return DIALOG_FINISHED; }
static ABI int32_t dialog_cancelled(int32_t *result) { if (result) *result = DIALOG_RESULT_CANCELED; return 0; }
/* SceSaveDataDialogResult: mode, result (0: OK), button. */
static ABI int32_t save_dialog_result(int32_t *result) {
    if (!result) return KERNEL_EINVAL;
    result[1] = 0;
    result[2] = 0;
    return 0;
}

/* ---- Small services -------------------------------------------------------------------------- */

static ABI int32_t np_signed_out(void) { return NP_SIGNED_OUT; }
static ABI int32_t netctl_not_connected(void) { return NET_CTL_NOT_CONNECTED; }
static ABI int32_t net_not_connected(void) { return NET_ENOTCONN; }
static ABI int32_t http_unavailable(void) { return HTTP_NETWORK; }
static ABI int32_t appcontent_not_found(void) { return APPCONTENT_NOT_FOUND; }
static ABI int32_t ssl_not_found(void) { return SSL_NOT_FOUND; }
static ABI int32_t rtc_invalid(void) { return RTC_INVALID; }
static ABI int32_t trophy_invalid(void) { return TROPHY_INVALID; }
static ABI int32_t safe_area(float *info) { if (!info) return KERNEL_EINVAL; info[0] = 1.0f; return 0; }
static ABI int32_t user_color(int32_t user, int32_t *color) { (void)user; if (color) *color = 0; return 0; }

/* PlayGo: everything installed; progress complete. */
typedef struct { uint64_t progress, total; } PlayGoProgress;
static ABI int32_t playgo_progress(uint32_t handle, const uint16_t *chunks, uint32_t count,
                                   PlayGoProgress *out) {
    (void)handle; (void)chunks; (void)count;
    if (!out) return KERNEL_EINVAL;
    out->progress = out->total = 1;
    return 0;
}

/* Save data: plenty of room; parameters read as empty. */
typedef struct { uint64_t blocks, free_blocks; uint8_t reserved[32]; } SaveMountInfo;
static ABI int32_t save_mount_info(const void *mount, SaveMountInfo *info) {
    (void)mount;
    if (!info) return KERNEL_EINVAL;
    memset(info, 0, sizeof(*info));
    info->blocks = 1u << 20;
    info->free_blocks = 1u << 19;
    return 0;
}
static ABI int32_t save_get_param(const void *mount, int32_t type, void *buffer, uint64_t size,
                                  uint64_t *got) {
    (void)mount; (void)type;
    if (buffer) memset(buffer, 0, size);
    if (got) *got = size;
    return 0;
}

/* Temporary data: /temp0 in the user folder. */
static ABI int32_t temporary_mount(int32_t option, char *mount_point) {
    (void)option;
    if (!mount_point) return KERNEL_EINVAL;
    char host[1024];
    snprintf(host, sizeof(host), "%s/temp0", runtime_file_user_dir());
    runtime_file_mount("/temp0", host);
    strcpy(mount_point, "/temp0");
    return 0;
}

/* ---- What another game asks beyond the others ------------------------------------------- */

#define VIDEODEC_UNSUPPORTED ((int32_t)0x80C10003) /* no hardware decoder here (yet) */
#define DISC_MAP_NO_BITMAP ((int32_t)0x81100004)

/* A base PS4, without the Pro's mode or a partially resident texture aperture. */
static ABI int32_t not_neo(void) { return 0; }
static ABI int32_t prt_aperture(int32_t index, uint64_t *address, uint64_t *size) {
    (void)index;
    if (address) *address = 0;
    if (size) *size = 0;
    return 0;
}
/* System strings (the console's name and the like): empty. */
static ABI int32_t system_param_string(int32_t id, char *buffer, uint64_t size) {
    (void)id;
    if (!buffer || !size) return KERNEL_EINVAL;
    buffer[0] = 0;
    return 0;
}
/* PlayGo: everything is installed, nothing to fetch. */
static ABI int32_t playgo_eta(uint32_t handle, const uint16_t *chunks, uint32_t count, int64_t *eta) {
    (void)handle; (void)chunks; (void)count;
    if (eta) *eta = 0;
    return 0;
}
static ABI int32_t playgo_todo(uint32_t handle, void *list, uint32_t count, uint32_t *out_count) {
    (void)handle; (void)list; (void)count;
    if (out_count) *out_count = 0;
    return 0;
}
/* Callbacks registered with services that never call them (signed out): a valid id. */
static ABI int32_t callback_id(void) { return 1; }
static ABI int32_t disc_map_no_bitmap(void) { return DISC_MAP_NO_BITMAP; }
static ABI int32_t videodec_unsupported(void) {
    static int said;
    if (!said++) printf("Runtime: sceVideodec asked for: no video decoder yet (videos are skipped)\n");
    return VIDEODEC_UNSUPPORTED;
}
static ABI int32_t error_dialog_open(const void *param) {
    (void)param;
    printf("Runtime: ErrorDialog opened; finished at once (no dialog UI)\n");
    return 0;
}

/* libc's heap tracing (sceLibcHeapGetTraceInfo, during libc's start): where it notes the mspaces
 * it makes. The storage is ours; nothing here reads it. Layout: size, flag, getSegmentInfo, the
 * atomic id mask's address, the mstate table's (64 entries). */
typedef struct {
    uint64_t size;
    uint32_t flag, get_segment_info;
    uint64_t *atomic_id_mask, *mstate_table;
} HeapTraceInfo;
static uint64_t heap_trace_mask, heap_trace_table[64];
static ABI void heap_trace_info(HeapTraceInfo *info) {
    if (!info) return;
    info->get_segment_info = 0;
    info->atomic_id_mask = &heap_trace_mask;
    info->mstate_table = heap_trace_table;
}

static const RuntimeExport exports[] = {
    {"sceKernelCreateEventFlag", evf_create}, {"sceKernelDeleteEventFlag", evf_delete},
    {"sceKernelSetEventFlag", evf_set}, {"sceKernelClearEventFlag", evf_clear},
    {"sceKernelWaitEventFlag", evf_wait}, {"sceKernelPollEventFlag", evf_poll},
    {"sceKernelCancelEventFlag", evf_cancel},
    {"sceKernelConvertUtcToLocaltime", utc_to_local}, {"sceKernelConvertLocaltimeToUtc", local_to_utc},
    {"sceKernelUuidCreate", uuid_create}, {"sceKernelMlock", ok}, {"msync", ok},
    {"scePthreadCondattrInit", condattr_init}, {"scePthreadCondattrDestroy", condattr_destroy},
    {"sceCoredumpRegisterCoredumpHandler", ok}, {"sceCoredumpWriteUserData", ok},
    {"sceGameCustomDataDialogInitialize", ok}, {"sceGameCustomDataDialogOpen", ok},
    {"sceGameCustomDataDialogUpdateStatus", dialog_finished},
    {"sceGameCustomDataDialogGetResult", dialog_cancelled}, {"sceGameCustomDataDialogTerminate", ok},
    {"sceInvitationDialogInitialize", ok}, {"sceInvitationDialogOpen", ok},
    {"sceInvitationDialogUpdateStatus", dialog_finished},
    {"sceInvitationDialogGetResult", dialog_cancelled}, {"sceInvitationDialogTerminate", ok},
    {"sceNpSnsFacebookDialogInitialize", ok}, {"sceNpSnsFacebookDialogOpen", ok},
    {"sceNpSnsFacebookDialogUpdateStatus", dialog_finished},
    {"sceNpSnsFacebookDialogGetResult", dialog_cancelled}, {"sceNpSnsFacebookDialogTerminate", ok},
    {"sceWebBrowserDialogInitialize", ok}, {"sceWebBrowserDialogOpen", ok},
    {"sceWebBrowserDialogUpdateStatus", dialog_finished}, {"sceWebBrowserDialogTerminate", ok},
    {"sceNpCommerceDialogGetResult", dialog_cancelled},
    {"sceSaveDataDialogClose", ok}, {"sceSaveDataDialogGetResult", save_dialog_result},
    {"sceSaveDataDialogProgressBarSetValue", ok},
    {"sceSaveDataGetMountInfo", save_mount_info}, {"sceSaveDataGetParam", save_get_param},
    {"sceAppContentTemporaryDataMount2", temporary_mount},
    {"sceAppContentGetAddcontInfo", appcontent_not_found}, {"sceAppContentAddcontMount", appcontent_not_found},
    {"sceAppContentAddcontUnmount", appcontent_not_found}, {"sceAppContentAddcontDelete", appcontent_not_found},
    {"sceAppContentAddcontEnqueueDownload", appcontent_not_found},
    {"sceAppContentGetEntitlementKey", appcontent_not_found},
    {"scePlayGoGetProgress", playgo_progress}, {"scePlayGoPrefetch", ok}, {"scePlayGoClose", ok},
    {"scePlayGoTerminate", ok},
    {"scePlayGoGetEta", playgo_eta}, {"scePlayGoGetToDoList", playgo_todo},
    {"scePlayGoSetToDoList", ok}, {"scePlayGoSetLanguageMask", ok},
    {"sceKernelIsNeoMode", not_neo}, {"sceKernelGetPrtAperture", prt_aperture},
    {"sceSystemServiceParamGetString", system_param_string},
    {"sceHttpSetChunkedTransferEnabled", ok}, {"sceHttpSetRecvTimeOut", ok},
    {"sceHttpSetSendTimeOut", ok}, {"sceHttpsLoadCert", ok},
    {"sceHttpGetAllResponseHeaders", http_unavailable},
    {"sceErrorDialogInitialize", ok}, {"sceErrorDialogOpen", error_dialog_open},
    {"sceErrorDialogUpdateStatus", dialog_finished}, {"sceErrorDialogTerminate", ok},
    {"sceNpCheckNpAvailabilityA", np_signed_out}, {"sceNpGetAccountCountryA", np_signed_out},
    {"sceNpGetAccountIdA", np_signed_out}, {"sceNpGetGamePresenceStatusA", np_signed_out},
    {"sceNpRegisterGamePresenceCallbackA", callback_id},
    {"sceNpRegisterStateCallbackA", callback_id},
    {"sceDiscMap_7C980FFB0AA27E7A", disc_map_no_bitmap},
    {"sceVideodecQueryResourceInfo", videodec_unsupported},
    {"sceVideodecCreateDecoder", videodec_unsupported}, {"sceVideodecDecode", videodec_unsupported},
    {"sceVideodecFlush", videodec_unsupported}, {"sceVideodecReset", videodec_unsupported},
    {"sceVideodecDeleteDecoder", ok}, {"__pthread_cxa_finalize", ok},
    {"sceLibcHeapGetTraceInfo", heap_trace_info},
    {"scePadSetLightBar", ok}, {"scePadResetLightBar", ok},
    {"sceRemoteplayInitialize", ok}, {"sceRemoteplayProhibit", ok},
    {"sceScreenShotEnable", ok}, {"sceScreenShotDisable", ok}, {"sceVideoRecordingSetInfo", ok},
    {"sceGameLiveStreamingPermitServerSideRecording", ok},
    {"sceSystemServiceGetDisplaySafeAreaInfo", safe_area}, {"sceUserServiceGetUserColor", user_color},
    {"sceRtcParseDateTime", rtc_invalid}, {"sceRtcParseRFC3339", rtc_invalid},
    {"sceSslGetSerialNumber", ssl_not_found},
    {"sceNetCtlInit", ok}, {"sceNetCtlTerm", ok}, {"sceNetCtlGetResult", netctl_not_connected},
    {"sceNetGetpeername", net_not_connected},
    {"sceHttpCreateRequestWithURL2", http_unavailable}, {"sceHttpsSetSslCallback", ok},
    {"sceHttpUriEscape", http_unavailable},
    {"sceNpTrophyDestroyContext", ok}, {"sceNpTrophyDestroyHandle", ok},
    {"sceNpTrophyGetGameIcon", trophy_invalid}, {"sceNpTrophyGetGroupIcon", trophy_invalid},
    {"sceNpTrophyGetGroupInfo", trophy_invalid}, {"sceNpTrophyGetTrophyIcon", trophy_invalid},
    {"sceNpTrophyGetTrophyUnlockState", trophy_invalid},
    /* PSN beyond Bloodborne's set: signed out; releases succeed. */
    {"sceNpAuthCreateRequest", np_signed_out}, {"sceNpCreateRequest", np_signed_out},
    {"sceNpGetAccountDateOfBirth", np_signed_out}, {"sceNpLookupCreateRequest", np_signed_out},
    {"sceNpBandwidthTestInitStart", np_signed_out}, {"sceNpBandwidthTestGetStatus", np_signed_out},
    {"sceNpBandwidthTestShutdown", ok},
    {"sceNpInGameMessageInitialize", np_signed_out}, {"sceNpInGameMessageTerminate", ok},
    {"sceNpInGameMessageCreateHandle", np_signed_out}, {"sceNpInGameMessageDeleteHandle", ok},
    {"sceNpInGameMessagePrepare", np_signed_out}, {"sceNpInGameMessageSendData", np_signed_out},
    {"sceNpMatching2GetRoomDataExternalList", np_signed_out},
    {"sceNpMatching2GetRoomDataInternal", np_signed_out},
    {"sceNpMatching2GetRoomMemberDataInternal", np_signed_out},
    {"sceNpMatching2GetUserInfoList", np_signed_out}, {"sceNpMatching2RegisterRoomMessageCallback", ok},
    {"sceNpMatching2SendRoomChatMessage", np_signed_out}, {"sceNpMatching2SendRoomMessage", np_signed_out},
    {"sceNpMatching2SetUserInfo", np_signed_out}, {"sceNpMatching2SignalingGetConnectionInfo", np_signed_out},
    {"sceNpScoreGetFriendsRanking", np_signed_out}, {"sceNpScoreGetFriendsRankingAsync", np_signed_out},
    {"sceNpScoreGetGameDataAsync", np_signed_out}, {"sceNpScoreGetRankingByNpId", np_signed_out},
    {"sceNpScoreGetRankingByNpIdAsync", np_signed_out}, {"sceNpScoreGetRankingByRangeAsync", np_signed_out},
    {"sceNpScorePollAsync", np_signed_out}, {"sceNpScoreRecordGameDataAsync", np_signed_out},
    {"sceNpScoreRecordScoreAsync", np_signed_out},
    {"sceNpSnsFacebookCreateRequest", np_signed_out}, {"sceNpSnsFacebookDeleteRequest", ok},
    {"sceNpSnsFacebookGetAccessToken", np_signed_out},
    {"sceNpTssGetData", np_signed_out},
    {"sceNpTusCreateNpTitleCtx", np_signed_out}, {"sceNpTusDeleteNpTitleCtx", ok},
    {"sceNpTusCreateRequest", np_signed_out}, {"sceNpTusDeleteRequest", ok},
    {"sceNpTusAddAndGetVariable", np_signed_out}, {"sceNpTusAddAndGetVariableVUser", np_signed_out},
    {"sceNpTusDeleteMultiSlotData", np_signed_out}, {"sceNpTusDeleteMultiSlotDataVUser", np_signed_out},
    {"sceNpTusGetData", np_signed_out}, {"sceNpTusGetDataVUser", np_signed_out},
    {"sceNpTusGetMultiSlotVariable", np_signed_out}, {"sceNpTusGetMultiSlotVariableVUser", np_signed_out},
    {"sceNpTusSetData", np_signed_out}, {"sceNpTusSetDataVUser", np_signed_out},
    {"sceNpTusSetMultiSlotVariable", np_signed_out}, {"sceNpTusSetMultiSlotVariableVUser", np_signed_out},
    {"sceNpWebApiCreateHandle", np_signed_out}, {"sceNpWebApiDeleteHandle", ok},
    {"sceNpWebApiCreateServicePushEventFilter", np_signed_out},
    {"sceNpWebApiDeleteServicePushEventFilter", ok},
    {"sceNpWebApiRegisterServicePushEventCallback", np_signed_out},
    {"sceNpWebApiUnregisterServicePushEventCallback", ok}, {"sceNpWebApiSendRequest2", np_signed_out},
    {"sceNpWordFilterCreateTitleCtx", np_signed_out}, {"sceNpWordFilterDeleteTitleCtx", ok},
    {"sceNpWordFilterCreateRequest", np_signed_out}, {"sceNpWordFilterCreateAsyncRequest", np_signed_out},
    {"sceNpWordFilterDeleteRequest", ok}, {"sceNpWordFilterAbortRequest", ok},
    {"sceNpWordFilterPollAsync", np_signed_out}, {"sceNpWordFilterCensorComment", np_signed_out},
    {"sceNpWordFilterSanitizeComment", np_signed_out},
};

uintptr_t runtime_compat_resolve(const char *name) { return RUNTIME_LOOKUP(exports, name); }

void runtime_compat_report(void) {
    printf("Runtime: event flags created=%zu, waits=%zu\n", flags_created, flag_waits);
}
