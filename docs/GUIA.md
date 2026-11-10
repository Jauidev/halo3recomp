# Guía: portar un juego de Xbox 360 con Xcompiler

Esta guía va del disco del juego a un APK. Las herramientas hacen lo repetitivo; lo que queda a mano está marcado
**(a mano)**. Todo lo comprobado se comprobó solo con GTA IV (y EFLC en la recompilación).

## 0. Requisitos

- Windows, Python 3 (`py -3`), MSVC Build Tools 2022, Android SDK con NDK 27.2.12479018, CMake 3.31.6,
  build-tools 36.0.0 y la JBR de Android Studio (para el APK).
- El juego: ISO del disco, paquete de Xbox Live Arcade (CON/LIVE/PIRS), carpeta del disco extraído o `default.xex`.
  Los paquetes Games on Demand (SVOD) no se admiten todavía.

## 1. Todo de una vez: `tools/port.py`

    py -3 -I tools/port.py <juego> <carpeta de trabajo> <nombre> [--label "Nombre"] [--seconds 300]
                           [--gpu native|xenia] [--no-build] [--no-apk] [--compare] [--phone]

`<nombre>` es la ranura (`games/<nombre>`) y el paquete del APK (`com.xport.<nombre>`): minúsculas, cifras y `_`.
Siete pasos: recompilar, ranura, shaders del disco, compilar para el PC, un arranque de prueba (los shaders que
faltan se traducen mientras juega y se vuelcan), la caché de shaders para el APK con ese volcado, y el APK
(`build\apk-<nombre>\xport-<nombre>.apk`). `--no-apk` para tras el arranque; `--no-build` tras los shaders.
`--compare` arranca también con la GPU de Xenia y compara las imágenes (ver 2.6); `--phone` instala el APK en el
móvil conectado por adb y lo prueba allí (ver 2.7). Tras el arranque, las recetas
de pipelines que aprendió el PC van a `games/<nombre>/data/pipeline_recipes.bin`: el APK las lleva dentro y el
primer arranque del móvil compila esos pipelines antes de dibujar con ellos.

Deja en la carpeta de trabajo:

| Archivo | Qué es |
| --- | --- |
| `report.md` | el informe del port: empieza por **Qué hacer ahora** (lo que falta, lo que más bloquea primero, con el archivo donde mirar) y sigue con cada paso |
| `logs/*.log` | la salida entera de cada paso |
| `disc/` | el disco extraído (la carpeta del juego para arrancarlo) |
| `ppc/`, `game.toml` | el código recompilado y su configuración de XenonRecomp |
| `d3d.json`, `slot.json` | lo que encontró `find_d3d.py` y lo que escribió `slot.py` |
| `boot/` | la prueba de arranque: `boot.md`, `boot.json`, capturas, log, shaders volcados y traducidos |
| `boot-xenia/`, `compare.md`, `compare.html` | con `--compare`: el arranque con la GPU de Xenia y la comparación |
| `phone/` | con `--phone`: la prueba en el móvil, como `boot/` (`boot.md`, capturas, logcat, `crash.txt`) |

### La interfaz

    tools\gui\build.bat        → build\host\gui\xcompiler.exe   (acceso directo «Xcompiler» en el escritorio)

El icono es el logo, `tools/gui/xcompiler.png`; la cabecera dibuja su X, `xcompiler-mark.png` (`xcompiler.ico`,
`xcompiler.rc` y `build.bat` los meten en el exe).

Una ventana (Dear ImGui, con los colores del logo) que hace lo mismo que `port.py`. A la izquierda, el juego:
**Elegir archivo…** o **Elegir carpeta…**, o arrastrarlo a la ventana o al acceso directo (rellena la ranura, el
nombre y la carpeta de trabajo, `work\<ranura>`, y enseña el informe de un port anterior si lo hay). Debajo, las
opciones de `port.py` como interruptores (**Sin APK**, **Comparar con Xenia**, **Probar en el móvil**...),
**Portar** para lanzarlo y **Detener** para pararlo todo (Python, la compilación y el juego). A la derecha, los
siete pasos en una fila (hechos, en marcha, fallidos, saltados), y dos pestañas: **Registro**, en directo y con
colores (también en `<carpeta de trabajo>\gui.log`), e **Informe**, que muestra `report.md` al terminar con
«Qué hacer ahora» destacado. **Abrir** y **Abrir carpeta** abren los archivos para leerlos o pasárselos a Claude.

