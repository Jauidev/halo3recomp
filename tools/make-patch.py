#!/usr/bin/env python3
# Makes a patch for the launcher's Parches page (android/src/com/xport/Patches.java reads it): a
# .zip with manifest.json, its signature manifest.sig and the files the manifest lists with their
# size and SHA-256. The phone installs it without a new APK:
#   lib/libgame.so         the game's library (the runtime, the renderer, the shader cache, the
#                          recompiled code), taken from a built APK; MainActivity loads it instead
#                          of the APK's own. Needs the same Java side (abi) and a newer build.
#   shaders/<HASH>.vs.spv  a shader's SPIR-V by its hash (also .ps; a pixel shader with a late-Z
#   shaders/<HASH>.ps.late.spv  variant needs both), read by graphics_system.cpp instead of the
#                          cache's copy; for_build names the library build they were made for.
#   config/opciones.txt    "cvar name=value" (renderer options) and "NAME=value" (start options).
# The signature (ECDSA P-256 over manifest.json, SHA-256, DER) is checked against the public keys
# in Patches.java; only a patch of renderer options alone may come unsigned ("No oficial").
#
# Usage (Git Bash, from the project folder):
#   py -3 tools/make-patch.py --new-key                      once: makes the signing key
#   py -3 tools/make-patch.py --apk build/apk-halo3/xport-halo3.apk --version 1.0.1 \
#       --name "Arreglo de sombras" [--description "..."] [--base old.apk] [--out folder]
#   py -3 tools/make-patch.py --config opciones.txt --name "..." --version ... [--unsigned]
#   py -3 tools/make-patch.py --shaders folder --for-build 20261111193000 --name ... --version ...
#   py -3 tools/make-patch.py --verify patch.zip
# The private key stays in %USERPROFILE%\.xport\parches (never in the project, which is public).
import argparse
import base64
import datetime
import hashlib
import io
import json
import os
import re
import sys
import zipfile

KEY_DIR = os.path.join(os.path.expanduser("~"), ".xport", "parches")
PRIVATE_KEY = os.path.join(KEY_DIR, "clave-privada.pem")
PUBLIC_KEY = os.path.join(KEY_DIR, "clave-publica.txt")
LIB = "lib/arm64-v8a/libgame.so"
SHADER_NAME = re.compile(r"^[0-9A-F]{16}\.(vs|ps)(\.late)?\.spv$")
CVAR_LINE = re.compile(r"^cvar ([A-Za-z0-9_]+)=([^\s,]+)$")
OPTION_LINE = re.compile(r"^([A-Z][A-Z0-9_]*)=(\S+)$")
# Patches.java's ID: the patch's folder name on the phone.
PATCH_ID = re.compile(r"^[a-z0-9][a-z0-9._-]{0,63}$")


def fail(message):
    print("ERROR: " + message, file=sys.stderr)
    sys.exit(1)


def crypto():
    try:
        from cryptography.hazmat.primitives import hashes, serialization
        from cryptography.hazmat.primitives.asymmetric import ec
    except ImportError:
        fail("falta el módulo cryptography: py -3 -m pip install cryptography")
    return hashes, serialization, ec


def new_key():
    hashes, serialization, ec = crypto()
    if os.path.exists(PRIVATE_KEY):
        fail("ya hay una clave en " + PRIVATE_KEY + " (bórrala a mano si de verdad quieres otra: "
             "los móviles con el APK actual solo aceptan parches firmados con la vieja)")
    os.makedirs(KEY_DIR, exist_ok=True)
    key = ec.generate_private_key(ec.SECP256R1())
    with open(PRIVATE_KEY, "wb") as f:
        f.write(key.private_bytes(serialization.Encoding.PEM, serialization.PrivateFormat.PKCS8,
                                  serialization.NoEncryption()))
    public = key.public_key().public_bytes(serialization.Encoding.DER,
                                           serialization.PublicFormat.SubjectPublicKeyInfo)
    text = base64.b64encode(public).decode()
    with open(PUBLIC_KEY, "w") as f:
        f.write(text + "\n")
    print("clave privada: " + PRIVATE_KEY + "  (haz una copia y no la compartas)")
    print("clave pública (va en Patches.java, KEYS): " + text)


def load_private_key():
    hashes, serialization, ec = crypto()
    if not os.path.exists(PRIVATE_KEY):
        fail("no hay clave de firma: py -3 tools/make-patch.py --new-key")
    with open(PRIVATE_KEY, "rb") as f:
        return serialization.load_pem_private_key(f.read(), password=None)


def sign(data):
    hashes, serialization, ec = crypto()
    return load_private_key().sign(data, ec.ECDSA(hashes.SHA256()))


def verify_signature(data, signature, keys):
    hashes, serialization, ec = crypto()
    from cryptography.exceptions import InvalidSignature
    for text in keys:
        key = serialization.load_der_public_key(base64.b64decode(text))
        try:
            key.verify(signature, data, ec.ECDSA(hashes.SHA256()))
            return True
        except InvalidSignature:
            continue
    return False


