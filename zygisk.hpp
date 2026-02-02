// Zygisk API v4 - Minimal compatible header
// https://github.com/topjohnwu/zygisk-module-sample

#pragma once

#include <jni.h>

namespace zygisk {

struct Api;
struct AppSpecializeArgs;
struct ServerSpecializeArgs;

struct AppSpecializeArgs {
    jint& uid;
    jint& gid;
    jintArray& gids;
    jint& runtime_flags;
    jobjectArray& rlimits;
    jint& mount_external;
    jstring& se_info;
    jstring& nice_name;
    jstring& instruction_set;
    jstring& app_data_dir;

    jintArray *fds_to_ignore = nullptr;
    jboolean *is_child_zygote = nullptr;
    jboolean *is_top_app = nullptr;
    jobjectArray *pkg_data_info_list = nullptr;
    jobjectArray *whitelisted_data_info_list = nullptr;
    jboolean *mount_data_dirs = nullptr;
    jboolean *mount_storage_dirs = nullptr;
};

struct ServerSpecializeArgs {
    jint& uid;
    jint& gid;
    jintArray& gids;
    jint& runtime_flags;
    jlong& permitted_capabilities;
    jlong& effective_capabilities;
};

enum Option : int {
    FORCE_DENYLIST_UNMOUNT = 0,
    DLCLOSE_MODULE_LIBRARY = 1,
};

enum StateFlag : uint32_t {
    PROCESS_GRANTED_ROOT = (1u << 0),
    PROCESS_ON_DENYLIST = (1u << 1),
};

struct Api {
    bool setOption(Option opt);
    uint32_t getFlags();
    int connectCompanion();
    void setError(const char *fmt, ...);

    void pltHookRegister(const char *regex, const char *symbol, void *newFunc, void **oldFunc);
    void pltHookExclude(const char *regex, const char *symbol);
    bool pltHookCommit();
};

class ModuleBase {
public:
    virtual void onLoad([[maybe_unused]] Api *api, [[maybe_unused]] JNIEnv *env) {}
    virtual void preAppSpecialize([[maybe_unused]] AppSpecializeArgs *args) {}
    virtual void postAppSpecialize([[maybe_unused]] const AppSpecializeArgs *args) {}
    virtual void preServerSpecialize([[maybe_unused]] ServerSpecializeArgs *args) {}
    virtual void postServerSpecialize([[maybe_unused]] const ServerSpecializeArgs *args) {}
    virtual ~ModuleBase() = default;
};

} // namespace zygisk

#define REGISTER_ZYGISK_MODULE(clazz) \
void *zygisk_module_entry(void *api, JNIEnv *env) { \
    static clazz module; \
    module.onLoad(reinterpret_cast<zygisk::Api*>(api), env); \
    return &module; \
}

#define REGISTER_ZYGISK_COMPANION(func) \
void zygisk_companion_entry(int fd) { func(fd); }