## 2. Los pasos, uno a uno

### 2.1 Recompilar: `tools/xport.py`

    py -3 -I tools/xport.py <juego> <carpeta> [--extract]

Saca el XEX, busca las funciones auxiliares de registros, el `setjmp`/`longjmp` del CRT y las tablas de saltos, y
repite XenonRecomp hasta que no queda nada que arreglar (switch partidos, saltos a otra función, punteros a
funciones). Su informe va a `recompile.md` dentro de `report.md`. Si quedan **instrucciones sin implementar** o
**saltos sin resolver**, el código no se puede enlazar todavía: es trabajo en XenonRecomp o en `game.toml`
**(a mano)**. Tras cambiar XenonRecomp, `tools\build-xenonrecomp.bat` lo compila y lo copia a `bin\`.

Un `blr`/`bctr` solo es incondicional si `(BO & 0x14) == 0x14`; el analizador trataba `bdzlr` como un retorno y
cortaba ahí la función (en Gears, el `memset` del CRT: los índices de los modelos con esqueleto salían basura y el
juego se caía a los pocos minutos de partida). Ya está arreglado en `XenonAnalyse/function.cpp`: tras un cambio así
hay que regenerar el juego desde cero con `xport.py` y `slot.py`.

### 2.2 La ranura del juego: `tools/slot.py`

    py -3 -I tools/slot.py <carpeta> <nombre> ["Nombre"]

Crea `games/<nombre>/`:

- `ppc/`: el código recompilado.
- `xd3d_device.h`: dónde guarda el device D3D de este juego cada campo que usan los hooks y el renderizador
  (`DeviceOffset(n)`, con `n` donde lo guarda GTA IV). `find_d3d.py` lo saca alineando las instrucciones de las
  mismas funciones D3D en los dos juegos; con el mismo XDK es la identidad. Los campos que no encuentra salen
  como `// DeviceOffset(n): not found` y el juego se compila sin el renderizador nativo: buscarlos a mano y
  apuntarlos en `xd3d_fixes.txt` **(a mano)**; los «guessed» son los primeros que revisar si la imagen sale mal.
- `xd3d_functions.h`: las funciones D3D del juego, encontradas por firma (`tools/find_d3d.py`, base de firmas
  `tools/signatures/xdk-2.0.6274.json`, sacada de GTA IV). Si el juego usa **otro XDK**, el informe lo dice: las
  firmas pueden fallar y los offsets de la estructura del device pueden ser otros. Una función sin
  firma exacta se busca **por parecido** (la secuencia de tipos de instrucción, sin registros ni offsets): sale
  en el informe como «encontrada por parecido» y en el `.h` con `// alike`. Probado con las firmas de GTA IV
  alteradas hasta un 30% (otro reparto de registros, instrucciones de más y de menos): 47-48 de 58 bien y
  ninguna mal; las que no encuentra (cortas o con gemelas casi iguales) quedan como no encontradas.
- `xd3d_fixes.txt` **(a mano, opcional)**: lo comprobado a mano, que gana a la búsqueda en cada port. Una línea
  por cosa, con el porqué detrás de `#`: `CreatePixelShader 82222710` (una función), `CreatePixelShader -` (no
  existe en este juego: su hook no se compila), `DeviceOffset(11812) 11536` (un campo del device),
  `DeviceOffset(16552) -` (este juego no lo tiene: el código que lo usa se salta). `slot.py` lo pasa a
  `find_d3d.py`, que lo marca como `// by hand` en los `.h`. El de Gears of War es el ejemplo.