def app_keys():
    # The keys the app trusts: Patches.java's KEYS.
    java = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "android", "src", "com", "xport", "Patches.java")
    with open(java, encoding="utf-8") as f:
        source = f.read()
    block = re.search(r"KEYS\s*=\s*\{(.*?)\};", source, re.S)
    return re.findall(r'"([A-Za-z0-9+/=]+)"', block.group(1)) if block else []


def build_info(apk):
    # tools/build-apk.sh writes assets/build.txt: build=, abi=, game=, version=.
    try:
        text = apk.read("assets/build.txt").decode()
    except KeyError:
        fail("el APK no tiene assets/build.txt: hazlo con el tools/build-apk.sh que trae los parches")
    info = dict(line.split("=", 1) for line in text.splitlines() if "=" in line)
    for key in ("build", "abi", "game"):
        if not info.get(key):
            fail("assets/build.txt sin " + key)
    return info


def compare_base(new, base, force):
    # What a patch cannot carry: the Java side, the other libraries, the manifest.
    problems = []
    new_info, base_info = build_info(new), build_info(base)
    if new_info["abi"] != base_info["abi"]:
        problems.append("la interfaz Java/nativa cambió (abi {} -> {}): hace falta APK nuevo".format(base_info["abi"], new_info["abi"]))
    if int(new_info["build"]) <= int(base_info["build"]):
        problems.append("el APK nuevo ({}) no es más reciente que la base ({})".format(new_info["build"], base_info["build"]))
    base_entries = {i.filename: i.CRC for i in base.infolist()}
    for item in new.infolist():
        name = item.filename
        if name in (LIB, "assets/build.txt") or name.startswith("META-INF/"):
            continue
        if base_entries.get(name) != item.CRC:
            problems.append("{} {}: un parche no puede cambiarlo".format(name, "es nuevo" if name not in base_entries else "cambió"))
    for name in base_entries:
        if name not in {i.filename for i in new.infolist()} and not name.startswith("META-INF/"):
            problems.append(name + " ya no está en el APK nuevo")
    for problem in problems:
        print(("AVISO: " if force else "ERROR: ") + problem, file=sys.stderr)
    if problems and not force:
        fail("el APK nuevo no se puede llevar como parche sobre esa base (--force para hacerlo igual)")
    if base.read(LIB) == new.read(LIB):
        fail("libgame.so es igual en los dos APK: no hay nada que parchear")


def check_config(text, unsigned):
    lines = []
    for number, raw in enumerate(text.splitlines(), 1):
        line = raw.strip()
        if not line or line.startswith("#"):
            continue
        cvar, option = CVAR_LINE.match(line), OPTION_LINE.match(line)
        if unsigned:
            if not cvar or not cvar.group(1).startswith("xnative_") or not re.fullmatch(r"[A-Za-z0-9._-]{1,64}", cvar.group(2)):
                fail("opciones.txt línea {}: un parche sin firma solo admite 'cvar xnative_...=valor'".format(number))
        elif not cvar and not option:
            fail("opciones.txt línea {}: se esperaba 'cvar nombre=valor' o 'NOMBRE=valor': {}".format(number, raw))
        elif option and (option.group(1).startswith("XPORT_PATCH_") or option.group(1) == "XPORT_NATIVE_LIB_DIR"):
            # The app sets these itself; Patches.java refuses a patch that does.
            fail("opciones.txt línea {}: {} lo pone la app, un parche no puede".format(number, option.group(1)))
        lines.append(line)
    if not lines:
        fail("opciones.txt no tiene ninguna opción")
    return "\n".join(lines) + "\n"


