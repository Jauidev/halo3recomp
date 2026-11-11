#!/usr/bin/env bash
# Builds build/apk-<game>/xport-<game>.apk without Gradle: javac -> d8 -> aapt2 link -> add classes.dex
# and lib/arm64-v8a/libgame.so -> zipalign -> apksigner (Android debug keystore).
# The game: XPORT_GAME (default gta4). Run XPORT_GAME=<game> tools/build-android.sh xportapp first.
# Each game is its own app: package com.xport.<game>, its name XPORT_APP_LABEL in games/<game>/game.cmake.
# Install: adb install -r build/apk-<game>/xport-<game>.apk
# Usage: tools/build-apk.sh [snap]
#   (none)  build/arm64-<game>: the one APK for every phone. Its launcher tells Mali from Adreno; on
#           Adreno, Mesa's Turnip (third_party/turnip, as libvulkan_turnip.so) and libadrenotools'
#           hooks, which runtime/os/vk_driver.cpp loads, run the renderer when the phone's driver cannot.
#   snap    build/arm64-snap-<game> (tools/build-android.sh snap xportapp): the CPU tuned for the
#           Snapdragon 680, for comparisons. Same package and data folder.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/.." && pwd -W)"
SDK="${ANDROID_HOME:-$LOCALAPPDATA/Android/Sdk}"
BT="$SDK/build-tools/36.0.0"
JAR="$SDK/platforms/android-34/android.jar"
JDK="${JAVA_HOME:-C:/Program Files/Android/Android Studio/jbr}"
STRIP="$SDK/ndk/27.2.12479018/toolchains/llvm/prebuilt/windows-x86_64/bin/llvm-strip.exe"
VARIANT="${1:-port}"
GAME="${XPORT_GAME:-gta4}"
MANIFEST="$ROOT/android/AndroidManifest.xml"
case "$VARIANT" in
  port) NAME=xport-$GAME; OUT="$ROOT/build/apk-$GAME"; BUILD="$ROOT/build/arm64-$GAME" ;;
  snap) NAME=xport-$GAME-snap; OUT="$ROOT/build/apk-snap-$GAME"; BUILD="$ROOT/build/arm64-snap-$GAME" ;;
  *) echo "usage: tools/build-apk.sh [snap]" >&2; exit 1 ;;