Con **el XDK de GTA IV** (`kReferenceXdk = true` en `xd3d_device.h`) los hooks sustituyen a las funciones D3D del
juego y el renderizador nativo necesita todas. Con **otro XDK** los hooks ejecutan la función del juego y además
avisan al renderizador (modo observador): solo hacen falta las que le mandan algo (`XD3D_CORE` en
`cmake/rexglue.cmake`: draws, clears, resolves, Swap, shaders, texturas, buffers, render targets y locks); las
demás, si faltan o están inlined, se quedan sin hook. Una SetVertexDeclaration inlined se nota en el siguiente draw
(el renderizador lee el campo del device). Los draws con vértices del título (DrawPrimitiveUP) llegan desde
`BeginVertices`: el hook manda el draw con el estado de ese momento y el renderizador copia los vértices del ring
buffer al recibir la siguiente orden, porque `EndVertices` suele estar inlined (en UE3 son dos instrucciones:
`lwz` del +13076 y `stw` al +40 del device). UE3 dibuja así la interfaz y los quads de pantalla completa del
postproceso: sin ellos la imagen sale negra. Los indexados (DrawIndexedPrimitiveUP) llegan igual desde
`BeginIndexedVertices`, que no está en la base de firmas y va a mano en `xd3d_fixes.txt` (Gears `8222D4F8`:
dos asignaciones con `82220EB0`, guarda índices y vértices en +13080/+13084 como BeginVertices; el noveno
argumento, el puntero de los vértices, va en la pila en +84); el renderizador copia un vértice por índice. Lo
que en modo observador no llega al renderizador: las command lists. Para forzar la GPU
de Xenia en un juego: `set(XPORT_NATIVE_RENDERER OFF)` en su `game.cmake`.
Los shaders que el motor crea sin `CreateVertexShader`/`CreatePixelShader` (Unreal Engine 3 reserva el objeto y
el microcódigo y lo rellena él) también llegan: los vertex shaders por `InitVertexShader` (la función con la que
`CreateVertexShader` rellena el objeto; `find_d3d.py` la saca de ella, «called by CreateVertexShader») y los pixel
shaders al ponerlos (SetPixelShader), con la copia de la cabecera y el microcódigo que guarda el objeto.
UE3 además ata cada vertex shader a su declaración de antemano (`IDirect3DVertexShader9::Bind`) y dibuja con la
declaración a NULL; el renderizador usa entonces la del vertex shader puesto. Esa función no está en la base de
firmas: va a mano en `xd3d_fixes.txt` como `BindVertexShader` (en Gears, `82234D70`; se reconoce porque lee el
+0x1C de la declaración y la tabla de vfetch del shader en +896). La prueba de arranque deja un volcado de
los hilos del juego poco antes de acabar (`XPORT_DUMP_AFTER`, `*** thread dump` en `boot/stdout.log`).
- `runtime-imports.txt`: sus imports. `tools/gen_stubs.py` (lo llama la compilación) da a los que el runtime no
  tiene un stub que escribe `not implemented` la primera vez que se llama.
- `game.cmake` y `game.cpp`: título, XEX aceptado, nombre de la app y las respuestas por defecto a lo que el runtime
  pregunta (`runtime/game.h`). Solo se escriben si no existen: ahí va el trabajo a mano de cada juego.
- `data/`: caché de shaders vacía.

### 2.3 Shaders: `tools/shaders.py`

    py -3 -I tools/shaders.py build <nombre> <carpeta del disco> [<volcado>...]
    py -3 -I tools/shaders.py check <nombre> <volcado>

El renderizador nativo solo dibuja shaders que están en `games/<nombre>/data/shader/shader_cache.cpp`. XenosRecomp
(`tools/build-xenosrecomp.bat` lo compila la primera vez) busca contenedores de shader en todos los archivos. Los
juegos que guardan los shaders comprimidos en sus archivos no dan ninguno desde el disco: entonces salen del
**volcado** que hace la prueba de arranque (`XPORT_SHADER_DUMP`): en el PC el traductor (abajo) ya los dibuja, y
`port.py` rehace la caché con el volcado para el APK. En el log, `xnative: shader cache miss` es un shader que
falta. Si un juego registra un shader con el hash de otra compilación del mismo programa, el par va en
`games/<nombre>/data/shader/shader_aliases.inc` (GTA IV tiene 48; un juego nuevo, ninguno).

