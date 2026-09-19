

#include "symbol_resolver.h"
#include <shadowhook.h>

#include <android/log.h>
#include <cxxabi.h>
#include <link.h>
#include <elf.h>
#include <fcntl.h>
#include <sys/mman.h>
#include <sys/stat.h>
#include <unistd.h>
#include <string.h>
#include <stdlib.h>
#include <stdint.h>
#include <stdbool.h>

#define TAG "amkush/sym_resolver"
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  TAG, __VA_ARGS__)
#define LOGW(...) __android_log_print(ANDROID_LOG_WARN,  TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, TAG, __VA_ARGS__)
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, TAG, __VA_ARGS__)

static const char CAMERASERVICE_LIB[] = "libcameraservice.so";

typedef struct {
    const char *mangled;
    int         variant;
} KnownSym;

static const KnownSym PCR_KNOWN[] = {

    { "_ZN7android7camera320processCaptureResultERNS0_19CaptureOutputStatesEPKNS0_21camera_capture_resultE",
      PCR_VARIANT_OUTPUTUTILS },

    { "_ZN7android17HidlCamera3Device24processCaptureResult_3_4ERKNS_8hardware8hidl_vecINS1_6camera6device4V3_413CaptureResultEEE",
      PCR_VARIANT_HIDL_3_4 },

    { "_ZN7android17HidlCamera3Device20processCaptureResultERKNS_8hardware8hidl_vecINS1_6camera6device4V3_213CaptureResultEEE",
      PCR_VARIANT_HIDL_3_2 },

    { "_ZN7android17AidlCamera3Device20processCaptureResultERKNSt3__16vectorIN4aidl7android8hardware6camera6device13CaptureResultENS1_9allocatorIS8_EEEE",
      PCR_VARIANT_AIDL },

    { "_ZN7android13Camera3Device20processCaptureResultEPK22camera3_capture_result",
      PCR_VARIANT_MEMBER },
    { nullptr, 0 }
};

static const KnownSym ROB_KNOWN[] = {

    { "_ZN7android7camera320returnOutputBuffersEbRKNS_2spINS0_20NotificationListenerEEEPK22camera3_stream_buffer_tmllbNS_15SessionStatsBuilderEbRKNSt3__14mapINS_2spINS_7SurfaceEEENSt3__16vectorIiNS9_9allocatorIiEEEENS9_4lessISA_EENS9_9allocatorINS9_4pairIKSA_SF_EEEEEERKNS0_20CaptureResultExtrasENS0_19ERROR_BUF_STRATEGYEi",
      PCR_VARIANT_ROB },

    { "_ZN7android7camera320returnOutputBuffersEbRKNS_2spINS0_20NotificationListenerEEEPK22camera3_stream_buffer_tmllbRNS_15SessionStatsBuilderEbRKNSt3__14mapINS_2spINS_7SurfaceEEENSt3__16vectorIiNS9_9allocatorIiEEEENS9_4lessISA_EENS9_9allocatorINS9_4pairIKSA_SF_EEEEEERKNS0_20CaptureResultExtrasENS0_19ERROR_BUF_STRATEGYEi",
      PCR_VARIANT_ROB },

    { "_ZN7android7camera320returnOutputBuffersEbRKNS_2spINS0_20NotificationListenerEEEPK22camera3_stream_buffer_tmllbNS_15SessionStatsBuilderEbRKNSt3__14mapINS_2spINS_7SurfaceEEENSt3__16vectorIiNS9_9allocatorIiEEEENS9_4lessISA_EENS9_9allocatorINS9_4pairIKSA_SF_EEEEEERKNS0_20CaptureResultExtrasENS0_19ERROR_BUF_STRATEGYE",
      PCR_VARIANT_ROB },

    { "_ZN7android7camera320returnOutputBuffersEbRKNS_2spINS0_20NotificationListenerEEEPK22camera3_stream_buffer_tmllbNS_15SessionStatsBuilderEbRKNSt3__14mapINS_2spINS_7SurfaceEEENSt3__16vectorIiNS9_9allocatorIiEEEENS9_4lessISA_EENS9_9allocatorINS9_4pairIKSA_SF_EEEEEERKNS0_20CaptureResultExtrasENS0_19ERROR_BUF_STRATEGYEix",
      PCR_VARIANT_ROB },

    /* Do not add returnAndRemovePendingOutputBuffers here.  That helper has
     * a different four-argument ABI (it takes an InFlightRequest&), so it
     * cannot use the returnOutputBuffers proxy even though its name is close.
     * It is deliberately left out rather than being treated as a fallback. */
    { nullptr, 0 }
};

static const KnownSym CS_KNOWN[] = {

    { "_ZN7android17HidlCamera3Device16HidlHalInterface16configureStreamsEPK15camera_metadataPNS_7camera327camera_stream_configurationERKNSt3__16vectorIjNS8_9allocatorIjEEEEl",
      CS_VARIANT_HIDL_HAL },

    { "_ZN7android17AidlCamera3Device16AidlHalInterface16configureStreamsEPK15camera_metadataPNS_7camera327camera_stream_configurationERKNSt3__16vectorIjNS8_9allocatorIjEEEEl",
      CS_VARIANT_AIDL_HAL },

    { "_ZN7android13Camera3Device16configureStreamsEPK28camera3_stream_configuration",
      CS_VARIANT_MEMBER_CFG },
    { nullptr, 0 }
};

static bool ptr_in_load_segments(struct dl_phdr_info *info, const void *ptr) {
    if (!info || !ptr) return false;
    uintptr_t addr = (uintptr_t)ptr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_LOAD) {
            uintptr_t seg_start = info->dlpi_addr + info->dlpi_phdr[i].p_vaddr;
            uintptr_t seg_end   = seg_start + info->dlpi_phdr[i].p_filesz;
            if (addr >= seg_start && addr < seg_end) return true;
        }
    }
    return false;
}

static bool validate_dt_ptr(struct dl_phdr_info *info, const void *ptr, size_t ) {
    return ptr_in_load_segments(info, ptr);
}

static size_t gnu_hash_get_nchain(const uint32_t *ght, struct dl_phdr_info *info) {
    if (!ght || !info) return 0;
    uint32_t nbuckets = ght[0], symoffset = ght[1], bloom_size = ght[2];
    const uint32_t *buckets = ght + 4 + bloom_size * (sizeof(size_t) / 4);
    size_t buckets_bytes = (size_t)nbuckets * sizeof(uint32_t);
    if (!validate_dt_ptr(info, buckets, buckets_bytes)) {
        LOGE("GNU hash bucket array out of bounds");
        return 0;
    }
    uint32_t max_idx = 0;
    for (uint32_t i = 0; i < nbuckets; i++) {
        if (buckets[i] > max_idx) max_idx = buckets[i];
    }
    if (max_idx == 0) return symoffset;
    const uint32_t *chains = buckets + nbuckets;
    uint32_t idx = max_idx;
    while (true) {
        uint32_t chain_idx = idx - symoffset;
        const uint32_t *chain_entry = chains + chain_idx;
        if (!validate_dt_ptr(info, chain_entry, sizeof(uint32_t))) {
            LOGE("GNU hash chain entry out of bounds at idx=%u", idx);
            return 0;
        }
        uint32_t hash = *chain_entry;
        idx++;
        if (hash & 1) break;
    }
    return idx;
}

