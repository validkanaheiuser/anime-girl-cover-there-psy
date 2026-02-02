# MockGPS v2.0 — Zygisk GPS Spoofing Module

GPS location spoofing + developer options hiding via ART method hooks.  
No external mock GPS app needed. Applies to **all apps** automatically.

## Features

- **GPS Spoofing** — All apps see your chosen coordinates (lat, lng, altitude, accuracy, speed, bearing)
- **Mock Detection Bypass** — `isFromMockProvider()` and `isMock()` always return false  
- **Developer Options Hiding** — `Settings.Secure/Global.getInt()` returns 0 for `mock_location`, `development_settings_enabled`, `adb_enabled`
- **Fresh Timestamps** — `getTime()` and `getElapsedRealtimeNanos()` return current time so location never appears stale
- **Live Toggle** — Enable/disable without reboot (3-second config polling)
- **Map UI** — Companion app with interactive map, address search, and settings

## Architecture

```
┌──────────────┐   writes (root)   ┌──────────────────┐
│  MockGPS     │ ─────────────────→ │  location.conf   │
│  App (UI)    │                    │  (text config)   │
└──────────────┘                    └──────────────────┘
                                            ↓ read
                                    ┌──────────────────┐
                                    │ Zygisk Companion  │
                                    │  (root daemon)    │
                                    └──────────────────┘
                                            ↓ socket
                                    ┌──────────────────┐
                                    │  App Processes    │
                                    │  (ART hooks)      │
                                    └──────────────────┘
```

## How It Works (Technical)

The module converts Java methods to native methods at the ART runtime level:

1. **ArtMethod Layout Detection** — Auto-detects 32-byte vs 40-byte ArtMethod struct size by examining known native methods (String.intern, Thread.isInterrupted)

2. **JNI Trampoline Discovery** — 4-strategy search to find the regular (non-@CriticalNative) JNI trampoline:
   - `dlsym(RTLD_DEFAULT, "art_quick_generic_jni_trampoline")`
   - `/proc/self/maps` → find libart.so → `dlopen` + `dlsym`
   - Instance native method probes (String.intern, Thread.isInterrupted, etc.)
   - Fallback to System.nanoTime (last resort)

3. **Method Conversion** — For each target method:
   - Set `kAccNative` flag in access_flags
   - Clear ambiguous bits: `kAccFastInterpreterToInterpreterInvoke`, `kAccSingleImplementation`→`kAccFastNative`, `kAccCriticalNative`
   - Write native function pointer to `data_` field
   - Write JNI trampoline to `entry_point_` field

4. **Fallback Reads** — When spoofing disabled, hooks read actual values from Location object fields (`mLatitude`, `mLongitude`, etc.)

## Install

1. **Build native module:**
   ```bash
   export ANDROID_NDK_HOME=/path/to/ndk
   chmod +x build.sh
   ./build.sh
   ```

2. **Flash** `build/MockGPS-v2.0.0.zip` in Magisk Manager

3. **Build companion app** in Android Studio (open `app/` folder), or:
   ```bash
   ./build.sh app
   ```

4. **Install APK**, open app, pick location on map, tap "Set Location"

5. **Reboot** (first time only)

## Config File

`/data/adb/modules/mockgps/location.conf`:
```
enabled=1
lat=10.776900
lng=106.700900
accuracy=3.0
altitude=10
speed=0
bearing=0
hidedev=1
```

The companion app writes this file via root. The Zygisk companion daemon reads it and sends to child processes. A background thread in each process polls for changes every 3 seconds.

## Hooked Methods

| Method | When Enabled | When Disabled |
|--------|-------------|---------------|
| `isFromMockProvider()` | `false` | `false` |
| `isMock()` | `false` | `false` |
| `getLatitude()` | Config value | Field `mLatitude` |
| `getLongitude()` | Config value | Field `mLongitude` |
| `getAccuracy()` | Config value | Field `mAccuracy` |
| `getAltitude()` | Config value | Field `mAltitude` |
| `getSpeed()` | Config value | Field `mSpeed` |
| `getBearing()` | Config value | Field `mBearing` |
| `getTime()` | Current time | Field `mTime` |
| `getElapsedRealtimeNanos()` | Current boottime | Field value |
| `Settings.Secure.getInt()` | 0 for mock/dev keys | Default |
| `Settings.Global.getInt()` | 0 for dev keys | Default |

## Requirements

- Magisk 24+ with Zygisk enabled (or KernelSU + ZygiskNext)
- Android 8+ (API 26+)
- Root access for companion app
