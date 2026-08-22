#pragma once
#include <stddef.h>

typedef enum {
    PCR_VARIANT_UNKNOWN = 0,


    PCR_VARIANT_MEMBER = 1,





    PCR_VARIANT_OUTPUTUTILS = 2,




    PCR_VARIANT_HIDL_3_4 = 3,


    PCR_VARIANT_HIDL_3_2 = 4,


    PCR_VARIANT_AIDL = 5,




    PCR_VARIANT_ROB = 6,








    PCR_VARIANT_RTRN = 7,









    PCR_VARIANT_RTRN_LOCKED = 8,
} PcrVariant;

typedef enum {
    CS_VARIANT_UNKNOWN = 0,



    CS_VARIANT_MEMBER_CFG = 1,


    CS_VARIANT_HIDL_HAL = 2,


    CS_VARIANT_AIDL_HAL = 3,
} CsVariant;

typedef struct {
    void       *ptr;
    const char *symbol;
    const char *source;
    int         variant;
    size_t      size;
} ResolvedSymbol;

#define MAX_PCR_VARIANTS 12
#define MAX_CS_VARIANTS  6

#ifdef __cplusplus
extern "C" {
#endif

ResolvedSymbol resolve_process_capture_result(void);
ResolvedSymbol resolve_configure_streams(void);

int resolve_all_pcr(ResolvedSymbol *out, int max_count);

int resolve_all_cs(ResolvedSymbol *out, int max_count);

#ifdef __cplusplus
}
#endif

#ifdef __cplusplus
extern "C" {
#endif

void resolve_camera3_usage_hooks(ResolvedSymbol *setusage, ResolvedSymbol *getendpoint);

int resolve_camera3_returnbuffer(ResolvedSymbol *out, int max_count);

int resolve_camera3_returnbufferlocked(ResolvedSymbol *out, int max_count);

#ifdef __cplusplus
}
#endif