typedef enum { SCAN_PCR = 0, SCAN_CS = 1, SCAN_ROB = 2 } ScanKind;

static bool is_thunk(const char *demangled) {
    if (!demangled) return false;

    if (strncmp(demangled, "virtual thunk to",     16) == 0) return true;

    if (strncmp(demangled, "non-virtual thunk to", 20) == 0) return true;
    return false;
}

static int score_symbol(const char *demangled, ScanKind kind, size_t size,
                         const char *target_name) {
    if (!demangled) return -9999;
    if (strstr(demangled, "OfflineSession"))  return -9999;
    if (strstr(demangled, "Callbacks"))       return -9999;
    if (strstr(demangled, "CompositeStream")) return -9999;

    /* Reject all Binder proxies, stubs, and HIDL/AIDL dispatcher interfaces.
     * Hooking these with member function pointers corrupts IPC registers/stack
     * and causes fatal SIGSEGV in writeToParcel / onTransact. */
    if (strstr(demangled, "BnHw") ||
        strstr(demangled, "BpHw") ||
        strstr(demangled, "BnCamera") ||
        strstr(demangled, "BpCamera") ||
        strstr(demangled, "BnInterface") ||
        strstr(demangled, "BpInterface") ||
        strstr(demangled, "_hidl_") ||
        strstr(demangled, "IInterface") ||
        strstr(demangled, "HidlInstrumentor") ||
        strstr(demangled, "CameraDeviceCallback") ||
        strstr(demangled, "ICameraDeviceCallback") ||
        strstr(demangled, "ICameraDeviceSession") ||
        strstr(demangled, "HidlCamera3DeviceCallback") ||
        strstr(demangled, "AidlCamera3DeviceCallback")) {
        return -9999;
    }

    if (is_thunk(demangled)) return -9999;

    if (!strstr(demangled, target_name))      return -9999;


    if (kind == SCAN_ROB) {
        /* returnOutputBuffers() has two unrelated ABI families on vendor
         * builds.  The framework helper in android::camera3 is a free
         * function with the 14-argument ABI used by my_rob_proxy.  OPlus/
         * MediaTek also expose extension methods with the same basename:
         *
         *   CameraServiceExtImpl*::returnOutputBuffers(CaptureOutputStates&,
         *       camera_stream_buffer*, size_t, InFlightRequest&, nsecs_t, bool)
         *
         * Those are member functions with a six-argument ABI.  Treating the
         * latter as the helper corrupts x0 (the C++ this pointer) when
         * SHADOWHOOK_CALL_PREV forwards the proxy's first bool, and the next
         * call into the extension commonly faults at this + 0x88 from
         * notifyShutter.  Only accept the framework helper here; the extension
         * is called normally by that helper and must not be inline-hooked with
         * this proxy. */
        if (strncmp(demangled, "android::camera3::returnOutputBuffers(",
                    sizeof("android::camera3::returnOutputBuffers(") - 1) != 0) {
            return -9999;
        }
        int score = 50;
        if (size > 200) score += 50;
        else if (size > 16) score += 20;
        return score;
    }

    int score = 0;

    if (strstr(demangled, "HidlCamera3Device::") ||
        strstr(demangled, "AidlCamera3Device::") ||
        strstr(demangled, "Camera3Device::")) {
        score += 100;
    }
    if (strstr(demangled, "HidlHalInterface::") ||
        strstr(demangled, "AidlHalInterface::")) {
        score += 80;
    }

    if (score == 0) score = 10;

    if (size > 1000) score += 50;
    else if (size > 16) score += 20;
    else if (size > 0)  score += 5;

    if (kind == SCAN_PCR) {
        if (strstr(demangled, "processCaptureResult_3_4(")) score += 10;
        else if (strstr(demangled, "processCaptureResult("))  score += 5;
    }

    return score;
}

static int classify_variant(const char *demangled, ScanKind kind) {
    if (!demangled) return 0;
    if (strstr(demangled, "BnHw") || strstr(demangled, "BpHw") ||
        strstr(demangled, "BnCamera") || strstr(demangled, "BpCamera") ||
        strstr(demangled, "_hidl_") || strstr(demangled, "CameraDeviceCallback") ||
        strstr(demangled, "ICameraDeviceCallback")) {
        return PCR_VARIANT_UNKNOWN;
    }
    if (kind == SCAN_ROB) {
        /* Keep this in lockstep with score_symbol(): only the free framework
         * helper has the 14-argument ABI implemented by hook_proxy.cpp.
         * Vendor extension methods are intentionally not ROB candidates. */
        if (strncmp(demangled, "android::camera3::returnOutputBuffers(",
                    sizeof("android::camera3::returnOutputBuffers(") - 1) == 0) {
            return PCR_VARIANT_ROB;
        }
        return PCR_VARIANT_UNKNOWN;
    }
    if (kind == SCAN_PCR) {
        if (strstr(demangled, "returnOutputBuffers("))
            return PCR_VARIANT_ROB;
        if (strstr(demangled, "camera3::processCaptureResult("))
            return PCR_VARIANT_OUTPUTUTILS;
        if (strstr(demangled, "HidlCamera3Device::processCaptureResult_3_4("))
            return PCR_VARIANT_HIDL_3_4;
        if (strstr(demangled, "HidlCamera3Device::processCaptureResult(") &&
            !strstr(demangled, "processCaptureResult_3_4"))
            return PCR_VARIANT_HIDL_3_2;
        if (strstr(demangled, "AidlCamera3Device::processCaptureResult("))
            return PCR_VARIANT_AIDL;
        if (strstr(demangled, "Camera3Device::processCaptureResult(") &&
            !strstr(demangled, "Hidl") && !strstr(demangled, "Aidl"))
            return PCR_VARIANT_MEMBER;

        return PCR_VARIANT_UNKNOWN;
    }
    if (kind == SCAN_CS) {
        if (strstr(demangled, "HidlHalInterface::configureStreams("))
            return CS_VARIANT_HIDL_HAL;
        if (strstr(demangled, "AidlHalInterface::configureStreams("))
            return CS_VARIANT_AIDL_HAL;
        if (strstr(demangled, "Camera3Device::configureStreams(") &&
            !strstr(demangled, "Locked") && !strstr(demangled, "HalInterface"))
            return CS_VARIANT_MEMBER_CFG;
        return CS_VARIANT_UNKNOWN;
    }
    return 0;
}

#define SCAN_ALL_MAX_ADDRS 64

struct ScanCtxAll {
    ScanKind       kind;
    ResolvedSymbol *out;
    int            max_count;
    int            count;

