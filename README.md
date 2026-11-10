# halo3recomp

Port no oficial de **Halo 3** (Xbox 360) a **Android** por recompilación estática: el código PowerPC del juego
se traduce a C++ y se compila para ARM64, con un renderer Vulkan nativo. No es un emulador. Port de
**Jauidev**, hecho con la herramienta de este repositorio que lleva un juego de Xbox 360 del disco a un APK.
Sigue a [Gears3Recomp](https://github.com/Jauidev/Gears3Recomp), el port de Gears of War 3.

> **No incluye el juego ni nada que salga de él** (ni el código recompilado, ni sus shaders, ni sus
> bibliotecas, ni ningún dato del disco): todo eso se genera desde tu propia copia del disco de Halo 3 para
> Xbox 360. Tampoco se distribuye aquí ninguna APK con el juego dentro.
> Halo y sus marcas pertenecen a Microsoft. Este proyecto no está afiliado a Microsoft, Bungie ni
> Halo Studios (343 Industries), ni cuenta con su aprobación. Necesitas una copia legítima del juego.

## Estado

- Se juega a **30 fps** (el límite del propio juego) en una GPU Mali-G615.
- El agua teselada y la vegetación instanciada tienen un camino propio en el renderer: los datos de cada
  parche se calculan una vez en la CPU y los shaders de vértices se compilan para el formato exacto de cada
  dibujo, sin cambiar la imagen.
- Los shaders que no están en la caché se traducen en segundo plano, para que la partida no se congele, y
  las pipelines ya creadas se guardan en disco para la siguiente sesión.
- Controles táctiles y mandos físicos por Bluetooth o USB (Xbox, PlayStation y la mayoría de mandos
  Android), a la vez.
- Launcher propio, con su fondo y sus colores.
- Probado solo en parte de la campaña y solo en una GPU Mali.

## Instalar en Android

Este repositorio no publica APK: se compila con tu propio disco (ver [Compilar](#compilar)). Con la APK ya
hecha (arm64, Android 10 o superior, Vulkan):

1. Instálala y ábrela.
2. En el launcher elige la ISO del juego o la carpeta del disco extraído. También puedes copiar el disco
   extraído a `/sdcard/Android/data/com.xport.halo3/files/game`.

En Adreno, el launcher puede usar Mesa Turnip si el driver del sistema no sirve.

## Compilar

Requisitos (Windows): Android NDK 27.2, Android SDK (build-tools 36, CMake 3.31), MSVC Build Tools 2022,
Python 3 y Git Bash. Todo el código de terceros va incluido en `third_party/`.

Del disco al APK, con todos los pasos (recompilar, ranura, shaders, prueba de arranque, APK):

    py -3 -I tools/port.py <juego.iso|carpeta del disco> <carpeta de trabajo> halo3 --label "Halo 3"

Ese paso es el que crea lo que aquí falta a propósito: `games/halo3/ppc/` (el código recompilado),
`games/halo3/dlls/` (las bibliotecas del juego recompiladas) y `games/halo3/data/shader/` (la caché de
shaders).

Con la ranura `games/halo3` ya preparada:

    XPORT_GAME=halo3 bash tools/build-android.sh xportapp
    XPORT_GAME=halo3 bash tools/build-apk.sh

Los shaders que no están en la caché se traducen en tiempo real; en Android hace falta `libdxcompiler.so`
(`tools\build-dxc-android.bat`).

La guía completa está en [docs/GUIA.md](docs/GUIA.md) y la descripción de las herramientas en
[docs/XPORT.md](docs/XPORT.md).

## Estructura

| Carpeta | Qué hay |
| --- | --- |
| `runtime/` | Runtime: kernel, CPU, audio, launcher |
| `rexglue_glue/`, `xenia_glue/` | Hooks de Direct3D y puente con el renderer |
| `third_party/rexglue/` | ReXGlue SDK y el renderer nativo xnative (de Liberty Recompiled) |
| `games/halo3/` | La ranura del juego: configuración, arreglos, funciones de D3D y el fondo e icono del launcher |
| `tools/` | XPort: del disco al APK (`port.py`) y utilidades; `halo3-*.ps1` miden el rendimiento en el móvil |
| `android/`, `LAUNCHER/` | La app de Android y los recursos del launcher |

Los scripts `tools/halo3-*.ps1` usan el móvil conectado por adb: el número de serie se toma de la variable
`ANDROID_SERIAL` o del parámetro `-Device`, y dejan sus resultados en `work/logs/`.

## Licencia

[GPL-3.0](LICENSE): el renderer viene de Liberty Recompiled (GPL-3). Cada componente de `third_party/`
conserva su propia licencia. La licencia cubre el código de este repositorio, no el juego.

## Créditos

Port Android: **Jauidev**.

Liberty Recompiled (renderer nativo xnative), ReXGlue SDK, XenonRecomp, XenosRecomp y Unleashed Recompiled
(hedge-dev), Xenia Canary, DirectX Shader Compiler, glslang, smol-v, volk, SMAA, Mesa Turnip, libadrenotools,
libGPUCounters, FFmpeg, libmspack, Zstandard, Snappy, xxHash, unordered_dense, O(1) Heap, spdlog, fmt,
Dear ImGui, stb_image, Khronos Validation Layers y GFXReconstruct.
