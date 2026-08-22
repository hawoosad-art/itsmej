#pragma once
#ifdef __cplusplus
extern "C" {
#endif

int  hook_proxy_install(void);
void hook_proxy_uninstall(void);
bool hook_proxy_is_active(void);
void hook_proxy_set_enabled(bool enabled);

#ifdef __cplusplus
}
#endif