    void           *seen_addrs[SCAN_ALL_MAX_ADDRS];
    int             seen_count;
};

static bool addr_is_seen(ScanCtxAll *ctx, void *addr) {
    for (int i = 0; i < ctx->seen_count; i++) {
        if (ctx->seen_addrs[i] == addr) return true;
    }
    return false;
}

static void addr_mark_seen(ScanCtxAll *ctx, void *addr) {
    if (ctx->seen_count < SCAN_ALL_MAX_ADDRS) {
        ctx->seen_addrs[ctx->seen_count++] = addr;
    }
}

static int phdr_callback_all(struct dl_phdr_info *info, size_t , void *data) {

    /* Android 16 (API 36): cameraserver may no longer map libcameraservice.so
     * by that name, so the old target-scoped scan found nothing. Scan EVERY
     * loaded library (and the main exe) — score_symbol() still matches by the
     * demangled name (processCaptureResult / returnOutputBuffers / ...), so
     * false positives from unrelated libs are effectively impossible and the
     * address-dedup keeps the list clean. Only log libs that actually match
     * anything to avoid log spam. */
    bool is_target = true;

    LOGD("ELF scan: checking '%s' base=%p",
         (info->dlpi_name && info->dlpi_name[0]) ? info->dlpi_name : "(main exe)",
         (void *)info->dlpi_addr);


    const ElfW(Dyn) *dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn || !ptr_in_load_segments(info, dyn)) {
        LOGE("ELF scan: PT_DYNAMIC not found/OOB in '%s'",
             info->dlpi_name ? info->dlpi_name : "(unknown)");
        return 0;
    }


    const ElfW(Sym) *symtab = nullptr;
    const char *strtab = nullptr;
    size_t syment = sizeof(ElfW(Sym));
    size_t nchain = 0;

    for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_SYMTAB: {
                const void *p = (const void *)(info->dlpi_addr + d->d_un.d_ptr);
                if (validate_dt_ptr(info, p, 1)) symtab = (const ElfW(Sym) *)p;
                break;
            }
            case DT_STRTAB: {
                const void *p = (const void *)(info->dlpi_addr + d->d_un.d_ptr);
                if (validate_dt_ptr(info, p, 1)) strtab = (const char *)p;
                break;
            }
            case DT_SYMENT:
                syment = (size_t)d->d_un.d_val;
                break;
            case DT_HASH: {
                const uint32_t *ht = (const uint32_t *)(info->dlpi_addr + d->d_un.d_ptr);
                if (validate_dt_ptr(info, ht, 2 * sizeof(uint32_t))) nchain = ht[1];
                break;
            }
            case DT_GNU_HASH: {
                const uint32_t *ght = (const uint32_t *)(info->dlpi_addr + d->d_un.d_ptr);
                if (validate_dt_ptr(info, ght, 4 * sizeof(uint32_t))) {
                    size_t gnu_n = gnu_hash_get_nchain(ght, info);
                    if (gnu_n > nchain) nchain = gnu_n;
                }
                break;
            }
        }
    }

    if (!symtab || !strtab || nchain == 0) {
        LOGW("ELF scan: incomplete ELF info for '%s' (symtab=%p strtab=%p nchain=%zu)",
             info->dlpi_name ? info->dlpi_name : "(unknown)",
             (void *)symtab, (void *)strtab, nchain);
        return 0;
    }

    ScanCtxAll *ctx = (ScanCtxAll *)data;


    const char *raw_filter;
    const char *target_name;
    if (ctx->kind == SCAN_ROB) {
        raw_filter  = "returnOutputBuffers";
        target_name = "returnOutputBuffers";
    } else if (ctx->kind == SCAN_PCR) {
        raw_filter  = "processCaptureResult";
        target_name = "processCaptureResult";
    } else {
        raw_filter  = "configureStreams";
        target_name = "configureStreams";
    }

    int found_this_elf = 0;

    for (size_t i = 0; i < nchain && ctx->count < ctx->max_count; ++i) {
        const ElfW(Sym) *sym = (const ElfW(Sym) *)((const char *)symtab + i * syment);
        unsigned char type = ELF_ST_TYPE(sym->st_info);
        if (type != STT_FUNC && type != STT_GNU_IFUNC) continue;
        if (sym->st_value == 0 || sym->st_name == 0) continue;
        if (!ptr_in_load_segments(info, strtab + sym->st_name)) continue;

        const char *raw_name = strtab + sym->st_name;


        if (!strstr(raw_name, raw_filter)) continue;

        int status = 0;
        char *demangled = abi::__cxa_demangle(raw_name, nullptr, nullptr, &status);
        if (status != 0 || !demangled) {
            if (demangled) free(demangled);
            continue;
        }

        int score = score_symbol(demangled, ctx->kind, sym->st_size, target_name);
        if (score < 0) {
            free(demangled);
            continue;
        }

        void *addr = (void *)((uintptr_t)info->dlpi_addr + sym->st_value);

        if (addr_is_seen(ctx, addr)) {
            LOGI("ELF scan: dedup skip: %s (addr=%p)", demangled, addr);
            free(demangled);
            continue;
        }

        int variant = classify_variant(demangled, ctx->kind);
        if (variant == 0) {
            free(demangled);
            continue;
        }
        LOGI("ELF scan: FOUND[%d] score=%d variant=%d addr=%p size=%llu: %s",
             ctx->count, score, variant, addr, (unsigned long long)sym->st_size, demangled);

        ctx->out[ctx->count].ptr     = addr;
        ctx->out[ctx->count].symbol  = raw_name;
        ctx->out[ctx->count].source  = "elf_scan";
        ctx->out[ctx->count].variant = variant;
        ctx->out[ctx->count].size    = (size_t)sym->st_size;
        addr_mark_seen(ctx, addr);
        ctx->count++;
        found_this_elf++;

        free(demangled);
    }

    LOGI("ELF scan: '%s' → %d match(es)",
         info->dlpi_name ? info->dlpi_name : "(main exe)", found_this_elf);
    return 0;
}

struct ScanCtxBest {
    ScanKind     kind;
    void        *found_addr;
    const char  *found_name;
    int          found_variant;
    int          best_score;
};