def make(args):
    files = {}  # path in the zip -> bytes
    manifest = {"format": 1}
    package = None
    if args.apk:
        with zipfile.ZipFile(args.apk) as apk:
            info = build_info(apk)
            package = "com.xport." + info["game"]
            if args.base:
                with zipfile.ZipFile(args.base) as base:
                    compare_base(apk, base, args.force)
            if not args.no_lib:
                files["lib/libgame.so"] = apk.read(LIB)
                manifest["abi"] = int(info["abi"])
                manifest["build"] = int(info["build"])
    if args.shaders:
        if not args.for_build:
            fail("--shaders necesita --for-build (la build de la librería para la que se hicieron)")
        manifest["for_build"] = int(args.for_build)
        names = sorted(os.listdir(args.shaders))
        for name in names:
            if not SHADER_NAME.match(name):
                fail("shaders: nombre no válido {} (HASH16.vs.spv, HASH16.ps.spv, HASH16.ps.late.spv)".format(name))
            data = open(os.path.join(args.shaders, name), "rb").read()
            if len(data) < 20 or len(data) % 4 or data[:4] != b"\x03\x02\x23\x07":
                fail("shaders: {} no es SPIR-V".format(name))
            files["shaders/" + name] = data
        if not names:
            fail("la carpeta de shaders está vacía")
    if args.config:
        files["config/opciones.txt"] = check_config(open(args.config, encoding="utf-8").read(), args.unsigned).encode()
    if not files:
        fail("el parche no lleva nada: --apk, --shaders o --config")
    if args.unsigned and set(files) != {"config/opciones.txt"}:
        fail("solo un parche de opciones puede ir sin firma")
    package = package or args.package
    if not package:
        fail("--package com.xport.<juego> (o --apk, que lo dice)")
    version = args.version
    ident = args.id or re.sub(r"[^a-z0-9._-]+", "-", "{}-{}".format(package.split(".")[-1], version).lower()).strip("-.")
    if not PATCH_ID.match(ident):
        fail("identificador no válido '{}': minúsculas, números, . _ -, hasta 64, sin empezar por . o -".format(ident))
    manifest.update({
        "id": ident,
        "name": args.name,
        "version": version,
        "description": args.description or "",
        "package": package,
        "gpu": args.gpu,
        "created": datetime.datetime.now().strftime("%Y-%m-%d %H:%M"),
    })
    if args.min_api:
        manifest["min_api"] = args.min_api
    manifest["files"] = [{"path": path, "size": len(data), "sha256": hashlib.sha256(data).hexdigest()}
                         for path, data in sorted(files.items())]
    manifest_bytes = json.dumps(manifest, ensure_ascii=False, indent=2).encode("utf-8")
    os.makedirs(args.out, exist_ok=True)
    out = os.path.join(args.out, ident + ".zip")
    with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=9) as patch:
        patch.writestr("manifest.json", manifest_bytes)
        if not args.unsigned:
            patch.writestr("manifest.sig", sign(manifest_bytes))
        for path, data in sorted(files.items()):
            patch.writestr(path, data)
    print("parche: " + out)
    print("  {} {} ({}) · {} · {}".format(args.name, version, ident, "sin firma" if args.unsigned else "firmado",
                                        "{:.1f} MB".format(os.path.getsize(out) / 1e6)))
    for entry in manifest["files"]:
        print("  {}  {:.1f} MB".format(entry["path"], entry["size"] / 1e6))
    verify(out)


def verify(path):
    with zipfile.ZipFile(path) as patch:
        names = set(patch.namelist())
        manifest_bytes = patch.read("manifest.json")
        manifest = json.loads(manifest_bytes)
        listed = {entry["path"]: entry for entry in manifest["files"]}
        extra = names - set(listed) - {"manifest.json", "manifest.sig"}
        if extra:
            fail("archivos sin listar: " + ", ".join(sorted(extra)))
        for path_, entry in listed.items():
            data = patch.read(path_)
            if len(data) != entry["size"] or hashlib.sha256(data).hexdigest() != entry["sha256"]:
                fail(path_ + " no coincide con el manifest")
        if "manifest.sig" in names:
            keys = app_keys()
            if not keys:
                fail("Patches.java no tiene claves públicas (KEYS)")
            if not verify_signature(manifest_bytes, patch.read("manifest.sig"), keys):
                fail("la firma no es de ninguna clave de Patches.java")
            print("comprobado: firma válida, {} archivos íntegros".format(len(listed)))
        else:
            print("comprobado: SIN firma (No oficial), {} archivos íntegros".format(len(listed)))


def main():
    for stream in (sys.stdout, sys.stderr):
        try:
            stream.reconfigure(encoding="utf-8", errors="replace")
        except AttributeError:
            pass
    parser = argparse.ArgumentParser(description="Crea y firma parches para el launcher (página Parches).")
    parser.add_argument("--new-key", action="store_true", help="crea la clave de firma (una vez)")
    parser.add_argument("--verify", metavar="ZIP", help="comprueba un parche")
    parser.add_argument("--apk", help="APK construido con build-apk.sh: su libgame.so va en el parche")
    parser.add_argument("--base", help="APK que tienen los móviles: comprueba que el parche basta")
    parser.add_argument("--no-lib", action="store_true", help="no llevar libgame.so del --apk")
    parser.add_argument("--force", action="store_true", help="seguir aunque --base vea cambios que el parche no lleva")
    parser.add_argument("--shaders", help="carpeta con HASH16.vs.spv / .ps.spv / .ps.late.spv")
    parser.add_argument("--for-build", help="build de la libgame.so para la que se hicieron los shaders")
    parser.add_argument("--config", help="opciones.txt: 'cvar nombre=valor' y 'NOMBRE=valor'")
    parser.add_argument("--unsigned", action="store_true", help="sin firma (solo opciones cvar xnative_*)")
    parser.add_argument("--package", help="com.xport.<juego>, sin --apk")
    parser.add_argument("--name", help="nombre que se ve en el launcher")
    parser.add_argument("--version", help="versión que se ve en el launcher (1.0.1)")
    parser.add_argument("--description", help="una línea sobre lo que arregla")
    parser.add_argument("--id", help="identificador (por defecto <juego>-<versión>)")
    parser.add_argument("--gpu", choices=["all", "mali", "adreno"], default="all")
    parser.add_argument("--min-api", type=int, help="Android mínimo (API)")
    parser.add_argument("--out", default="parches", help="carpeta de salida (parches/)")
    args = parser.parse_args()
    if args.new_key:
        new_key()
    elif args.verify:
        verify(args.verify)
    else:
        if not args.name or not args.version:
            fail("faltan --name y --version")
        make(args)


if __name__ == "__main__":
    main()