**Traductor en tiempo real.** Un shader que no está en la caché se traduce mientras el juego corre,
cuando el juego lo crea (`rex/graphics/xnative/shader_translator.h`): el mismo XenosRecomp y el mismo DXC
(1.8.2407) que hacen la caché, con los mismos argumentos, así que sale el mismo módulo que habría en la caché.
Cada traducción se guarda en disco (`xnative-shaders/` junto a la carpeta de partidas; en el móvil,
`/sdcard/Android/data/com.xport.<nombre>/files/xnative-shaders`) y la siguiente vez se carga de ahí. Traducir
cuesta un rato la primera vez (tirones en las cargas), así que la caché sigue siendo lo primero y el volcado
sigue sirviendo para rellenarla. En el log: `xnative: translated ... shader` por cada traducción. DXC viene en
`dxcompiler.dll` (junto al exe) y en el móvil en `libdxcompiler.so` (`tools\build-dxc-android.bat`; sin ella el
APK funciona, pero solo con la caché).

La interfaz con el renderizador es `tools/shaders/xenos_shader_common.h`. El XenosRecomp de aquí es el de
Unleashed, parcheado para el renderizador nativo (huecos de textura, atributos de vértice, alpha test con
función, módulo late-Z). Con el volcado de GTA IV da los mismos hashes, máscaras de texturas y constantes de
especialización que la caché original en 1250 de 1250 shaders. No genera: la máscara de cobertura
(alpha-to-mask, se dibuja con alpha test normal), el sesgo de LOD por textura ni un heap propio para texturas 3D
(las lee del de 2D array).

### 2.4 Compilar

    set XPORT_GAME=<nombre>
    tools\build-host.bat --target xport                 → build\x64-<nombre>\<nombre>.exe
    XPORT_GAME=<nombre> tools/build-android.sh xportapp  (Git Bash)
    XPORT_GAME=<nombre> tools/build-apk.sh               → build\apk-<nombre>\xport-<nombre>.apk

Errores típicos y qué significan:

- `use of undeclared identifier 'XD3D_...'`: una función D3D que los hooks usan y `find_d3d.py` no encontró.
  Buscarla a mano y apuntarla en `xd3d_fixes.txt` **(a mano)**.
- Símbolos sin resolver `__imp__...`: un import nuevo; se arregla volviendo a compilar (genera su stub).

### 2.5 Arrancar en el PC: `tools/boot_test.py`

    py -3 -I tools/boot_test.py build\x64-<nombre>\<nombre>.exe <carpeta del disco> <salida> [segundos] [--gpu xenia] [--dump-shaders]

Arranca el juego, pulsa START en las pantallas de título y lo vigila. `boot.md` dice cómo acabó:

- **sigue en marcha** y la imagen cambia: bien; mirar las capturas en `<salida>/cap`.
- **colgado**: ni log ni capturas nuevas en 90 s.
- **se cae**: excepción y el informe de `crash.txt` (función invitada `sub_...` y pila).
- Capturas negras o de un solo color: el juego corre pero no dibuja (shaders que faltan, D3D mal localizado).
- Imports llamados que solo son stub: candidatos a implementar en `runtime/kernel` **(a mano)**.
- **Dibujos que el renderizador rechaza** (`xnative-diag: draw... failure` y `pipeline reject` en `run/xnative.log`):
  no se ven y el juego sigue sin avisar. Lo primero que hay que mirar si falta algo en la imagen.

`boot.md` da también los FPS (mediana y peor tramo de la segunda mitad, sacados de los `swaps` del log) y los
shaders traducidos.

Para investigar la imagen (variables en la sección 3):

- `XNATIVE_SHADER_PATCH=<archivo>` cambia shaders sin recompilar: líneas `=== <hash>`, cada una seguida de HLSL que se
  añade al final de ese shader (`output.oC0 = float4(1, 0, 0, 0);`). Es la forma más rápida de saber si un dibujo llega
  a la pantalla y qué valores recibe (por ejemplo, franjas según la posición en pantalla). Si ni un color fijo se ve,
  el dibujo se rechaza o lo descartan los tests.
- `XNATIVE_NO_STENCIL=1` y `XNATIVE_NO_LIGHT_DEPTH=1` quitan el test de stencil o el de profundidad (este, en los
  dibujos que no escriben profundidad).
- `XNATIVE_DRAW_TRACE=<n>` escribe en `<salida>/run/draw_trace_<fotograma>.txt`, cada n fotogramas, todas las órdenes:
  shaders, destinos, estado y texturas (`tN=handle:fFORMATO:AxA:sSWIZZLE:gSIGNOS:eEXP:mMAGMINMIP`, filtros del fetch
  constant: 0 punto, 1 lineal, 2 mip base), el rango de profundidad
  del viewport (`zr=min/max`; UE3 lo invierte, `1/0`) y el bias (`bias=activo:constante:pendiente`). Debajo de cada
  dibujo, la línea `host:` dice lo que llegó a Vulkan: máscara y formato de color, profundidad activa, vista y
  superficie de profundidad, muestras, formato y tamaño. Dos dibujos con la misma superficie comparten profundidad.