static int phdr_callback_best(struct dl_phdr_info *info, size_t , void *data) {
    bool is_target = false;
    if (info->dlpi_name) {
        if (strstr(info->dlpi_name, "libcameraservice.so")) is_target = true;
        if (strstr(info->dlpi_name, "cameraserver"))        is_target = true;
    }
    if (!info->dlpi_name || info->dlpi_name[0] == '\0')    is_target = true;
    if (!is_target) return 0;

    const ElfW(Dyn) *dyn = nullptr;
    for (int i = 0; i < info->dlpi_phnum; i++) {
        if (info->dlpi_phdr[i].p_type == PT_DYNAMIC) {
            dyn = (const ElfW(Dyn) *)(info->dlpi_addr + info->dlpi_phdr[i].p_vaddr);
            break;
        }
    }
    if (!dyn || !ptr_in_load_segments(info, dyn)) return 0;

    const ElfW(Sym) *symtab = nullptr;
    const char *strtab = nullptr;
    size_t syment = sizeof(ElfW(Sym));
    size_t nchain = 0;

    for (const ElfW(Dyn) *d = dyn; d->d_tag != DT_NULL; ++d) {
        switch (d->d_tag) {
            case DT_SYMTAB: {
                const void *p = (const void *)(info->dlpi_addr + d->d_un.d_ptr);
                if (validate_dt_ptr(info, p, 1)) symtab = (const ElfW(Sym) *)p;
                break;
            }
            case DT_STRTAB: {
                const void *p = (const void *)(info->dlpi_addr + d->d_un.d_ptr);
                if (validate_dt_ptr(info, p, 1)) strtab = (const char *)p;
                break;
            }
            case DT_SYMENT: syment = (size_t)d->d_un.d_val; break;
            case DT_HASH: {
                const uint32_t *ht = (const uint32_t *)(info->dlpi_addr + d->d_un.d_ptr);
                if (validate_dt_ptr(info, ht, 2 * sizeof(uint32_t))) nchain = ht[1];
                break;
            }
            case DT_GNU_HASH: {
                const uint32_t *ght = (const uint32_t *)(info->dlpi_addr + d->d_un.d_ptr);
                if (validate_dt_ptr(info, ght, 4 * sizeof(uint32_t))) {
                    size_t gnu_n = gnu_hash_get_nchain(ght, info);
                    if (gnu_n > nchain) nchain = gnu_n;
                }
                break;
            }
        }
    }
    if (!symtab || !strtab || nchain == 0) return 0;

    ScanCtxBest *ctx = (ScanCtxBest *)data;
    const char *target_name = (ctx->kind == SCAN_PCR) ? "processCaptureResult"
                            : (ctx->kind == SCAN_ROB) ? "returnOutputBuffers"
                                                       : "configureStreams";

    for (size_t i = 0; i < nchain; ++i) {
        const ElfW(Sym) *sym = (const ElfW(Sym) *)((const char *)symtab + i * syment);
        unsigned char type = ELF_ST_TYPE(sym->st_info);
        if (type != STT_FUNC && type != STT_GNU_IFUNC) continue;
        if (sym->st_value == 0 || sym->st_name == 0) continue;
        if (!ptr_in_load_segments(info, strtab + sym->st_name)) continue;

        const char *raw_name = strtab + sym->st_name;
        if (!strstr(raw_name, target_name)) continue;

        int status = 0;
        char *demangled = abi::__cxa_demangle(raw_name, nullptr, nullptr, &status);

        int score = (status == 0 && demangled)
                    ? score_symbol(demangled, ctx->kind, sym->st_size, target_name)
                    : -9999;

        if (score > ctx->best_score) {
            ctx->best_score   = score;
            ctx->found_addr   = (void *)((uintptr_t)info->dlpi_addr + sym->st_value);
            ctx->found_name   = raw_name;
            ctx->found_variant = (status == 0 && demangled)
                                 ? classify_variant(demangled, ctx->kind) : 0;
            if (demangled) LOGI("New best score=%d: %s", score, demangled);
        }

        if (demangled) free(demangled);
    }
    return 0;
}

static ResolvedSymbol resolve(const KnownSym *known_syms, ScanKind kind, const char *what) {
    ResolvedSymbol r = {};


    void *handle = shadowhook_dlopen(CAMERASERVICE_LIB);
    if (handle) {
        for (int i = 0; known_syms[i].mangled; i++) {
            void *addr = shadowhook_dlsym(handle, known_syms[i].mangled);
            if (!addr) addr = shadowhook_dlsym_symtab(handle, known_syms[i].mangled);
            if (addr) {
                LOGI("%s: known-list matched variant=%d at %p",
                     what, known_syms[i].variant, addr);
                r.ptr     = addr;
                r.symbol  = known_syms[i].mangled;
                r.source  = "known_list";
                r.variant = known_syms[i].variant;
                shadowhook_dlclose(handle);
                return r;
            }
        }
        shadowhook_dlclose(handle);
    }


    LOGW("%s: known-list missed — running ELF scan", what);
    ScanCtxBest ctx = { kind, nullptr, nullptr, 0, -9999 };
    dl_iterate_phdr(phdr_callback_best, &ctx);
    if (ctx.found_addr) {
        r.ptr     = ctx.found_addr;
        r.symbol  = ctx.found_name ? ctx.found_name : "(scan)";
        r.source  = "elf_scan";
        r.variant = ctx.found_variant;
        LOGI("%s: ELF scan found variant=%d at %p", what, r.variant, r.ptr);
    } else {
        LOGE("%s: ALL resolution strategies failed", what);
    }
    return r;
}

ResolvedSymbol resolve_process_capture_result(void) {
    return resolve(PCR_KNOWN, SCAN_PCR, "processCaptureResult");
}

ResolvedSymbol resolve_configure_streams(void) {
    return resolve(CS_KNOWN, SCAN_CS, "configureStreams");
}