esac
# The app's name on the phone: XPORT_APP_LABEL in the game's game.cmake, else the folder name.
LABEL=$(sed -n 's/^set(XPORT_APP_LABEL "\(.*\)")/\1/p' "$ROOT/games/$GAME/game.cmake")
LABEL="${LABEL:-$GAME}"
TURNIP="$ROOT/third_party/turnip/v27.0.0-R2/libvulkan_freedreno.so"
# The other drivers go in the APK as their AdrenoTools .zip
# (assets/drivers/<id>.zip); MainActivity unpacks them into files/drivers/<id>/, the turnip-* ones
# on an Adreno phone and the panvk-* ones (Mesa PanVK on Mali's kbase kernel driver, 2026-11-11)
# on a Mali one, where the launcher lists them beside the imported ones.
BUNDLED_DRIVERS="turnip-gen8-v39=$ROOT/third_party/turnip/gen8-v39/Turnip_Gen8_V39_Patched.zip;turnip-v26.0.0-r8=$ROOT/third_party/turnip/v26.0.0-R8/Turnip_v26.0.0_R8.zip;turnip-v27.0.0-r2-oneui=$ROOT/third_party/turnip/v27.0.0-R2-oneui/Turnip_v27.0.0-R2-SAMSUNG.zip"
BUNDLED_DRIVERS="$BUNDLED_DRIVERS;panvk-fristonerr-v3.7.0=$ROOT/third_party/panvk/fristonerr-v3.7.0/PanVK-FristOneRR-V.3.7.0.zip"
BUNDLED_DRIVERS="$BUNDLED_DRIVERS;panvk-g925-v13-0.0.1=$ROOT/third_party/panvk/g925-v13-csf-0.0.1/PanVK-Kbase-Android-g925-v13-csf-v0.0.1-bc-mip-priority-20261029-e2d83eff.adpkg.zip"
BUNDLED_DRIVERS="$BUNDLED_DRIVERS;panvk-strato-safety-v4=$ROOT/third_party/panvk/strato-safety-v4/PanVK-Strato-Mali-SAFETY-v4.zip"
for item in ${BUNDLED_DRIVERS//;/ }; do
    [ -f "${item#*=}" ] || { echo "missing bundled driver: ${item#*=}" >&2; exit 1; }
done
HOOKS="$BUILD/third_party/libadrenotools/src/hook"

rm -rf "$OUT"
mkdir -p "$OUT/classes" "$OUT/dex" "$OUT/lib/arm64-v8a"
# The manifest with the game's name (the package is renamed at link time).
sed "s|android:label=\"[^\"]*\"|android:label=\"$LABEL\"|" "$MANIFEST" > "$OUT/AndroidManifest.xml"
# The game's own version name: XPORT_APP_VERSION in its game.cmake, else the manifest's. Only the name:
# the versionCode stays, as a lower one would not install over the app without uninstalling it, which
# deletes its data (saves, caches).
APP_VERSION=$(sed -n 's/^set(XPORT_APP_VERSION "\(.*\)")/\1/p' "$ROOT/games/$GAME/game.cmake")
if [ -n "$APP_VERSION" ]; then
  sed -i "s|android:versionName=\"[^\"]*\"|android:versionName=\"$APP_VERSION\"|" "$OUT/AndroidManifest.xml"
fi
# Gears of War 3: not debuggable, so ART compiles the Java instead of interpreting it with CheckJNI
# (touch pad and input took ~5% of a core on the main thread, 2026-11-03), but profileable from the
# shell so simpleperf still records it. XPORT_DEBUGGABLE=1 (or VALIDATION=1 / GFXR=1, whose layers
# need it) keeps it debuggable.
if { [ "$GAME" = gears_of_war_3 ] || [ "$GAME" = halo3 ]; } && [ "${XPORT_DEBUGGABLE:-0}" != 1 ] && [ "${VALIDATION:-0}" != 1 ] &&
   [ "${GFXR:-0}" != 1 ]; then
  sed -i 's|android:debuggable="true"|android:debuggable="false"|; s|^\( *\)<activity$|\1<profileable android:shell="true" />\n\1<activity|' \
      "$OUT/AndroidManifest.xml"
  grep -q '<profileable android:shell="true" />' "$OUT/AndroidManifest.xml" || { echo "manifest: profileable not added" >&2; exit 1; }
fi

"$JDK/bin/javac" -source 8 -target 8 -bootclasspath "$JAR" -nowarn -Xlint:-options \
    -d "$OUT/classes" $(find "$ROOT/android/src" -name '*.java')
"$BT/d8.bat" --release --min-api 29 --lib "$JAR" --output "$OUT/dex" $(find "$OUT/classes" -name '*.class')
# The app icon (made by tools/make_icon.py): the game's own (games/<game>/LAUNCHER/res), else LAUNCHER/res.
ICON_RES="$ROOT/games/$GAME/LAUNCHER/res"
[ -d "$ICON_RES" ] || ICON_RES="$ROOT/LAUNCHER/res"
"$BT/aapt2.exe" compile --dir "$ICON_RES" -o "$OUT/res.zip"
"$BT/aapt2.exe" link -o "$OUT/base.apk" --manifest "$OUT/AndroidManifest.xml" -I "$JAR" "$OUT/res.zip" \
    --min-sdk-version 29 --target-sdk-version 34 --rename-manifest-package "com.xport.$GAME"

# The APK carries a stripped copy; $BUILD/libgame.so keeps the symbols for llvm-symbolizer
# (crash offsets are the same in both).
"$STRIP" --strip-debug -o "$OUT/lib/arm64-v8a/libgame.so" "$BUILD/libgame.so"

# Debug Vulkan layers (the app is debuggable, so the Vulkan loader finds them in the app's library
# directory). VALIDATION=1: Khronos validation, enabled with XPORT_XENIA_CVARS=vulkan_validation=true.
# GFXR=1: LunarG GFXReconstruct, enabled with adb shell setprop debug.vulkan.layers
# VK_LAYER_LUNARG_gfxreconstruct (captures are configured with the debug.gfxrecon.* properties).
LAYERS=()
if [ "${VALIDATION:-0}" = 1 ]; then
  LAYERS+=("$ROOT/third_party/vulkan-validation/arm64-v8a/libVkLayer_khronos_validation.so")
fi
if [ "${GFXR:-0}" = 1 ]; then
  LAYERS+=("$ROOT/third_party/gfxreconstruct/arm64-v8a/libVkLayer_gfxreconstruct.so")
fi
# Turnip and adrenotools' hooks go in the same folder (the package manager extracts it to
# nativeLibraryDir, where adrenotools looks for both); only Adreno phones load them.
cp "$TURNIP" "$OUT/lib/arm64-v8a/libvulkan_turnip.so"
LAYERS+=("$OUT/lib/arm64-v8a/libvulkan_turnip.so")
for hook in hook_impl main_hook file_redirect_hook gsl_alloc_hook; do
  "$STRIP" --strip-debug -o "$OUT/lib/arm64-v8a/lib$hook.so" "$HOOKS/lib$hook.so"
  LAYERS+=("$OUT/lib/arm64-v8a/lib$hook.so")
done
# DXC for the runtime shader translator (tools/build-dxc-android.bat). Without it the app still
# runs, drawing only the shaders in the cache.
DXC="$ROOT/build/dxc/libdxcompiler.so"
if [ -f "$DXC" ]; then
  LAYERS+=("$DXC")
else
  echo "warning: no $DXC (tools/build-dxc-android.bat): shaders missing from the cache will not be drawn" >&2
fi

# The launcher's backgrounds (LAUNCHER/: Mali blue, Adreno red) and its font
# (runtime/os/launcher_ui.cpp reads them with the AssetManager).
LAUNCHER="$ROOT/LAUNCHER"
UI_FONT="$ROOT/third_party/xenia-gpu/third_party/imgui/misc/fonts/Roboto-Medium.ttf"
# A game's own background (games/<game>/LAUNCHER/FONDO.png) replaces both GPU ones in its APK.
GAME_BG="$ROOT/games/$GAME/LAUNCHER/FONDO.png"
[ -f "$GAME_BG" ] || GAME_BG=""

# assets/build.txt, for the launcher's patches (android/src/com/xport/Patches.java, tools/make-patch.py):
# this build's number (the date and time it was made: a patch's library must be newer to load), the
# Java side's ABI (a patch's library loads only with the same), the game and its version name.
PATCH_ABI=$(sed -n 's/.*static final int ABI = \([0-9][0-9]*\);.*/\1/p' "$ROOT/android/src/com/xport/Patches.java")
[ -n "$PATCH_ABI" ] || { echo "Patches.java: no ABI" >&2; exit 1; }
BUILD_INFO="build=$(date +%Y%m%d%H%M%S)
abi=$PATCH_ABI
game=$GAME
version=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$OUT/AndroidManifest.xml")
"
echo "build.txt: $(echo "$BUILD_INFO" | tr '\n' ' ')"

# The libraries keep the source paths of __FILE__ (asserts, logs): the folder the project was built in
# (and its parent) becomes a relative path of the same length ("././.../third_party/..."), so an APK
# does not carry the builder's folders.
BUILD_INFO="$BUILD_INFO" BUNDLED_DRIVERS="$BUNDLED_DRIVERS" py -3 - "$ROOT" "$OUT" "$LAUNCHER" "$UI_FONT" "$GAME_BG" "${LAYERS[@]}" <<'EOF'
import sys, zipfile, os, re
root, out, launcher, ui_font, game_bg, layers = sys.argv[1], sys.argv[2], sys.argv[3], sys.argv[4], sys.argv[5], sys.argv[6:]


def path_pattern(path):
    parts = [re.escape(p.encode()) for p in re.split(r"[\\/]+", path) if p]
    return re.compile(rb"[\\/]+".join(parts), re.IGNORECASE)


folders = [p for p in (root, os.path.dirname(root.rstrip("/\\"))) if len(re.split(r"[\\/]+", p.strip("/\\"))) > 1]
patterns = [path_pattern(p) for p in folders]


def relative(match):
    n = len(match.group(0))
    return b"./" * (n // 2) + b"/" * (n % 2)


def library(path):
    data = open(path, "rb").read()
    for pattern in patterns:
        data = pattern.sub(relative, data)
    return data


with zipfile.ZipFile(os.path.join(out, "base.apk"), "a", zipfile.ZIP_DEFLATED) as apk:
    apk.write(os.path.join(out, "dex", "classes.dex"), "classes.dex")
    for path in [os.path.join(out, "lib", "arm64-v8a", "libgame.so"), *layers]:
        apk.writestr("lib/arm64-v8a/" + os.path.basename(path), library(path))
    mali = game_bg or os.path.join(launcher, "FONDO DE MALI.png")
    adreno = game_bg or os.path.join(launcher, "FONDO DE ADRENO.png")
    apk.write(mali, "assets/launcher/bg_mali.png", zipfile.ZIP_STORED)
    apk.write(adreno, "assets/launcher/bg_adreno.png", zipfile.ZIP_STORED)
    apk.write(ui_font, "assets/launcher/Roboto-Medium.ttf")
    for item in os.environ.get("BUNDLED_DRIVERS", "").split(";"):
        name, _, path = item.partition("=")
        if name and os.path.isfile(path):
            apk.write(path, "assets/drivers/" + name + ".zip", zipfile.ZIP_STORED)
    apk.writestr("assets/build.txt", os.environ["BUILD_INFO"])
EOF

"$BT/zipalign.exe" -f -p 4 "$OUT/base.apk" "$OUT/aligned.apk"
"$BT/apksigner.bat" sign --ks "$USERPROFILE/.android/debug.keystore" --ks-pass pass:android \
    --key-pass pass:android --out "$OUT/$NAME.apk" "$OUT/aligned.apk"
ls -la "$OUT/$NAME.apk"

# Keep a copy named by its version (tools/bump-version.sh sets it before a build with changes).
VERSION=$(sed -n 's/.*android:versionName="\([^"]*\)".*/\1/p' "$OUT/AndroidManifest.xml")
cp "$OUT/$NAME.apk" "$ROOT/build/$NAME-$VERSION.apk"
echo "version $VERSION -> build/$NAME-$VERSION.apk"
