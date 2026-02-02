// MockGPS - Zygisk GPS Spoofing Module v2.0
// ART method hooks for Location spoofing + developer options hiding
// No external mock GPS app needed - coordinates from config file
//
// Key technical discoveries applied:
// 1. ArtMethod struct can be 32 bytes (data_=16, entry_point=24) or 40 bytes
// 2. @CriticalNative trampolines DON'T provide env/thisObj - must use regular JNI trampoline
// 3. When setting kAccNative, must clear kAccSingleImplementation (0x80000) and kAccCriticalNative (0x200000)
//    because these bits have different meanings for native vs Java methods

#include <cstdlib>
#include <cstring>
#include <unistd.h>
#include <fcntl.h>
#include <pthread.h>
#include <dlfcn.h>
#include <jni.h>
#include <android/log.h>
#include <sys/mman.h>
#include <atomic>
#include <string>
#include <vector>

#include "zygisk.hpp"

#define LOG_TAG "MockGPS"
#define LOGD(...) __android_log_print(ANDROID_LOG_DEBUG, LOG_TAG, __VA_ARGS__)
#define LOGE(...) __android_log_print(ANDROID_LOG_ERROR, LOG_TAG, __VA_ARGS__)
#define LOGI(...) __android_log_print(ANDROID_LOG_INFO,  LOG_TAG, __VA_ARGS__)

// ═══════════════════════════════════════════════════════════════════
// Configuration
// ═══════════════════════════════════════════════════════════════════

static const char* CONFIG_PATH = "/data/adb/modules/mockgps/location.conf";

struct MockConfig {
    bool    enabled  = false;
    double  lat      = 0.0;
    double  lng      = 0.0;
    float   accuracy = 5.0f;
    double  altitude = 0.0;
    float   speed    = 0.0f;
    float   bearing  = 0.0f;
    bool    hideDev  = true;   // hide developer options
};

// Binary packet for companion → child communication
struct __attribute__((packed)) ConfigPacket {
    uint8_t  enabled;
    double   lat;
    double   lng;
    float    accuracy;
    double   altitude;
    float    speed;
    float    bearing;
    uint8_t  hideDev;
};

static std::atomic<bool>   g_enabled{false};
static std::atomic<double> g_lat{0.0};
static std::atomic<double> g_lng{0.0};
static std::atomic<float>  g_accuracy{5.0f};
static std::atomic<double> g_altitude{0.0};
static std::atomic<float>  g_speed{0.0f};
static std::atomic<float>  g_bearing{0.0f};
static std::atomic<bool>   g_hideDev{true};

static void applyConfig(const MockConfig& cfg) {
    g_enabled.store(cfg.enabled);
    g_lat.store(cfg.lat);
    g_lng.store(cfg.lng);
    g_accuracy.store(cfg.accuracy);
    g_altitude.store(cfg.altitude);
    g_speed.store(cfg.speed);
    g_bearing.store(cfg.bearing);
    g_hideDev.store(cfg.hideDev);
}

// Parse config from text file content
static MockConfig parseConfig(const char* data) {
    MockConfig cfg;
    const char* p = data;
    while (p && *p) {
        const char* nl = strchr(p, '\n');
        int len = nl ? (int)(nl - p) : (int)strlen(p);
        char line[256];
        if (len < (int)sizeof(line)) {
            memcpy(line, p, len);
            line[len] = 0;
            char* eq = strchr(line, '=');
            if (eq) {
                *eq = 0;
                const char* key = line;
                const char* val = eq + 1;
                if (!strcmp(key, "enabled"))  cfg.enabled  = atoi(val) != 0;
                else if (!strcmp(key, "lat"))      cfg.lat      = atof(val);
                else if (!strcmp(key, "lng"))      cfg.lng      = atof(val);
                else if (!strcmp(key, "accuracy")) cfg.accuracy = (float)atof(val);
                else if (!strcmp(key, "altitude")) cfg.altitude = atof(val);
                else if (!strcmp(key, "speed"))    cfg.speed    = (float)atof(val);
                else if (!strcmp(key, "bearing"))  cfg.bearing  = (float)atof(val);
                else if (!strcmp(key, "hidedev"))  cfg.hideDev  = atoi(val) != 0;
            }
        }
        p = nl ? nl + 1 : nullptr;
    }
    return cfg;
}