int resolve_all_pcr(ResolvedSymbol *out, int max_count) {
    if (!out || max_count <= 0) return 0;

    LOGI("resolve_all_pcr: starting full ELF scan (dynamic, no hardcoded names)");


    ScanCtxAll ctx = {};
    ctx.kind      = SCAN_PCR;
    ctx.out       = out;
    ctx.max_count = max_count;
    ctx.count     = 0;
    ctx.seen_count = 0;
    dl_iterate_phdr(phdr_callback_all, &ctx);
    int count = ctx.count;
    LOGI("resolve_all_pcr: processCaptureResult scan found %d variant(s)", count);







    if (count < max_count) {
        ScanCtxAll rob_ctx = {};
        rob_ctx.kind      = SCAN_ROB;
        rob_ctx.out       = out + count;
        rob_ctx.max_count = max_count - count;
        rob_ctx.count     = 0;
        rob_ctx.seen_count = 0;

        rob_ctx.seen_count = (ctx.seen_count < SCAN_ALL_MAX_ADDRS) ? ctx.seen_count : SCAN_ALL_MAX_ADDRS;
        for (int i = 0; i < rob_ctx.seen_count; i++) {
            rob_ctx.seen_addrs[i] = ctx.seen_addrs[i];
        }
        dl_iterate_phdr(phdr_callback_all, &rob_ctx);
        if (rob_ctx.count > 0) {
            LOGI("resolve_all_pcr: returnOutputBuffers scan found %d variant(s)", rob_ctx.count);


            for (int ri = 0; ri < rob_ctx.count; ri++) {
                LOGI("  ROB candidate[%d]: addr=%p size=%zu sym=%s src=%s",
                     ri, rob_ctx.out[ri].ptr, rob_ctx.out[ri].size,
                     rob_ctx.out[ri].symbol ? rob_ctx.out[ri].symbol : "<null>",
                     rob_ctx.out[ri].source ? rob_ctx.out[ri].source : "<null>");
            }
            count += rob_ctx.count;

            for (int i = 0; i < rob_ctx.seen_count && ctx.seen_count < SCAN_ALL_MAX_ADDRS; i++) {
                bool already = false;
                for (int j = 0; j < ctx.seen_count; j++) {
                    if (ctx.seen_addrs[j] == rob_ctx.seen_addrs[i]) { already = true; break; }
                }
                if (!already) ctx.seen_addrs[ctx.seen_count++] = rob_ctx.seen_addrs[i];
            }
        } else {
            LOGW("resolve_all_pcr: returnOutputBuffers ELF scan found 0 — "
                 "will try known-list supplement next");
        }
    }


    void *handle = shadowhook_dlopen(CAMERASERVICE_LIB);
    if (handle) {
        int supplement_found = 0;
        for (int i = 0; PCR_KNOWN[i].mangled && count < max_count; i++) {
            void *addr = shadowhook_dlsym(handle, PCR_KNOWN[i].mangled);
            if (!addr) addr = shadowhook_dlsym_symtab(handle, PCR_KNOWN[i].mangled);
            if (!addr) continue;

            if (addr_is_seen(&ctx, addr)) {
                LOGI("resolve_all_pcr: known[%d] variant=%d addr=%p already in list",
                     i, PCR_KNOWN[i].variant, addr);
                continue;
            }

            out[count].ptr     = addr;
            out[count].symbol  = PCR_KNOWN[i].mangled;
            out[count].source  = "known_list_supplement";
            out[count].variant = PCR_KNOWN[i].variant;
            out[count].size    = 0;
            addr_mark_seen(&ctx, addr);
            LOGI("resolve_all_pcr: pcr_supplement[%d]: variant=%d at %p",
                 count, PCR_KNOWN[i].variant, addr);
            count++;
            supplement_found++;
        }


        for (int i = 0; ROB_KNOWN[i].mangled && count < max_count; i++) {
            void *addr = shadowhook_dlsym(handle, ROB_KNOWN[i].mangled);
            if (!addr) addr = shadowhook_dlsym_symtab(handle, ROB_KNOWN[i].mangled);
            if (!addr) continue;

            if (addr_is_seen(&ctx, addr)) {
                LOGI("resolve_all_pcr: rob_known[%d] addr=%p already in list", i, addr);
                continue;
            }

            out[count].ptr     = addr;
            out[count].symbol  = ROB_KNOWN[i].mangled;
            out[count].source  = "rob_known_list";
            out[count].variant = PCR_VARIANT_ROB;
            out[count].size    = 0;
            addr_mark_seen(&ctx, addr);
            LOGI("resolve_all_pcr: rob_supplement[%d]: variant=%d at %p",
                 count, PCR_VARIANT_ROB, addr);
            count++;
            supplement_found++;
        }

        shadowhook_dlclose(handle);
        if (supplement_found > 0) {
            LOGI("resolve_all_pcr: known-list added %d additional variant(s)", supplement_found);
        }
    }

    if (count == 0) {
        LOGE("resolve_all_pcr: CRITICAL — no PCR/ROB variants found on this device/ROM.");
    } else {
        LOGI("resolve_all_pcr: TOTAL %d variant(s) to hook:", count);
        for (int i = 0; i < count; i++) {
            LOGI("  [%d] variant=%d source=%s addr=%p",
                 i, out[i].variant, out[i].source, out[i].ptr);
        }
    }

    return count;
}

int resolve_all_cs(ResolvedSymbol *out, int max_count) {
    if (!out || max_count <= 0) return 0;

    LOGI("resolve_all_cs: starting full ELF scan for configureStreams");

    ScanCtxAll ctx = {};
    ctx.kind      = SCAN_CS;
    ctx.out       = out;
    ctx.max_count = max_count;
    ctx.count     = 0;
    ctx.seen_count = 0;
    dl_iterate_phdr(phdr_callback_all, &ctx);
    int count = ctx.count;
    LOGI("resolve_all_cs: ELF scan found %d variant(s)", count);


    void *handle = shadowhook_dlopen(CAMERASERVICE_LIB);
    if (handle) {
        for (int i = 0; CS_KNOWN[i].mangled && count < max_count; i++) {
            void *addr = shadowhook_dlsym(handle, CS_KNOWN[i].mangled);
            if (!addr) addr = shadowhook_dlsym_symtab(handle, CS_KNOWN[i].mangled);
            if (!addr) continue;
            if (addr_is_seen(&ctx, addr)) continue;

            out[count].ptr     = addr;
            out[count].symbol  = CS_KNOWN[i].mangled;
            out[count].source  = "known_list_supplement";
            out[count].variant = CS_KNOWN[i].variant;
            out[count].size    = 0;
            addr_mark_seen(&ctx, addr);
            LOGI("resolve_all_cs: supplement[%d]: variant=%d at %p",
                 count, CS_KNOWN[i].variant, addr);
            count++;
        }
        shadowhook_dlclose(handle);
    }

    LOGI("resolve_all_cs: total %d configureStreams variant(s)", count);
    return count;
}

