# Drivers PanVK para Mali

Paquetes de drivers Vulkan PanVK (Mesa) compilados por la comunidad para GPUs
Mali con el driver de kernel kbase. `tools/build-apk.sh` los mete en el APK
(`assets/drivers/panvk-*.zip`) y el launcher solo los ofrece en móviles Mali.
Son experimentales: el driver del sistema sigue siendo la opción por defecto.

Los zip están tal cual los publicaron sus autores, sin cambios.

| Carpeta | Paquete | Autor | Android mínimo |
|---|---|---|---|
| `fristonerr-v3.7.0` | PanVK FristOneRR 3.7.0 | FristOneRR ([código](https://github.com/FristOneRR-Admin/FristOneRR-Panvk-Source)) | 9 (API 28) |
| `g925-v13-csf-0.0.1` | PanVK Kbase Android, Mali-G925 (v13 CSF) | panvk-kbase-android (Mesa `e2d83eff1c71`) | 15 (API 35) |
| `strato-safety-v4` | PanVK RC3 Safety v4, Strato Mali | LukeValen, zenithblue-oss | 15 (API 35) |

## Licencias

Los tres drivers están hechos a partir de [Mesa](https://mesa3d.org). La mayor
parte de Mesa, PanVK incluido, tiene la licencia MIT de abajo; algunos archivos
de Mesa llevan su propia cabecera de licencia (compatible y permisiva), que es
la que vale para esos archivos. El paquete de FristOneRR trae además su propio
`LICENSE-Mesa.txt` dentro del zip.

El paquete G925 lleva también `libdrm.so` (libdrm, licencia MIT) y `libc++.so`
(libc++ de LLVM, licencia Apache 2.0 con la excepción de LLVM).

```
Copyright (C) 1999-2007 Brian Paul   All Rights Reserved.
Copyright (C) The Mesa contributors.

Permission is hereby granted, free of charge, to any person obtaining a
copy of this software and associated documentation files (the "Software"),
to deal in the Software without restriction, including without limitation
the rights to use, copy, modify, merge, publish, distribute, sublicense,
and/or sell copies of the Software, and to permit persons to whom the
Software is furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included
in all copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS
OR IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL
THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING
FROM, OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER
DEALINGS IN THE SOFTWARE.
```
