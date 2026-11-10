#!/usr/bin/env bash
# Builds tools/maliprof for arm64 Android and pushes it to /data/local/tmp/maliprof.
# Run while the game is in-game: adb shell /data/local/tmp/maliprof 10 1000
set -euo pipefail
unset MSYS_NO_PATHCONV
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK="$SDK/ndk/27.2.12479018"
CM="$SDK/cmake/3.31.6/bin"
BUILD="$ROOT/build/maliprof"
if [ ! -f "$BUILD/build.ninja" ]; then
  "$CM/cmake" -S "$ROOT/tools/maliprof" -B "$BUILD" -G Ninja -DCMAKE_MAKE_PROGRAM="$CM/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=android-26 -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release
fi
"$CM/cmake" --build "$BUILD"
MSYS_NO_PATHCONV=1 adb push "$(cygpath -w "$BUILD/maliprof")" /data/local/tmp/maliprof
MSYS_NO_PATHCONV=1 adb shell chmod 755 /data/local/tmp/maliprof