void resolve_camera3_usage_hooks(ResolvedSymbol *setusage, ResolvedSymbol *getendpoint) {
    if (!setusage || !getendpoint) return;
    memset(setusage,    0, sizeof(*setusage));
    memset(getendpoint, 0, sizeof(*getendpoint));


    void *handle = shadowhook_dlopen(CAMERASERVICE_LIB);
    if (handle) {


        static const char *const SU_SYMS[] = {

            "_ZN7android12Camera3Stream8setUsageEy",

            "_ZN7android12Camera3Stream8setUsageEm",

            "_ZN7android7camera312Camera3Stream8setUsageEy",
            "_ZN7android7camera312Camera3Stream8setUsageEm",
            nullptr
        };
        for (int i = 0; SU_SYMS[i] && !setusage->ptr; i++) {
            void *addr = shadowhook_dlsym(handle, SU_SYMS[i]);
            if (!addr) addr = shadowhook_dlsym_symtab(handle, SU_SYMS[i]);
            if (addr) {
                setusage->ptr    = addr;
                setusage->symbol = SU_SYMS[i];
                setusage->source = "known_list";
                LOGI("resolve_camera3_usage_hooks: setUsage found (%s) @ %p",
                     SU_SYMS[i], addr);
            }
        }


        static const char *const GEP_SYMS[] = {

            "_ZNK7android20Camera3OutputStream16getEndpointUsageEPy",

            "_ZN7android20Camera3OutputStream16getEndpointUsageEPy",

            "_ZNK7android20Camera3OutputStream16getEndpointUsageEPm",
            "_ZN7android20Camera3OutputStream16getEndpointUsageEPm",

            "_ZNK7android7camera320Camera3OutputStream16getEndpointUsageEPy",
            "_ZN7android7camera320Camera3OutputStream16getEndpointUsageEPy",
            nullptr
        };
        for (int i = 0; GEP_SYMS[i] && !getendpoint->ptr; i++) {
            void *addr = shadowhook_dlsym(handle, GEP_SYMS[i]);
            if (!addr) addr = shadowhook_dlsym_symtab(handle, GEP_SYMS[i]);
            if (addr) {
                getendpoint->ptr    = addr;
                getendpoint->symbol = GEP_SYMS[i];
                getendpoint->source = "known_list";
                LOGI("resolve_camera3_usage_hooks: getEndpointUsage found (%s) @ %p",
                     GEP_SYMS[i], addr);
            }
        }
        shadowhook_dlclose(handle);
    }




    if (!setusage->ptr || !getendpoint->ptr) {
        LOGW("resolve_camera3_usage_hooks: dlsym missed at least one symbol — "
             "falling back to ELF substring scan");

        struct UsageCtx {
            ResolvedSymbol *su;
            ResolvedSymbol *gep;
        } uctx = { setusage, getendpoint };

        dl_iterate_phdr([](struct dl_phdr_info *info, size_t , void *data) -> int {
            bool is_target = false;
            const char *open_path = nullptr;
            if (info->dlpi_name && strstr(info->dlpi_name, "libcameraservice.so")) {
                is_target = true;
                open_path = info->dlpi_name;
            } else if (info->dlpi_name && strstr(info->dlpi_name, "cameraserver")) {
                is_target = true;
                open_path = info->dlpi_name;
            } else if (!info->dlpi_name || info->dlpi_name[0] == '\0') {
                is_target = true;
                open_path = "/proc/self/exe";
            }
            if (!is_target || !open_path) return 0;

            UsageCtx *uc = (UsageCtx *)data;

            int fd = open(open_path, O_RDONLY);
            if (fd < 0) return 0;
            struct stat st;
            if (fstat(fd, &st) < 0) { close(fd); return 0; }
            size_t file_sz = (size_t)st.st_size;
            void *mapped = mmap(nullptr, file_sz, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (mapped == MAP_FAILED) return 0;

            auto cleanup = [&]{ munmap(mapped, file_sz); };

            const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)mapped;
            if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
                ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
                cleanup(); return 0;
            }

            const Elf64_Shdr *shdrs = (const Elf64_Shdr *)((char *)mapped + ehdr->e_shoff);
            const char *shstrtab    = nullptr;
            if (ehdr->e_shstrndx < ehdr->e_shnum)
                shstrtab = (const char *)mapped + shdrs[ehdr->e_shstrndx].sh_offset;

            const Elf64_Sym  *symtab  = nullptr;
            uint64_t          nsyms   = 0;
            const char       *strtab  = nullptr;

            for (int si = 0; si < (int)ehdr->e_shnum; si++) {
                const Elf64_Shdr *sh = &shdrs[si];
                if ((sh->sh_type == SHT_SYMTAB || sh->sh_type == SHT_DYNSYM) &&
                    sh->sh_entsize == sizeof(Elf64_Sym)) {
                    symtab = (const Elf64_Sym *)((char *)mapped + sh->sh_offset);
                    nsyms  = sh->sh_size / sizeof(Elf64_Sym);
                    if (sh->sh_link < (uint32_t)ehdr->e_shnum)
                        strtab = (const char *)mapped + shdrs[sh->sh_link].sh_offset;
                    if (sh->sh_type == SHT_SYMTAB) break;
                }
            }

            if (!symtab || !strtab) { cleanup(); return 0; }

            for (uint64_t si = 0; si < nsyms; si++) {
                const Elf64_Sym *sym = &symtab[si];
                if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC) continue;
                if (sym->st_value == 0) continue;
                const char *name = strtab + sym->st_name;

                void *addr = (void *)(info->dlpi_addr + sym->st_value);

                if (!uc->su->ptr) {


                    if (strstr(name, "Camera3Stream") && strstr(name, "setUsage") &&
                        !strstr(name, "Camera3OutputStream")) {
                        uc->su->ptr    = addr;
                        uc->su->symbol = name;
                        uc->su->source = "elf_scan";
                        LOGI("resolve_camera3_usage_hooks: setUsage elf_scan hit: %s @ %p",
                             name, addr);
                    }
                }

                if (!uc->gep->ptr) {
                    if (strstr(name, "Camera3OutputStream") &&
                        strstr(name, "getEndpointUsage")) {
                        uc->gep->ptr    = addr;
                        uc->gep->symbol = name;
                        uc->gep->source = "elf_scan";
                        LOGI("resolve_camera3_usage_hooks: getEndpointUsage elf_scan hit: "
                             "%s @ %p", name, addr);
                    }
                }

                if (uc->su->ptr && uc->gep->ptr) break;
            }

            cleanup();
            return (uc->su->ptr && uc->gep->ptr) ? 1 : 0;
        }, &uctx);



        if (setusage->ptr    && strcmp(setusage->source,    "elf_scan") == 0)
            setusage->symbol    = "<elf_scan>";
        if (getendpoint->ptr && strcmp(getendpoint->source, "elf_scan") == 0)
            getendpoint->symbol = "<elf_scan>";
    }


    if (setusage->ptr && getendpoint->ptr) {
        LOGI("resolve_camera3_usage_hooks: BOTH found — "
             "setUsage=%p(%s) getEndpointUsage=%p(%s)",
             setusage->ptr,    setusage->source,
             getendpoint->ptr, getendpoint->source);
    } else {
        LOGW("resolve_camera3_usage_hooks: INCOMPLETE — "
             "setUsage=%s getEndpointUsage=%s",
             setusage->ptr    ? "found" : "MISSING",
             getendpoint->ptr ? "found" : "MISSING");
        LOGW("Without both hooks, PROTECTED buffers will survive gralloc allocation "
             "and returnAndRemovePendingOutputBuffers will SEGV_ACCERR as before.");
    }
}

