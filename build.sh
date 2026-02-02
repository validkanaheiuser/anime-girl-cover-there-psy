#!/bin/bash
# MockGPS v2.0 Build Script
# Builds native .so with NDK and packages Magisk module zip
#
# Usage:
#   ./build.sh          # Build native + package module
#   ./build.sh app      # Build companion APK (requires Gradle/Android Studio)

set -e

SCRIPT_DIR="$(cd "$(dirname "$0")" && pwd)"
BUILD_DIR="$SCRIPT_DIR/build"
MODULE_DIR="$SCRIPT_DIR/module"

# Find NDK
NDK="${ANDROID_NDK_HOME:-${NDK_HOME:-${ANDROID_HOME}/ndk-bundle}}"
if [ ! -d "$NDK" ]; then
    echo "ERROR: Android NDK not found."
    echo "Set ANDROID_NDK_HOME, NDK_HOME, or ANDROID_HOME/ndk-bundle"
    exit 1
fi
echo "NDK: $NDK"

# Build native .so
echo "=== Building native module ==="
mkdir -p "$BUILD_DIR"
cd "$SCRIPT_DIR"
"$NDK/ndk-build" \
    NDK_PROJECT_PATH="$SCRIPT_DIR" \
    NDK_LIBS_OUT="$BUILD_DIR/libs" \
    APP_BUILD_SCRIPT="$SCRIPT_DIR/jni/Android.mk" \
    -j$(nproc)

# Copy .so to module
echo "=== Packaging Magisk module ==="
mkdir -p "$MODULE_DIR/zygisk"

for abi in arm64-v8a armeabi-v7a; do
    if [ -f "$BUILD_DIR/libs/$abi/libmockgps.so" ]; then
        cp "$BUILD_DIR/libs/$abi/libmockgps.so" "$MODULE_DIR/zygisk/$abi.so"
        echo "  ✓ $abi"
    fi
done

# Create flashable zip
cd "$MODULE_DIR"
ZIP_NAME="MockGPS-v2.0.0.zip"
rm -f "$BUILD_DIR/$ZIP_NAME"
zip -r "$BUILD_DIR/$ZIP_NAME" . -x "*.DS_Store"

echo ""
echo "=== BUILD COMPLETE ==="
echo "  Module:  $BUILD_DIR/$ZIP_NAME"
echo "  Flash via Magisk Manager"
echo ""

# Optional APK build
if [ "$1" = "app" ]; then
    echo "=== Building companion APK ==="
    cd "$SCRIPT_DIR/app"
    if [ -f "./gradlew" ]; then
        ./gradlew assembleRelease
    elif command -v gradle &>/dev/null; then
        gradle assembleRelease
    else
        echo "Open app/ folder in Android Studio to build APK"
    fi
fi
