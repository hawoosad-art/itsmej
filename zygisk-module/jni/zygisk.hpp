

#pragma once

#include <jni.h>
#include <cstdint>
#include <functional>

namespace zygisk {

struct AppSpecializeArgs;
struct ServerSpecializeArgs;
class Api;
class ModuleBase;

enum class Option : int {
    DLCLOSE_MODULE_LIBRARY = 0,
    FORCE_DENYLIST_UNMOUNT = 1,
};

struct AppSpecializeArgs {
    jint  &uid;
    jint  &gid;
    jintArray   &gids;
    jint  &runtime_flags;
    jint  &mount_external;
    jstring &se_info;
    jstring &nice_name;
    jstring &instruction_set;
    jstring &app_data_dir;

    jboolean *const is_child_zygote;
    jboolean *const is_top_app;
    jobjectArray *const pkg_data_info_list;
    jobjectArray *const whitelisted_data_info_list;
    jboolean *const mount_data_dirs;
    jboolean *const mount_storage_dirs;

protected:
    AppSpecializeArgs() = delete;
};

struct ServerSpecializeArgs {
    jint &uid;
    jint &gid;
    jintArray &gids;
    jint &runtime_flags;
    jlong &permitted_capabilities;
    jlong &effective_capabilities;

protected:
    ServerSpecializeArgs() = delete;
};

namespace internal {
struct api_table;
struct module_abi;
}

class Api {
public:
    void setOption(Option opt);
    int  getModuleDir();
    bool exemptFd(int fd);
    void *connectCompanion();
    void *getModuleData();



    void _set_impl(internal::api_table *t) { impl = t; }

private:
    internal::api_table *impl;
};

class ModuleBase {
public:
    virtual void onLoad(Api *api, JNIEnv *env) {}
    virtual void preAppSpecialize(AppSpecializeArgs *args) {}
    virtual void postAppSpecialize(const AppSpecializeArgs *args) {}
    virtual void preServerSpecialize(ServerSpecializeArgs *args) {}
    virtual void postServerSpecialize(const ServerSpecializeArgs *args) {}
    virtual ~ModuleBase() = default;
};

namespace internal {

struct module_abi {
    long api_version;
    ModuleBase *impl;

    void (*preAppSpecialize)(ModuleBase *, AppSpecializeArgs *);
    void (*postAppSpecialize)(ModuleBase *, const AppSpecializeArgs *);
    void (*preServerSpecialize)(ModuleBase *, ServerSpecializeArgs *);
    void (*postServerSpecialize)(ModuleBase *, const ServerSpecializeArgs *);
};

struct api_table {
    void *impl;
    bool (*registerModule)(api_table *, module_abi *);

    void  (*setOption)(api_table *, Option);
    int   (*getModuleDir)(api_table *);
    bool  (*exemptFd)(api_table *, int);
    bool  (*pltHookRegister)(api_table *, const char *, const char *, void *, void **);
    bool  (*pltHookExclude)(api_table *, const char *, const char *);
    bool  (*pltHookCommit)(api_table *);
    int   (*connectCompanion)(api_table *);
    void  (*setModuleData)(api_table *, void *);
    void *(*getModuleData)(api_table *);
};

template <class T>
void *entry_impl(internal::api_table *table, JNIEnv *env) {
    auto *mod = new T();
    static module_abi abi;
    abi.api_version = 4;
    abi.impl        = mod;
    abi.preAppSpecialize    = [](ModuleBase *b, AppSpecializeArgs *a) { b->preAppSpecialize(a); };
    abi.postAppSpecialize   = [](ModuleBase *b, const AppSpecializeArgs *a) { b->postAppSpecialize(a); };
    abi.preServerSpecialize = [](ModuleBase *b, ServerSpecializeArgs *a) { b->preServerSpecialize(a); };
    abi.postServerSpecialize= [](ModuleBase *b, const ServerSpecializeArgs *a) { b->postServerSpecialize(a); };
    table->registerModule(table, &abi);

    auto *api = new Api();
    api->_set_impl(table);
    mod->onLoad(api, env);
    return nullptr;
}

}

inline void Api::setOption(Option opt) {
    impl->setOption(impl, opt);
}
inline int Api::getModuleDir() {
    return impl->getModuleDir(impl);
}
inline bool Api::exemptFd(int fd) {
    return impl->exemptFd(impl, fd);
}

using EntryFn = void *(*)(internal::api_table *, JNIEnv *);
using CompanionFn = void (*)(int);

}

#define REGISTER_ZYGISK_MODULE(clazz)                                        \
    __attribute__((visibility("default")))                                    \
    void *zygisk_module_entry(zygisk::internal::api_table *table, JNIEnv *env) { \
        return zygisk::internal::entry_impl<clazz>(table, env);              \
    }

#define REGISTER_ZYGISK_COMPANION(func)                                       \
    __attribute__((visibility("default")))                                    \
    void zygisk_companion_entry(int client) { func(client); }