- `XNATIVE_DRAW_TRACE_VS=<hash>,<hash>` añade en los dibujos de esos vertex shaders la declaración y los
  constantes (c0-c19 y c233-c238).
- `XNATIVE_SKIP_PS=<hash>,<hash>` deja fuera los dibujos de esos pixel shaders, para ver qué aporta cada pase.
- `XENOS_RECOMP_DUMP_HLSL=<carpeta>` hace que XenosRecomp escriba el HLSL de cada shader: `<hash>.hlsl`. Cada
  archivo lleva delante la cabecera entera, así que para buscar llamadas hay que buscar `= tfetch` y no el nombre
  de la función. La carpeta tiene que existir (si no, cada shader da `error: cannot write`).
- `XPORT_CRASH_WORDS=r19:0x100,[r27+0x260]+0x10:0x40` vuelca memoria del juego en `crash.txt`, por registro.

Con `--gpu xenia` usa la GPU emulada de Xenia, más lenta pero sin depender de las funciones D3D ni de la caché de
shaders: sirve para separar fallos del renderizador nativo de fallos del juego.

### 2.6 Comparar con la GPU de Xenia: `tools/compare_gpu.py`

    py -3 -I tools/compare_gpu.py build\x64-<nombre>\<nombre>.exe <carpeta del disco> <salida> [segundos]
    py -3 -I tools/compare_gpu.py --analyze <salida> [<arranque nativo> <arranque Xenia>]

Arranca el juego con el renderizador nativo y con la GPU de Xenia (la que emula el Xenos entero: EDRAM, resolves,
todos los formatos) y empareja las dos series de capturas por orden (no por tiempo: Xenia va más lenta).
`compare.md` dice desde qué captura el nativo dibuja otra cosa: ahí mirar sus render targets, resolves y shaders
en el log. `compare.html` pone las parejas lado a lado. `port.py --compare` lo hace con su arranque.

Las dos partidas nunca capturan los mismos instantes, así que solo cuenta lo grueso: el nativo negro o de un
color en 3 capturas seguidas donde Xenia dibuja algo, o un brillo muy distinto (capas o colores que faltan). Un
detalle mal dibujado no sale. Probado con GTA IV: 111 capturas nativas y 82 de Xenia, ninguna marcada; con 15
capturas nativas seguidas puestas en negro a propósito, marca exactamente esas 15 y la primera.

### 2.7 En el móvil

    tools\build-dxc-android.bat     → build\dxc\libdxcompiler.so (una vez; build-apk.sh la mete en el APK)
    adb install -r build\apk-<nombre>\xport-<nombre>.apk

La app pide la ISO o la carpeta del juego. Datos: `/sdcard/Android/data/com.xport.<nombre>/files/` (`game`,
`save`, `crash.txt`). Log: `adb logcat -s xport xnative`.

    py -3 -I tools/phone_test.py <nombre> <carpeta del disco> <salida> [segundos]

Lo mismo que `boot_test.py` pero en el móvil, sin tocarlo: instala el APK, copia el juego a `files/game` si el
móvil no lo tiene (solo la primera vez, es el disco entero), lo arranca sin el launcher y con START pulsado
(`--es env "XPORT_LAUNCHER=0 XPORT_AUTOPRESS=1"`), saca una captura de pantalla cada 10 s y al final baja el
logcat y `crash.txt`. `boot.md` dice cómo acabó, los FPS, el móvil y la temperatura de la batería antes y
después. Necesita un solo móvil conectado con la depuración USB activada.

Su sección **Rendimiento** dice qué limita los FPS, medido en la segunda mitad de la prueba:
- **CPU:** el % de núcleo que usa cada hilo del juego (leído de `/proc`) y en qué núcleo corría. Un hilo al 85% o
  más de su núcleo frena el juego.
- **GPU:** MediaTek no deja leer su carga por adb, así que se usa su frecuencia: si va a la máxima el 80% del
  tiempo o más, está al límite (en Adreno se lee `kgsl`).