int resolve_camera3_returnbuffer(ResolvedSymbol *out, int max_count) {
    if (!out || max_count <= 0) return 0;
    int count = 0;





    static const char *const RTRN_SYMS[] = {

        "_ZN7android7camera319Camera3OutputStream12returnBufferERKNS1_20camera_stream_bufferExxbRKNSt3__16vectorImNS4_9allocatorImEEEEyi",

        "_ZN7android7camera319Camera3OutputStream12returnBufferERKNS1_20camera_stream_bufferEllbRKNSt3__16vectorImNS4_9allocatorImEEEEmi",

        "_ZN7android7camera319Camera3OutputStream12returnBufferERKNS1_20camera_stream_bufferEllbRKNSt3__16vectorImNS4_9allocatorImEEEEmi",

        "_ZN7android19Camera3OutputStream12returnBufferERKNS_20camera_stream_bufferEllbRKNSt3__16vectorImNS1_9allocatorImEEEEmi",

        "_ZN7android7camera319Camera3OutputStream12returnBufferERKNS1_20camera_stream_bufferEllbRKNSt3__16vectorImNS4_9allocatorImEEEEmj",

        "_ZN7android7camera319Camera3OutputStream12returnBufferERKNS1_20camera_stream_bufferElbRKNSt3__16vectorImNS4_9allocatorImEEEEmj",



        "_ZN7android7camera319Camera3OutputStream12returnBufferERKNS1_20camera_stream_bufferElliRKNSt3__16vectorImNS4_9allocatorImEEEEmi",

        "_ZN7android7camera319Camera3OutputStream12returnBufferERKNS1_20camera_stream_bufferElliRKNSt3__16vectorImNS4_9allocatorImEEEEyi",

        "_ZN7android19Camera3OutputStream12returnBufferERKNS_20camera_stream_bufferElliRKNSt3__16vectorImNS1_9allocatorImEEEEmi",
        nullptr
    };

    void *handle = shadowhook_dlopen(CAMERASERVICE_LIB);
    if (handle) {
        void *seen_addrs[16] = {};
        int seen_count = 0;

        for (int i = 0; RTRN_SYMS[i] && count < max_count; i++) {
            void *addr = shadowhook_dlsym(handle, RTRN_SYMS[i]);
            if (!addr) addr = shadowhook_dlsym_symtab(handle, RTRN_SYMS[i]);
            if (!addr) continue;


            bool dup = false;
            for (int j = 0; j < seen_count; j++) {
                if (seen_addrs[j] == addr) { dup = true; break; }
            }
            if (dup) continue;
            if (seen_count < 16) seen_addrs[seen_count++] = addr;

            out[count].ptr     = addr;
            out[count].symbol  = RTRN_SYMS[i];
            out[count].source  = "known_list";
            out[count].variant = PCR_VARIANT_RTRN;
            out[count].size    = 0;
            LOGI("resolve_camera3_returnbuffer: known-list[%d] hit @ %p sym=%s",
                 count, addr, RTRN_SYMS[i]);
            count++;
        }
        shadowhook_dlclose(handle);
    }

    if (count == 0) {




        LOGW("resolve_camera3_returnbuffer: known-list missed — running ELF scan");

        struct RtrnCtx {
            ResolvedSymbol *out;
            int max_count;
            int count;
            void *seen_addrs[16];
            int seen_count;
        } rctx = {};
        rctx.out       = out;
        rctx.max_count = max_count;

        dl_iterate_phdr([](struct dl_phdr_info *info, size_t , void *data) -> int {
            bool is_target = false;
            const char *open_path = nullptr;
            if (info->dlpi_name && strstr(info->dlpi_name, "libcameraservice.so")) {
                is_target = true;
                open_path = info->dlpi_name;
            } else if (info->dlpi_name && strstr(info->dlpi_name, "cameraserver")) {
                is_target = true;
                open_path = info->dlpi_name;
            } else if (!info->dlpi_name || info->dlpi_name[0] == '\0') {
                is_target = true;
                open_path = "/proc/self/exe";
            }
            if (!is_target || !open_path) return 0;

            RtrnCtx *rc = (RtrnCtx *)data;

            int fd = open(open_path, O_RDONLY);
            if (fd < 0) return 0;
            struct stat st;
            if (fstat(fd, &st) < 0) { close(fd); return 0; }
            size_t file_sz = (size_t)st.st_size;
            void *mapped = mmap(nullptr, file_sz, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (mapped == MAP_FAILED) return 0;

            const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)mapped;
            if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
                ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
                munmap(mapped, file_sz);
                return 0;
            }

            const Elf64_Shdr *shdrs = (const Elf64_Shdr *)((char *)mapped + ehdr->e_shoff);
            const Elf64_Sym  *symtab  = nullptr;
            uint64_t          nsyms   = 0;
            const char       *strtab  = nullptr;

            for (int si = 0; si < (int)ehdr->e_shnum; si++) {
                const Elf64_Shdr *sh = &shdrs[si];
                if ((sh->sh_type == SHT_SYMTAB || sh->sh_type == SHT_DYNSYM) &&
                    sh->sh_entsize == sizeof(Elf64_Sym)) {
                    symtab = (const Elf64_Sym *)((char *)mapped + sh->sh_offset);
                    nsyms  = sh->sh_size / sizeof(Elf64_Sym);
                    if (sh->sh_link < (uint32_t)ehdr->e_shnum)
                        strtab = (const char *)mapped + shdrs[sh->sh_link].sh_offset;
                    if (sh->sh_type == SHT_SYMTAB) break;
                }
            }

            if (!symtab || !strtab) {
                munmap(mapped, file_sz);
                return 0;
            }

            for (uint64_t si = 0; si < nsyms && rc->count < rc->max_count; si++) {
                const Elf64_Sym *sym = &symtab[si];
                if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC) continue;
                if (sym->st_value == 0) continue;

                const char *name = strtab + sym->st_name;










                if (!strstr(name, "Camera3OutputStream")) continue;
                if (!strstr(name, "returnBuffer"))        continue;
                if (strstr(name, "Checked"))              continue;
                if (strstr(name, "Input"))                continue;
                if (strstr(name, "Locked"))               continue;

                void *addr = (void *)(info->dlpi_addr + sym->st_value);


                bool dup = false;
                for (int j = 0; j < rc->seen_count; j++) {
                    if (rc->seen_addrs[j] == addr) { dup = true; break; }
                }
                if (dup) continue;
                if (rc->seen_count < 16) rc->seen_addrs[rc->seen_count++] = addr;

                int status = 0;
                char *dem = abi::__cxa_demangle(name, nullptr, nullptr, &status);
                LOGI("resolve_camera3_returnbuffer: ELF scan hit[%d] @ %p sym=%s",
                     rc->count, addr, dem ? dem : name);
                if (dem) free(dem);

                rc->out[rc->count].ptr     = addr;
                rc->out[rc->count].symbol  = "<elf_scan>";
                rc->out[rc->count].source  = "elf_scan";
                rc->out[rc->count].variant = PCR_VARIANT_RTRN;
                rc->out[rc->count].size    = (size_t)sym->st_size;
                rc->count++;
            }

            munmap(mapped, file_sz);
            return (rc->count >= rc->max_count) ? 1 : 0;
        }, &rctx);

        count = rctx.count;
    }

    if (count == 0) {
        LOGW("resolve_camera3_returnbuffer: NOT FOUND — "
             "Camera3OutputStream::returnBuffer unavailable on this device/ROM. "
             "Will attempt returnBufferLocked fallback.");
    } else {
        LOGI("resolve_camera3_returnbuffer: found %d variant(s) — "
             "OPlus per-buffer injection point ready.", count);
    }
    return count;
}