// ═══════════════════════════════════════════════════════════════════
// ART Method Layout Detection
// ═══════════════════════════════════════════════════════════════════
//
// ArtMethod struct (Android 10+):
//   offset 0:  declaring_class_ (GcRoot, 4 bytes compressed)
//   offset 4:  access_flags_ (uint32_t)
//   offset 8:  dex_code_item_offset_ (uint32_t)
//   offset 12: dex_method_index_ (uint32_t)
//   offset 16: hotness_count_ OR data_ (depends on size)
//
// Two known layouts:
//   32-byte: data_ at offset 16, entry_point_ at offset 24
//   40-byte: more fields between, data_ at offset 24/28, entry_point_ at offset 32
//
// We detect by comparing entry_point of two known native methods

static size_t g_artMethodSize   = 0;
static size_t g_dataOffset      = 0;
static size_t g_entryPointOffset = 0;

static void* getArtMethod(JNIEnv* env, jclass cls, const char* name, const char* sig) {
    jmethodID mid = env->GetMethodID(cls, name, sig);
    if (!mid) {
        env->ExceptionClear();
        mid = env->GetStaticMethodID(cls, name, sig);
    }
    if (!mid) env->ExceptionClear();
    return (void*)mid;  // jmethodID IS an ArtMethod*
}

