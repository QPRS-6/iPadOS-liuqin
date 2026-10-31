#!/bin/bash
# Build libliuqintouch.dylib: the library injected into backboardd that turns
# /dev/liuqin-touch samples into HID digitizer events (host/liuqin_touch_inject.c).
#
#   tools/build-inject.sh [OUTDIR]
#
# Needs Xcode's iPhoneOS SDK.  The HID event-system symbols this uses are private
# to IOKit (no header, no .tbd in the SDK), so the library looks them up with
# dlsym() at run time - linking against a stub .tbd instead would record it as a
# load-time dependency and the process then dies with "Library not loaded".
set -euo pipefail
cd "$(dirname "$0")/.."
out=${1:-shim/out}
mkdir -p "$out"


SDK=$(xcrun --sdk iphoneos --show-sdk-path)
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -mios-version-min=16.0 \
    -isysroot "$SDK" -O2 -fPIC -dynamiclib \
    -install_name /usr/lib/libliuqintouch.dylib \
    -framework CoreFoundation \
    -o "$out/libliuqintouch.dylib" host/liuqin_touch_inject.c

file "$out/libliuqintouch.dylib"
shasum -a 256 "$out/libliuqintouch.dylib"

# The launcher carries the library inside it.
python3 host/liuqin_dylib_blob.py "$out/libliuqintouch.dylib" host/liuqin_dylib_blob.h

# The launcher that starts backboardd with the library inserted (launchd ignores
# a job's EnvironmentVariables on iOS).
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -mios-version-min=16.0 \
    -isysroot "$SDK" -O2 -fPIC \
    -o "$out/liuqin-backboardd" host/liuqin_backboardd_launcher.c
codesign --force --sign - "$out/liuqin-backboardd" 2>/dev/null || true
file "$out/liuqin-backboardd"
shasum -a 256 "$out/liuqin-backboardd"

# The probe that answers "can launchd run a binary of ours at all?".
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -mios-version-min=16.0 \
    -isysroot "$SDK" -O2 -fPIC \
    -o "$out/liuqin-probe" host/liuqin_probe.c
codesign --force --sign - "$out/liuqin-probe" 2>/dev/null || true
file "$out/liuqin-probe"
shasum -a 256 "$out/liuqin-probe"

# The launcher probe: says it ran, then exits (no exec yet).
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -mios-version-min=16.0 \
    -isysroot "$SDK" -O2 -fPIC \
    -o "$out/liuqin-launcher-test" host/liuqin_launcher_test.c
codesign --force --sign - "$out/liuqin-launcher-test" 2>/dev/null || true
file "$out/liuqin-launcher-test"
shasum -a 256 "$out/liuqin-launcher-test"

# The kick job: asks launchd about com.apple.backboardd and starts it.
xcrun --sdk iphoneos clang -target arm64-apple-ios16.0 -mios-version-min=16.0 \
    -isysroot "$SDK" -O2 -fPIC \
    -o "$out/liuqin-kick" host/liuqin_kick.c
codesign --force --sign - "$out/liuqin-kick" 2>/dev/null || true
shasum -a 256 "$out/liuqin-kick"