- Si no está al límite ninguna de las dos, el juego va a su tope de FPS o espera sincronizaciones.

Para que vaya fluido sin tocar nada del juego: el hilo que presenta los fotogramas y el que manda el audio se
nombran solos (`XpRenderThread`, `XpSoundThread`) si el juego no los nombra, y con eso van a sus núcleos; el
primer arranque compila los pipelines de `pipeline_recipes.bin` (los que vio el PC) en vez de hacerlo en el
primer dibujo de cada uno. En el log: `xnative: N pipeline recipes from the port's seed`.

## 3. Variables de entorno

| Variable | Para qué |
| --- | --- |
| `XPORT_GAME` | qué `games/<nombre>` compilan los scripts (por defecto `gta4`) |
| `XPORT_GAME_DIR`, `XPORT_SAVE_DIR` | carpeta del juego y de partidas en el PC |
| `XPORT_GPU` | `native` (renderizador nativo) o `xenia` |
| `XPORT_SHADER_DUMP` | carpeta donde volcar cada shader que el juego registra |
| `XPORT_AUTOPRESS` | `1`: pulsa START en las pantallas de título |
| `XPORT_AUTOPRESS_START_UNTIL` | segundos en los que pulsa también START; después solo A (para que no abra la pausa en partida) |
| `XPORT_CRASH_WORDS` | memoria del juego que volcar en `crash.txt`, por registro (solo Windows) |
| `XNATIVE_DRAW_TRACE`, `XNATIVE_DRAW_TRACE_VS` | traza de órdenes cada n fotogramas; constantes de un vertex shader (2.5) |
| `XNATIVE_SKIP_PS` | pixel shaders cuyos dibujos se saltan (2.5) |
| `XNATIVE_SHADER_PATCH` | archivo con HLSL que se añade a los shaders indicados (2.5) |
| `XNATIVE_NO_STENCIL`, `XNATIVE_NO_LIGHT_DEPTH` | quitan esos tests, para ver si rechazan un dibujo (2.5) |
| `XENOS_RECOMP_DUMP_HLSL` | carpeta donde XenosRecomp deja el HLSL de cada shader |
| `XNATIVE_CAPTURE` | carpeta para capturas PNG del renderizador nativo |
| `XNATIVE_CVARS` | opciones del renderizador, `nombre=valor,...` |
| `XNATIVE_RUNTIME_SHADERS` | `0`: sin traductor de shaders (solo la caché) |
| `XNATIVE_SHADER_CACHE_DIR` | dónde guarda el traductor sus shaders (`boot_test.py` usa `<salida>/translated`) |

## 4. Lo que queda a mano en cada juego

1. Lo que XenonRecomp no resuelve (instrucciones nuevas, `setjmp`/`longjmp`, excepciones).
2. Funciones D3D no encontradas, sobre todo con otro XDK.
3. Imports que el juego necesita de verdad y solo son stub.
4. `game.cpp`: hilos que van a los núcleos grandes (el de render y el de audio ya se reconocen solos), entradas
   que el recompilador no vio, resolución, menú de pausa.