int resolve_camera3_returnbufferlocked(ResolvedSymbol *out, int max_count) {
    if (!out || max_count <= 0) return 0;
    int count = 0;




    static const char *const RTRN_LOCKED_SYMS[] = {

        "_ZN7android7camera319Camera3OutputStream18returnBufferLockedERKNS1_20camera_stream_bufferElliRKNSt3__16vectorImNS4_9allocatorImEEEE",

        "_ZN7android7camera319Camera3OutputStream18returnBufferLockedERKNS1_20camera_stream_bufferExxbRKNSt3__16vectorImNS4_9allocatorImEEEE",

        "_ZN7android7camera319Camera3OutputStream18returnBufferLockedERKNS1_20camera_stream_bufferEllbRKNSt3__16vectorImNS4_9allocatorImEEEE",

        "_ZN7android19Camera3OutputStream18returnBufferLockedERKNS_20camera_stream_bufferElliRKNSt3__16vectorImNS1_9allocatorImEEEE",
        "_ZN7android19Camera3OutputStream18returnBufferLockedERKNS_20camera_stream_bufferEllbRKNSt3__16vectorImNS1_9allocatorImEEEE",
        nullptr
    };

    void *handle = shadowhook_dlopen(CAMERASERVICE_LIB);
    if (handle) {
        void *seen_addrs[16] = {};
        int seen_count = 0;

        for (int i = 0; RTRN_LOCKED_SYMS[i] && count < max_count; i++) {
            void *addr = shadowhook_dlsym(handle, RTRN_LOCKED_SYMS[i]);
            if (!addr) addr = shadowhook_dlsym_symtab(handle, RTRN_LOCKED_SYMS[i]);
            if (!addr) continue;

            bool dup = false;
            for (int j = 0; j < seen_count; j++) {
                if (seen_addrs[j] == addr) { dup = true; break; }
            }
            if (dup) continue;
            if (seen_count < 16) seen_addrs[seen_count++] = addr;

            out[count].ptr     = addr;
            out[count].symbol  = RTRN_LOCKED_SYMS[i];
            out[count].source  = "known_list";
            out[count].variant = PCR_VARIANT_RTRN_LOCKED;
            out[count].size    = 0;
            LOGI("resolve_camera3_returnbufferlocked: known-list[%d] hit @ %p sym=%s",
                 count, addr, RTRN_LOCKED_SYMS[i]);
            count++;
        }
        shadowhook_dlclose(handle);
    }

    if (count == 0) {



        LOGW("resolve_camera3_returnbufferlocked: known-list missed — running ELF scan");

        struct RtrnLockedCtx {
            ResolvedSymbol *out;
            int max_count;
            int count;
            void *seen_addrs[16];
            int seen_count;
        } rctx = {};
        rctx.out       = out;
        rctx.max_count = max_count;

        dl_iterate_phdr([](struct dl_phdr_info *info, size_t , void *data) -> int {
            bool is_target = false;
            const char *open_path = nullptr;
            if (info->dlpi_name && strstr(info->dlpi_name, "libcameraservice.so")) {
                is_target = true;
                open_path = info->dlpi_name;
            } else if (info->dlpi_name && strstr(info->dlpi_name, "cameraserver")) {
                is_target = true;
                open_path = info->dlpi_name;
            } else if (!info->dlpi_name || info->dlpi_name[0] == '\0') {
                is_target = true;
                open_path = "/proc/self/exe";
            }
            if (!is_target || !open_path) return 0;

            RtrnLockedCtx *rc = (RtrnLockedCtx *)data;

            int fd = open(open_path, O_RDONLY);
            if (fd < 0) return 0;
            struct stat st;
            if (fstat(fd, &st) < 0) { close(fd); return 0; }
            size_t file_sz = (size_t)st.st_size;
            void *mapped = mmap(nullptr, file_sz, PROT_READ, MAP_PRIVATE, fd, 0);
            close(fd);
            if (mapped == MAP_FAILED) return 0;

            const Elf64_Ehdr *ehdr = (const Elf64_Ehdr *)mapped;
            if (memcmp(ehdr->e_ident, ELFMAG, SELFMAG) != 0 ||
                ehdr->e_ident[EI_CLASS] != ELFCLASS64) {
                munmap(mapped, file_sz);
                return 0;
            }

            const Elf64_Shdr *shdrs = (const Elf64_Shdr *)((char *)mapped + ehdr->e_shoff);
            const Elf64_Sym  *symtab  = nullptr;
            uint64_t          nsyms   = 0;
            const char       *strtab  = nullptr;

            for (int si = 0; si < (int)ehdr->e_shnum; si++) {
                const Elf64_Shdr *sh = &shdrs[si];
                if ((sh->sh_type == SHT_SYMTAB || sh->sh_type == SHT_DYNSYM) &&
                    sh->sh_entsize == sizeof(Elf64_Sym)) {
                    symtab = (const Elf64_Sym *)((char *)mapped + sh->sh_offset);
                    nsyms  = sh->sh_size / sizeof(Elf64_Sym);
                    if (sh->sh_link < (uint32_t)ehdr->e_shnum)
                        strtab = (const char *)mapped + shdrs[sh->sh_link].sh_offset;
                    if (sh->sh_type == SHT_SYMTAB) break;
                }
            }

            if (!symtab || !strtab) {
                munmap(mapped, file_sz);
                return 0;
            }

            for (uint64_t si = 0; si < nsyms && rc->count < rc->max_count; si++) {
                const Elf64_Sym *sym = &symtab[si];
                if (ELF64_ST_TYPE(sym->st_info) != STT_FUNC) continue;
                if (sym->st_value == 0) continue;

                const char *name = strtab + sym->st_name;


                if (!strstr(name, "Camera3OutputStream"))   continue;
                if (!strstr(name, "returnBufferLocked"))    continue;
                if (strstr(name, "Checked"))                continue;
                if (strstr(name, "Input"))                  continue;

                void *addr = (void *)(info->dlpi_addr + sym->st_value);

                bool dup = false;
                for (int j = 0; j < rc->seen_count; j++) {
                    if (rc->seen_addrs[j] == addr) { dup = true; break; }
                }
                if (dup) continue;
                if (rc->seen_count < 16) rc->seen_addrs[rc->seen_count++] = addr;

                int status = 0;
                char *dem = abi::__cxa_demangle(name, nullptr, nullptr, &status);
                LOGI("resolve_camera3_returnbufferlocked: ELF scan hit[%d] @ %p sym=%s",
                     rc->count, addr, dem ? dem : name);
                if (dem) free(dem);

                rc->out[rc->count].ptr     = addr;
                rc->out[rc->count].symbol  = "<elf_scan>";
                rc->out[rc->count].source  = "elf_scan";
                rc->out[rc->count].variant = PCR_VARIANT_RTRN_LOCKED;
                rc->out[rc->count].size    = (size_t)sym->st_size;
                rc->count++;
            }

            munmap(mapped, file_sz);
            return (rc->count >= rc->max_count) ? 1 : 0;
        }, &rctx);

        count = rctx.count;
    }

    if (count == 0) {
        LOGW("resolve_camera3_returnbufferlocked: NOT FOUND — "
             "returnBufferLocked unavailable in this ROM's ELF. "
             "No per-buffer injection point available on this device.");
    } else {
        LOGI("resolve_camera3_returnbufferlocked: found %d variant(s) — "
             "OPlus fallback per-buffer injection point ready.", count);
    }
    return count;
}