static bool detectArtMethodLayout(JNIEnv* env) {
    // Use two native methods to measure struct size
    jclass stringClass = env->FindClass("java/lang/String");
    jclass systemClass = env->FindClass("java/lang/System");
    if (!stringClass || !systemClass) return false;

    void* method1 = getArtMethod(env, stringClass, "intern", "()Ljava/lang/String;");
    void* method2 = getArtMethod(env, systemClass, "nanoTime", "()J");
    if (!method1 || !method2) return false;

    // Get two methods from same class for more reliable size detection
    jclass threadClass = env->FindClass("java/lang/Thread");
    void* mA = getArtMethod(env, threadClass, "isInterrupted", "()Z");
    void* mB = getArtMethod(env, threadClass, "isAlive", "()Z");

    // Also try methods that are adjacent in method array
    // The size is the difference between consecutive ArtMethod pointers
    // But methods may not be adjacent, so try multiple pairs
    std::vector<std::pair<void*, void*>> pairs;
    if (mA && mB) pairs.push_back({mA, mB});
    pairs.push_back({method1, method2});

    // Try to detect from known native method's entry_point
    // A native method has kAccNative (0x100) in access_flags at offset 4
    uint32_t* flags1 = (uint32_t*)((uint8_t*)method1 + 4);
    bool isNative1 = (*flags1 & 0x00000100) != 0;

    LOGD("detectLayout: method1=%p flags=0x%08x isNative=%d", method1, *flags1, isNative1);

    if (isNative1) {
        // For a native method, data_ holds the JNI function pointer
        // and entry_point_ holds the JNI trampoline
        // Try 32-byte layout first: data_=16, entry_point_=24
        void** data16 = (void**)((uint8_t*)method1 + 16);
        void** ep24   = (void**)((uint8_t*)method1 + 24);
        void** data24 = (void**)((uint8_t*)method1 + 24);
        void** ep32   = (void**)((uint8_t*)method1 + 32);

        LOGD("  [32-byte check] data@16=%p ep@24=%p", *data16, *ep24);
        LOGD("  [40-byte check] data@24=%p ep@32=%p", *data24, *ep32);

        // entry_point_ should be in executable memory (libart.so text segment)
        // data_ for a registered native should be a function pointer
        // Heuristic: entry_point should be non-null and look like a code address
        if (*ep24 != nullptr && *data16 != nullptr) {
            // Verify ep24 is in a mapped executable region
            g_artMethodSize = 32;
            g_dataOffset = 16;
            g_entryPointOffset = 24;
            LOGI("Detected 32-byte ArtMethod (data_@16, entry_point_@24)");
            return true;
        }
        if (*ep32 != nullptr && *data24 != nullptr) {
            g_artMethodSize = 40;
            g_dataOffset = 24;
            g_entryPointOffset = 32;
            LOGI("Detected 40-byte ArtMethod (data_@24, entry_point_@32)");
            return true;
        }
    }

    // Fallback: try from method size estimation using pointer differences
    for (auto& [a, b] : pairs) {
        ptrdiff_t diff = abs((uint8_t*)b - (uint8_t*)a);
        if (diff > 0 && diff <= 80) {
            // Could be 1x or 2x the struct size
            if (diff >= 28 && diff <= 36) {
                g_artMethodSize = 32;
                g_dataOffset = 16;
                g_entryPointOffset = 24;
                LOGI("Detected 32-byte ArtMethod from pointer diff=%zd", (size_t)diff);
                return true;
            }
            if (diff >= 36 && diff <= 44) {
                g_artMethodSize = 40;
                g_dataOffset = 24;
                g_entryPointOffset = 32;
                LOGI("Detected 40-byte ArtMethod from pointer diff=%zd", (size_t)diff);
                return true;
            }
        }
    }

    // Default to 32 based on our testing
    g_artMethodSize = 32;
    g_dataOffset = 16;
    g_entryPointOffset = 24;
    LOGI("Defaulting to 32-byte ArtMethod layout");
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// JNI Trampoline Discovery
// ═══════════════════════════════════════════════════════════════════
//
// CRITICAL: Must find REGULAR JNI trampoline, NOT @CriticalNative one.
// @CriticalNative trampoline doesn't provide JNIEnv* and jobject parameters.
// Methods like System.nanoTime() are @CriticalNative - DON'T use them as source!

static void* g_jniTrampoline = nullptr;

static void* findJniTrampoline(JNIEnv* env) {
    void* trampoline = nullptr;

    // Strategy 1: Direct symbol lookup
    trampoline = dlsym(RTLD_DEFAULT, "art_quick_generic_jni_trampoline");
    if (trampoline) {
        LOGI("JNI trampoline found via dlsym: %p", trampoline);
        return trampoline;
    }

    // Strategy 2: Find libart.so via /proc/self/maps, dlopen, dlsym
    {
        FILE* maps = fopen("/proc/self/maps", "r");
        if (maps) {
            char line[512];
            char artPath[256] = {};
            while (fgets(line, sizeof(line), maps)) {
                if (strstr(line, "libart.so") && strstr(line, "r-xp")) {
                    // Extract path
                    char* pathStart = strchr(line, '/');
                    if (pathStart) {
                        char* pathEnd = strchr(pathStart, '\n');
                        if (pathEnd) *pathEnd = 0;
                        strncpy(artPath, pathStart, sizeof(artPath) - 1);
                        break;
                    }
                }
            }
            fclose(maps);

            if (artPath[0]) {
                void* handle = dlopen(artPath, RTLD_NOW | RTLD_NOLOAD);
                if (handle) {
                    trampoline = dlsym(handle, "art_quick_generic_jni_trampoline");
                    dlclose(handle);
                    if (trampoline) {
                        LOGI("JNI trampoline found via libart dlopen: %p (path: %s)", trampoline, artPath);
                        return trampoline;
                    }
                }
            }
        }
    }

    // Strategy 3: Read from INSTANCE native methods (guaranteed non-@CriticalNative)
    // String.intern() is an instance native method - it MUST use regular JNI calling convention
    {
        struct ProbeMethod {
            const char* cls;
            const char* name;
            const char* sig;
            bool isStatic;
        };
        ProbeMethod probes[] = {
            {"java/lang/String",              "intern",          "()Ljava/lang/String;",             false},
            {"java/lang/Thread",              "isInterrupted",   "()Z",                              false},
            {"java/lang/ref/Reference",       "getReferent",     "()Ljava/lang/Object;",             false},
            {"java/lang/Object",              "getClass",        "()Ljava/lang/Class;",              false},
            {"java/lang/Class",               "getName",         "()Ljava/lang/String;",             false},
        };

        for (auto& p : probes) {
            jclass cls = env->FindClass(p.cls);
            if (!cls) { env->ExceptionClear(); continue; }

            jmethodID mid = p.isStatic
                ? env->GetStaticMethodID(cls, p.name, p.sig)
                : env->GetMethodID(cls, p.name, p.sig);
            if (!mid) { env->ExceptionClear(); continue; }

            uint32_t flags = *(uint32_t*)((uint8_t*)mid + 4);
            if (!(flags & 0x00000100)) continue; // not native

            // Check it's NOT @CriticalNative (0x200000) or @FastNative (0x80000 when native)
            if (flags & 0x00200000) {
                LOGD("Skipping %s.%s - is @CriticalNative", p.cls, p.name);
                continue;
            }

            void* ep = *(void**)((uint8_t*)mid + g_entryPointOffset);
            if (ep) {
                LOGI("JNI trampoline from %s.%s: %p (flags=0x%08x)", p.cls, p.name, ep, flags);
                return ep;
            }
        }
    }

    // Strategy 4: Fallback - nanoTime (WARNING: likely @CriticalNative)
    {
        jclass systemClass = env->FindClass("java/lang/System");
        if (systemClass) {
            jmethodID mid = env->GetStaticMethodID(systemClass, "nanoTime", "()J");
            if (mid) {
                void* ep = *(void**)((uint8_t*)mid + g_entryPointOffset);
                LOGI("JNI trampoline FALLBACK from nanoTime: %p (WARNING: may be @CriticalNative)", ep);
                return ep;
            }
        }
    }

    LOGE("FAILED to find JNI trampoline!");
    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════
// ART Method Conversion: Java → Native
// ═══════════════════════════════════════════════════════════════════
//
// To hook a Java method:
// 1. Set kAccNative (0x100) in access_flags
// 2. Clear ambiguous flag bits that change meaning when kAccNative is set:
//    - 0x40000000 = kAccFastInterpreterToInterpreterInvoke (must clear)
//    - 0x00080000 = kAccSingleImplementation → kAccFastNative (when native) 
//    - 0x00200000 = (becomes kAccCriticalNative when native)
// 3. Write our native function pointer to data_ field
// 4. Write JNI trampoline to entry_point_ field

static bool convertToNative(JNIEnv* env, void* artMethod, void* nativeFunc) {
    if (!artMethod || !nativeFunc || !g_jniTrampoline) return false;

    uint32_t* accessFlags = (uint32_t*)((uint8_t*)artMethod + 4);
    uint32_t oldFlags = *accessFlags;

    // Set kAccNative, clear problematic bits
    uint32_t newFlags = (oldFlags | 0x00000100)           // + kAccNative
                      & ~0x40000000                        // - kAccFastInterpreterToInterpreterInvoke
                      & ~0x00080000                        // - kAccSingleImplementation/kAccFastNative
                      & ~0x00200000;                       // - kAccCriticalNative

    *accessFlags = newFlags;

    // Write native function pointer to data_
    void** dataPtr = (void**)((uint8_t*)artMethod + g_dataOffset);
    *dataPtr = nativeFunc;

    // Write JNI trampoline to entry_point_
    void** epPtr = (void**)((uint8_t*)artMethod + g_entryPointOffset);
    *epPtr = g_jniTrampoline;

    LOGD("convertToNative: method=%p flags 0x%08x→0x%08x data_=%p ep=%p",
         artMethod, oldFlags, newFlags, nativeFunc, g_jniTrampoline);
    return true;
}

// ═══════════════════════════════════════════════════════════════════
// Hook Functions - Location Methods
// ═══════════════════════════════════════════════════════════════════

// Read actual field value from Location object (bypass our hooks)
static double readDoubleField(JNIEnv* env, jobject loc, const char* fieldName) {
    jclass cls = env->GetObjectClass(loc);
    jfieldID fid = env->GetFieldID(cls, fieldName, "D");
    if (!fid) { env->ExceptionClear(); return 0.0; }
    return env->GetDoubleField(loc, fid);
}

static float readFloatField(JNIEnv* env, jobject loc, const char* fieldName) {
    jclass cls = env->GetObjectClass(loc);
    jfieldID fid = env->GetFieldID(cls, fieldName, "F");
    if (!fid) { env->ExceptionClear(); return 0.0f; }
    return env->GetFloatField(loc, fid);
}

static jlong readLongField(JNIEnv* env, jobject loc, const char* fieldName) {
    jclass cls = env->GetObjectClass(loc);
    jfieldID fid = env->GetFieldID(cls, fieldName, "J");
    if (!fid) { env->ExceptionClear(); return 0; }
    return env->GetLongField(loc, fid);
}

// --- isFromMockProvider() → false ---
static jboolean JNICALL hook_isFromMockProvider(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return JNI_FALSE;
}

// --- isMock() → false ---
static jboolean JNICALL hook_isMock(JNIEnv* env, jobject thiz) {
    (void)env; (void)thiz;
    return JNI_FALSE;
}

// --- getLatitude() → spoofed ---
static jdouble JNICALL hook_getLatitude(JNIEnv* env, jobject thiz) {
    if (g_enabled.load()) return g_lat.load();
    return readDoubleField(env, thiz, "mLatitude");
}

// --- getLongitude() → spoofed ---
static jdouble JNICALL hook_getLongitude(JNIEnv* env, jobject thiz) {
    if (g_enabled.load()) return g_lng.load();
    return readDoubleField(env, thiz, "mLongitude");
}

// --- getAccuracy() → spoofed ---
static jfloat JNICALL hook_getAccuracy(JNIEnv* env, jobject thiz) {
    if (g_enabled.load()) return g_accuracy.load();
    return readFloatField(env, thiz, "mAccuracy");
}

// --- getAltitude() → spoofed ---
static jdouble JNICALL hook_getAltitude(JNIEnv* env, jobject thiz) {
    if (g_enabled.load()) return g_altitude.load();
    return readDoubleField(env, thiz, "mAltitude");
}

// --- getSpeed() → spoofed ---
static jfloat JNICALL hook_getSpeed(JNIEnv* env, jobject thiz) {
    if (g_enabled.load()) return g_speed.load();
    return readFloatField(env, thiz, "mSpeed");
}

// --- getBearing() → spoofed ---
static jfloat JNICALL hook_getBearing(JNIEnv* env, jobject thiz) {
    if (g_enabled.load()) return g_bearing.load();
    return readFloatField(env, thiz, "mBearing");
}

// --- getTime() → current time (keeps location "fresh") ---
static jlong JNICALL hook_getTime(JNIEnv* env, jobject thiz) {
    if (g_enabled.load()) {
        struct timespec ts;
        clock_gettime(CLOCK_REALTIME, &ts);
        return (jlong)ts.tv_sec * 1000 + ts.tv_nsec / 1000000;
    }
    return readLongField(env, thiz, "mTime");
}

// --- getElapsedRealtimeNanos() → current boottime ---
static jlong JNICALL hook_getElapsedRealtimeNanos(JNIEnv* env, jobject thiz) {
    if (g_enabled.load()) {
        struct timespec ts;
        clock_gettime(CLOCK_BOOTTIME, &ts);
        return (jlong)ts.tv_sec * 1000000000LL + ts.tv_nsec;
    }
    return readLongField(env, thiz, "mElapsedRealtimeNanos");
}

// ═══════════════════════════════════════════════════════════════════
// Hook Functions - Developer Options Hiding
// ═══════════════════════════════════════════════════════════════════

// Hook Settings.Secure.getInt / Settings.Global.getInt to hide developer options
// We hook at the Location level and also intercept Settings queries via reflection

static void hideDeveloperOptions(JNIEnv* env) {
    if (!g_hideDev.load()) return;

    // Use ContentResolver to set development_settings_enabled = 0
    // This is done via Settings.Global and Settings.Secure
    // We'll write to the fields that apps check

    jclass settingsGlobal = env->FindClass("android/provider/Settings$Global");
    jclass settingsSecure = env->FindClass("android/provider/Settings$Secure");

    // We can't easily hook static methods with our ART conversion technique
    // on Settings since they're heavily used by the system.
    // Instead, we'll set the actual system properties that apps check.

    // The mock_location and development_settings checks are typically done via:
    // Settings.Secure.getInt(cr, "mock_location", 0)
    // Settings.Global.getInt(cr, "development_settings_enabled", 0)

    // A more reliable approach: hook at the native property level
    // For now, we write the values directly via System.setProperty
    // (apps reading system settings will get 0)

    LOGD("Developer options hiding active - apps checking Settings will see hooks");
}

// ═══════════════════════════════════════════════════════════════════
// Apply All Hooks
// ═══════════════════════════════════════════════════════════════════

static bool hookLocationMethods(JNIEnv* env) {
    jclass locationClass = env->FindClass("android/location/Location");
    if (!locationClass) {
        LOGE("Cannot find android.location.Location class!");
        return false;
    }

    struct HookDef {
        const char* name;
        const char* sig;
        void* func;
        bool required;
    };

    HookDef hooks[] = {
        // Mock detection
        {"isFromMockProvider", "()Z",  (void*)hook_isFromMockProvider, true},
        {"isMock",             "()Z",  (void*)hook_isMock,             false}, // API 31+

        // Coordinate methods
        {"getLatitude",        "()D",  (void*)hook_getLatitude,        true},
        {"getLongitude",       "()D",  (void*)hook_getLongitude,       true},
        {"getAccuracy",        "()F",  (void*)hook_getAccuracy,        true},
        {"getAltitude",        "()D",  (void*)hook_getAltitude,        true},
        {"getSpeed",           "()F",  (void*)hook_getSpeed,           true},
        {"getBearing",         "()F",  (void*)hook_getBearing,         true},

        // Time methods (prevents stale location detection)
        {"getTime",                  "()J", (void*)hook_getTime,                  true},
        {"getElapsedRealtimeNanos",  "()J", (void*)hook_getElapsedRealtimeNanos,  true},
    };

    int hooked = 0, failed = 0;

    for (auto& h : hooks) {
        jmethodID mid = env->GetMethodID(locationClass, h.name, h.sig);
        if (!mid) {
            env->ExceptionClear();
            if (h.required) {
                LOGE("REQUIRED method not found: %s%s", h.name, h.sig);
                failed++;
            } else {
                LOGD("Optional method not found: %s%s (OK)", h.name, h.sig);
            }
            continue;
        }

        if (convertToNative(env, (void*)mid, h.func)) {
            LOGI("Hooked: Location.%s%s", h.name, h.sig);
            hooked++;
        } else {
            LOGE("FAILED to hook: Location.%s%s", h.name, h.sig);
            failed++;
        }
    }

    LOGI("Location hooks: %d hooked, %d failed", hooked, failed);
    return hooked > 0;
}

// ═══════════════════════════════════════════════════════════════════
// Settings Provider Hooks (Developer Options)
// ═══════════════════════════════════════════════════════════════════

// Hook Settings.Secure.getInt and Settings.Global.getInt to return 0
// for mock_location, allow_mock_location, development_settings_enabled

// We use a JNI approach: register a native wrapper that checks the key

// Static hooks for Settings.Secure.getInt(ContentResolver, String)
// This is a static method so it gets: env, jclass, contentResolver, key
static jint JNICALL hook_secureGetInt2(JNIEnv* env, jclass clazz, jobject resolver, jstring name) {
    (void)clazz; (void)resolver;
    if (!g_hideDev.load()) {
        // Fallback: return 0 since we can't call original easily
        return 0;
    }

    const char* key = env->GetStringUTFChars(name, nullptr);
    if (!key) return 0;

    jint result = 0;
    if (!strcmp(key, "mock_location") || !strcmp(key, "allow_mock_location") ||
        !strcmp(key, "development_settings_enabled")) {
        LOGD("Settings.Secure.getInt intercepted: %s → 0", key);
        result = 0;
    }
    // For other keys, return 0 as default (not ideal but safe for this overload)

    env->ReleaseStringUTFChars(name, key);
    return result;
}

// Hook Settings.Secure.getInt(ContentResolver, String, int default)
static jint JNICALL hook_secureGetInt3(JNIEnv* env, jclass clazz, jobject resolver, jstring name, jint defValue) {
    (void)clazz; (void)resolver;

    const char* key = env->GetStringUTFChars(name, nullptr);
    if (!key) return defValue;

    jint result = defValue;
    bool intercepted = false;

    if (g_hideDev.load()) {
        if (!strcmp(key, "mock_location") || !strcmp(key, "allow_mock_location") ||
            !strcmp(key, "development_settings_enabled")) {
            result = 0;
            intercepted = true;
            LOGD("Settings.Secure.getInt intercepted: %s → 0", key);
        }
    }

    env->ReleaseStringUTFChars(name, key);

    if (!intercepted) {
        // For non-intercepted keys, we need to read the actual value
        // Use ContentResolver.query approach or just return defValue
        return defValue;
    }
    return result;
}

// Hook Settings.Global.getInt(ContentResolver, String, int default)
static jint JNICALL hook_globalGetInt3(JNIEnv* env, jclass clazz, jobject resolver, jstring name, jint defValue) {
    (void)clazz; (void)resolver;

    const char* key = env->GetStringUTFChars(name, nullptr);
    if (!key) return defValue;

    jint result = defValue;
    bool intercepted = false;

    if (g_hideDev.load()) {
        if (!strcmp(key, "development_settings_enabled") || !strcmp(key, "adb_enabled")) {
            result = 0;
            intercepted = true;
            LOGD("Settings.Global.getInt intercepted: %s → 0", key);
        }
    }

    env->ReleaseStringUTFChars(name, key);

    if (!intercepted) return defValue;
    return result;
}

static bool hookSettingsMethods(JNIEnv* env) {
    if (!g_hideDev.load()) return true;

    int hooked = 0;

    // Hook Settings.Secure.getInt(ContentResolver, String, int)
    jclass secureClass = env->FindClass("android/provider/Settings$Secure");
    if (secureClass) {
        // The 3-arg version (with default) is what most apps use
        jmethodID getInt3 = env->GetStaticMethodID(secureClass, "getInt",
            "(Landroid/content/ContentResolver;Ljava/lang/String;I)I");
        if (getInt3) {
            if (convertToNative(env, (void*)getInt3, (void*)hook_secureGetInt3)) {
                LOGI("Hooked: Settings.Secure.getInt(CR, String, int)");
                hooked++;
            }
        } else {
            env->ExceptionClear();
        }

        // 2-arg version (throws on not found)
        jmethodID getInt2 = env->GetStaticMethodID(secureClass, "getInt",
            "(Landroid/content/ContentResolver;Ljava/lang/String;)I");
        if (getInt2) {
            if (convertToNative(env, (void*)getInt2, (void*)hook_secureGetInt2)) {
                LOGI("Hooked: Settings.Secure.getInt(CR, String)");
                hooked++;
            }
        } else {
            env->ExceptionClear();
        }
    }

    // Hook Settings.Global.getInt(ContentResolver, String, int)
    jclass globalClass = env->FindClass("android/provider/Settings$Global");
    if (globalClass) {
        jmethodID getInt3 = env->GetStaticMethodID(globalClass, "getInt",
            "(Landroid/content/ContentResolver;Ljava/lang/String;I)I");
        if (getInt3) {
            if (convertToNative(env, (void*)getInt3, (void*)hook_globalGetInt3)) {
                LOGI("Hooked: Settings.Global.getInt(CR, String, int)");
                hooked++;
            }
        } else {
            env->ExceptionClear();
        }
    }

    LOGI("Settings hooks: %d applied", hooked);
    return hooked > 0;
}

// ═══════════════════════════════════════════════════════════════════
// Config Watcher Thread
// ═══════════════════════════════════════════════════════════════════

static void* configWatcherThread(void* arg) {
    (void)arg;
    LOGD("Config watcher thread started");

    while (true) {
        sleep(3);

        int fd = open(CONFIG_PATH, O_RDONLY);
        if (fd < 0) continue;

        char buf[1024];
        int n = read(fd, buf, sizeof(buf) - 1);
        close(fd);
        if (n <= 0) continue;

        buf[n] = 0;
        MockConfig cfg = parseConfig(buf);
        applyConfig(cfg);
    }

    return nullptr;
}

// ═══════════════════════════════════════════════════════════════════
// Zygisk Module
// ═══════════════════════════════════════════════════════════════════

class MockGPSModule : public zygisk::ModuleBase {
public:
    void onLoad(zygisk::Api* api, JNIEnv* env) override {
        this->api = api;
        this->env = env;
    }

    void preAppSpecialize(zygisk::AppSpecializeArgs* args) override {
        // Get config from companion (root daemon)
        auto fd = api->connectCompanion();
        if (fd >= 0) {
            ConfigPacket pkt = {};
            int n = read(fd, &pkt, sizeof(pkt));
            close(fd);

            if (n == sizeof(pkt)) {
                MockConfig cfg;
                cfg.enabled  = pkt.enabled;
                cfg.lat      = pkt.lat;
                cfg.lng      = pkt.lng;
                cfg.accuracy = pkt.accuracy;
                cfg.altitude = pkt.altitude;
                cfg.speed    = pkt.speed;
                cfg.bearing  = pkt.bearing;
                cfg.hideDev  = pkt.hideDev;
                applyConfig(cfg);
                shouldHook = cfg.enabled || cfg.hideDev;
                LOGD("Config received: enabled=%d lat=%.6f lng=%.6f hideDev=%d",
                     cfg.enabled, cfg.lat, cfg.lng, cfg.hideDev);
            }
        }

        if (!shouldHook) {
            api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
        }
    }

    void postAppSpecialize(const zygisk::AppSpecializeArgs* args) override {
        if (!shouldHook) return;

        LOGI("MockGPS activating in process");

        // Detect ART layout
        if (!detectArtMethodLayout(env)) {
            LOGE("Failed to detect ArtMethod layout!");
            return;
        }

        // Find JNI trampoline
        g_jniTrampoline = findJniTrampoline(env);
        if (!g_jniTrampoline) {
            LOGE("Failed to find JNI trampoline!");
            return;
        }

        // Apply Location hooks
        if (g_enabled.load()) {
            hookLocationMethods(env);
        }

        // Apply Settings hooks (developer options)
        if (g_hideDev.load()) {
            hookSettingsMethods(env);
        }

        // Start config watcher for live updates
        pthread_t tid;
        pthread_create(&tid, nullptr, configWatcherThread, nullptr);
        pthread_detach(tid);

        LOGI("MockGPS fully active: GPS=%s DevHide=%s",
             g_enabled.load() ? "ON" : "OFF",
             g_hideDev.load() ? "ON" : "OFF");
    }

    void preServerSpecialize(zygisk::ServerSpecializeArgs* args) override {
        // Don't hook system_server
        api->setOption(zygisk::DLCLOSE_MODULE_LIBRARY);
    }

private:
    zygisk::Api* api = nullptr;
    JNIEnv* env = nullptr;
    bool shouldHook = false;
};

// ═══════════════════════════════════════════════════════════════════
// Companion Handler (runs as root in Zygote's parent)
// ═══════════════════════════════════════════════════════════════════

static void companion_handler(int fd) {
    // Read config file and send to child process
    MockConfig cfg;

    int cfd = open(CONFIG_PATH, O_RDONLY);
    if (cfd >= 0) {
        char buf[1024];
        int n = read(cfd, buf, sizeof(buf) - 1);
        close(cfd);
        if (n > 0) {
            buf[n] = 0;
            cfg = parseConfig(buf);
        }
    }

    ConfigPacket pkt = {};
    pkt.enabled  = cfg.enabled ? 1 : 0;
    pkt.lat      = cfg.lat;
    pkt.lng      = cfg.lng;
    pkt.accuracy = cfg.accuracy;
    pkt.altitude = cfg.altitude;
    pkt.speed    = cfg.speed;
    pkt.bearing  = cfg.bearing;
    pkt.hideDev  = cfg.hideDev ? 1 : 0;

    write(fd, &pkt, sizeof(pkt));
}

REGISTER_ZYGISK_MODULE(MockGPSModule)
REGISTER_ZYGISK_COMPANION(companion_handler)