5. Fallos de imagen propios del juego. Los que salieron con Gears y ya están resueltos para el siguiente:
   - Texturas: el renderizador aplica el swizzle del fetch (en las texturas que salen de un resolve, solo las
     constantes 0/1) y el `TextureSign::kGamma`. La gamma no va con formatos sRGB: la curva de Xenos es por tramos
     (cortes en 64, 96 y 192) y da el doble de luz que sRGB en los tonos oscuros. El índice del sampler lleva el
     aviso en el bit 31 y `tfetch*` de `tools/shaders/xenos_shader_common.h` aplica la curva; cambiar ese archivo
     obliga a rehacer la caché de shaders (`shaders.py build`).
   - Vértices: los tipos enteros (`UBYTE4`...) llegan a una entrada float como números (0..255); van con formatos
     `USCALED`/`SSCALED`, que Vulkan no exige. Sin ellos el log avisa (`vertex format ... is not supported`) y las
     normales salen mal. Un formato nuevo hay que añadirlo también a `NativeVertexFormatSize`
     (`native_pipeline_policy.h`), o la pipeline se rechaza.
   - Rampa de gamma de la pantalla: `runtime/gpu/command_processor.cpp` lee la tabla de 256 entradas
     (`DC_LUT_30_COLOR`) y `hdr_present_ps.glsl` la aplica; los modos SEQ/PWL no están hechos.
   - EDRAM: varias superficies en la misma dirección son la misma memoria (`PrepareSurfaceContent`). UE3 guarda y
     restaura ahí la escena, y usa Z invertida con el borrado de profundidad dentro del resolve.
   - Superficies `2_10_10_10` (unorm) que en realidad llevan la escena en 7e3: van como float y sin recortar a 0..1
     (`ConvertSurfaceFormat` y `native_color_output.h`). Recortadas, la escena iluminada (a 8 veces su valor) se
     quedaba en un gris plano: caras y armaduras blanquecinas sin matices.
   - Listas de rectángulos (`kRectangleList`): sin culling ni bias de profundidad, como en Xenia
     (`IsPrimitivePolygonal`). UE3 restaura la profundidad de la escena para las luces con un rectángulo de
     pantalla completa que escribe `oDepth`; con el culling del juego se descartaba, las luces no tenían
     profundidad y sumaban todas las capas del personaje (el collar se veía a través de la cara).
   - Fetch con filtro de punto justo entre dos texels: XenosRecomp suma 1.5/1024 de texel al desplazamiento de
     cada `tfetch`/`getWeights`, como los traductores de Xenia. Con el medio píxel de D3D, un pase de pantalla
     completa cae exactamente en la frontera y, sin ese margen, algunas columnas leían el texel de al lado: la
     profundidad restaurada salía mal y las luces dejaban rayas verticales oscuras en la cara y el pecho.
     Comprobar el filtro con la traza (`:m000` = punto).

## 5. Estado

- Comprobado con GTA IV: compila, arranca en el PC y genera imagen tras el recorte; el APK se compila.
- `port.py` de principio a fin con GTA IV como si fuera nuevo (ranura `gta4test`, sin nada hecho a mano):
  recompila, encuentra 58/58 funciones D3D, compila, la primera vuelta sale negra (caché vacía), vuelca 1274
  shaders, la segunda dibuja la escena, el HUD y los subtítulos y sigue en marcha a los 4 minutos.
- Traductor de shaders en tiempo real: GTA IV en el PC con la caché vacía traduce sus 1274 shaders (20-30 ms
  cada uno), no le falta ninguno y dibuja; 1250 salen idénticos byte a byte a la caché hecha con
  `shaders.py` y los otros 24 son los que a la caché le faltaban. `libdxcompiler.so` ya está compilada y va en el
  APK; en el móvil el traductor está sin probar.
- `find_d3d.py`: probado con GTA IV y EFLC (mismo XDK): 58 de 58 funciones y 3 de 3 datos, idénticos a la tabla
  hecha a mano.
- En el móvil (`phone_test.py`, POCO X6 Pro, Dimensity 8300, Mali-G615): el APK de `gta4test` hecho por
  `port.py` sigue en marcha a los 4 minutos, 22 capturas distintas y ninguna negra, 30 fps de mediana (28 el
  peor 10%), batería de 30 a 34 °C; carga 128 recetas de pipelines de la semilla y el hilo de render va a su
  núcleo grande.
- Gears of War (UE3, XDK 2.0.3529, primer juego que no es de RAGE), comprobado en el PC con el renderizador nativo
  (2026-11-02): menús, carga y partida («Cenizas») con Marcus y Dom iluminados, 30 fps, ningún dibujo rechazado,
  sin caerse en 4 minutos, 493 shaders sin fallos de DXC. La cara y la armadura de Marcus ya no tienen manchas
  blancas, collar transparente ni rayas (rectángulos sin culling y margen de redondeo en los fetch, sección 4).
  Los menús se parecen a los de la GPU de Xenia; de la partida no hay referencia: la prueba con Xenia se paró por
  falta de memoria antes de llegar, así que el brillo y las sombras dentro del juego no están comparados. En el
  móvil, los arreglos de esta tanda están sin probar (entre ellos `USCALED` en Mali y la curva de gamma).
- Sin probar: el extractor de paquetes XBLA (`tools/stfs.py`) y la interfaz con el ratón (solo se comprobó que abre).
