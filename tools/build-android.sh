#!/usr/bin/env bash
# Builds the port for Android arm64 (NDK 27, CMake 3.31). Usage: tools/build-android.sh [snap] [target...]
# The game: XPORT_GAME, a folder under games/ (default gta4), built in build/arm64-<game>.
# snap: the CPU tuned for the Snapdragon 680 (XPORT_SNAPDRAGON) in its own tree,
# build/arm64-snap-<game>, for tools/build-apk.sh snap.
set -euo pipefail
unset MSYS_NO_PATHCONV  # CMake needs Git Bash path conversion
ROOT="$(cd "$(dirname "$0")/.." && pwd)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
NDK="$SDK/ndk/27.2.12479018"
CM="$SDK/cmake/3.31.6/bin"
GAME="${XPORT_GAME:-gta4}"
BUILD="$ROOT/build/arm64-$GAME"
SNAPDRAGON=OFF
if [ "${1:-}" = snap ]; then
  shift
  BUILD="$ROOT/build/arm64-snap-$GAME"
  SNAPDRAGON=ON
fi
# API 29 (Android 10) is the first with ELF TLS: below it the NDK emulates thread_local with a
# call to __emutls_get_address on every access (g_ppcContext in every guest lock, the renderer's
# per-thread state), 1% of the recorder alone (simpleperf, 2026-10-26).
PLATFORM=android-29

# A build tree configured for another API level is configured again from scratch (the compiler
# target is fixed at the first configure); every object is rebuilt.
FRESH=()
if [ -f "$BUILD/CMakeCache.txt" ] && ! grep -q "^ANDROID_PLATFORM:.*=$PLATFORM\$" "$BUILD/CMakeCache.txt"; then
  echo "build-android: $BUILD was configured for another API level; configuring for $PLATFORM"
  FRESH=(--fresh)
fi
if [ ! -f "$BUILD/build.ninja" ] || [ ${#FRESH[@]} -ne 0 ]; then
  "$CM/cmake" "${FRESH[@]}" -S "$ROOT" -B "$BUILD" -G Ninja -DCMAKE_MAKE_PROGRAM="$CM/ninja" \
    -DCMAKE_TOOLCHAIN_FILE="$NDK/build/cmake/android.toolchain.cmake" \
    -DANDROID_ABI=arm64-v8a -DANDROID_PLATFORM=$PLATFORM -DANDROID_STL=c++_static \
    -DCMAKE_BUILD_TYPE=Release -DXPORT_SNAPDRAGON=$SNAPDRAGON -DXPORT_GAME=$GAME
fi
# Stubs for the imports no source implements (rewritten only when that set changes).
(cd "$ROOT" && py -3 tools/gen_stubs.py)
"$CM/cmake" --build "$BUILD" -j "${JOBS:-6}" ${1:+--target "$@"}
