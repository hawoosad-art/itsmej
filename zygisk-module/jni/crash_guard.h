#pragma once

#ifdef __cplusplus
extern "C" {
#endif

static inline int  crash_guard_init(void)    { return 0; }
static inline void crash_guard_cleanup(void) { }
static inline int  crash_guard_enter(void)   { return 0; }
static inline void crash_guard_exit(void)    { }
static inline int  crash_guard_record_crash(void) { return 0; }
static inline int  crash_guard_is_disabled(void)  { return 0; }

#ifdef __cplusplus
}
#endif
