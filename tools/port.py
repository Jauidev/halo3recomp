"""Every automatic step of a port, one after the other, and one report: from the disc to the APK.

  port.py <game.iso|default.xex|XBLA package|disc folder> <work dir> <slot name> [options]
    --label "Name"      the app's name on the phone (default: the slot name)
    --seconds N         how long the boot test runs (default 300)
    --gpu native|xenia  the renderer for the boot test (default native)
    --no-build          stop after the slot and the shaders
    --no-apk            stop after the boot test
    --compare           boot it on Xenia's GPU too and compare the images (tools/compare_gpu.py):
                        where the native renderer starts drawing wrong
    --phone             install the APK on the phone connected over adb and test it there
                        (tools/phone_test.py; copies the game to the phone the first time)

  1  tools/xport.py     the XEX, recompiled until XenonRecomp has nothing left to report
  2  tools/slot.py      games/<slot>: the code, its D3D functions (find_d3d.py), its imports
  3  tools/shaders.py   the shader cache from the disc
  4  build              tools/build-host.bat with XPORT_GAME=<slot>
  5  tools/boot_test.py the game on the PC, with its shaders dumped (the ones the cache lacks are
                        translated while it runs, so the first boot already draws)
  6  tools/shaders.py   the cache again with the dump, so that the phone does not translate them;
                        the boot's pipeline recipes go to games/<slot>/data/pipeline_recipes.bin
  7  APK               tools/build-android.sh xportapp and tools/build-apk.sh
  8  report.md          everything above, in <work dir>

Each step's output goes to <work dir>/logs/<step>.log; progress lines go to the standard output.
"""
import json, os, re, shutil, subprocess, sys, time

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
TOOLS = os.path.join(ROOT, "tools")
# Git Bash runs the Android scripts (not the bash.exe of WSL that may come first in PATH).
BASH = os.path.join(os.environ.get("ProgramFiles", r"C:\Program Files"), "Git", "bin", "bash.exe")
# What the native renderer needs from a game with another XDK than GTA IV's (XD3D_CORE in
# cmake/rexglue.cmake).
D3D_CORE = {"DrawVertices", "DrawIndexedVertices", "ClearRectangle", "Resolve", "Swap", "SetStreamSource", "SetIndices",
            "SetTexture", "SetVertexShader", "SetPixelShader", "CreateVertexShader", "CreatePixelShader", "SetRenderTarget",
            "SetDepthStencilSurface", "LockResource", "UnlockResource", "ReleaseResource", "LockTextureRect",
            "LockTextureRectArray", "ADDR_NullClearColor"}


def step(message):
    print(f"[{time.strftime('%H:%M:%S')}] {message}", flush=True)


def run(name, args, work, env=None):
    """Runs one step with its output in logs/<name>.log; returns (exit code, output)."""
    os.makedirs(os.path.join(work, "logs"), exist_ok=True)
    path = os.path.join(work, "logs", name + ".log")
    with open(path, "w", encoding="utf-8", errors="replace") as log:
        process = subprocess.Popen(args, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, env=env, cwd=ROOT)
        for raw in process.stdout:
            line = raw.decode("utf-8", "replace")
            log.write(line)
            log.flush()
            if line.startswith("[") and name == "xport":
                print("  " + line.rstrip(), flush=True)
        code = process.wait()
    with open(path, encoding="utf-8", errors="replace") as f:
        return code, f.read()


def python(name, script, args, work):
    return run(name, [sys.executable, "-I", os.path.join(TOOLS, script), *args], work)


def build(slot, work, label):
    env = dict(os.environ, XPORT_GAME=slot)
    code, text = run(f"build-{label}", ["cmd", "/c", os.path.join(TOOLS, "build-host.bat"), "--target", "xport"], work, env)
    return build_result(code, text, os.path.join(ROOT, "build", f"x64-{slot}", f"{slot}.exe"))


def apk(slot, work):
    """libgame.so for arm64, then the APK (the second only when the first links)."""
    env = dict(os.environ, XPORT_GAME=slot)
    code, text = run("build-android", [BASH, "tools/build-android.sh", "xportapp"], work, env)
    library = os.path.join(ROOT, "build", f"arm64-{slot}", "libgame.so")
    result = build_result(code, text, library)
    if result["ok"]:
        code, text = run("build-apk", [BASH, "tools/build-apk.sh"], work, env)
        result = build_result(code, text, os.path.join(ROOT, "build", f"apk-{slot}", f"xport-{slot}.apk"))
        result["no_dxc"] = bool(re.search(r"warning: no \S*libdxcompiler\.so", text))
    return result


def build_result(code, text, exe):
    errors = []
    for line in text.splitlines():
        if re.search(r"error(:| LNK| C\d)|undefined symbol|unresolved external", line) and line.strip() not in errors:
            errors.append(line.strip()[:240])
    failed = code != 0 or "ninja: build stopped" in text or not os.path.exists(exe)
    missing_d3d = sorted(set(re.findall(r"use of undeclared identifier '(XD3D_\w+)'", text)))
    unresolved = sorted(set(re.findall(r"unresolved external symbol \"?[^\"]*?(\w+)\"? referenced", text)))
    return {"ok": not failed, "exe": exe, "errors": errors[:40], "missing_d3d": missing_d3d, "unresolved": unresolved[:40]}


def pipeline_recipes(boot_dir):
    """The renderer's recipes file from the boot (native-cache/xnative/<title>/*.recipes), or None."""
    found = []
    for folder, _, names in os.walk(os.path.join(boot_dir, "native-cache")):
        found += [os.path.join(folder, n) for n in names if n.endswith(".recipes")]
    return max(found, key=os.path.getsize) if found else None


def game_folder(source, work):
    if os.path.isdir(source):
        return source
    if os.path.isdir(os.path.join(work, "disc")):
        return os.path.join(work, "disc")
    return os.path.dirname(source)


def load_json(work, path):
    try:
        with open(os.path.join(work, path), encoding="utf-8") as f:
            return json.load(f)
    except (OSError, ValueError):
        return None


def todo(work, slot, parts):
    """What is left to do, the most blocking first, from what the steps found."""
    items = []
    steps = dict(parts["steps"])
    if steps.get("recompilación", "hecha") != "hecha":
        items.append("La recompilación falla: `logs/xport.log` y la sección Recompilación (XenonRecomp, `game.toml`).")
    if steps.get("ranura", "").startswith("falla"):
        items.append("La ranura falla: `logs/slot.log`.")
    s = parts.get("slot")
    if s:
        d = s["d3d"]
        # cmake/rexglue.cmake: with GTA IV's XDK the native renderer needs every function, with another
        # only these (its hooks run the game's functions and tell the renderer).
        needed = [n for n in d["missing"] + sorted(d["ambiguous"]) + [f"ADDR_{n}" for n in d.get("data_missing", [])]
                  if not n.startswith("Lzx") and (d["same_xdk"] or n in D3D_CORE)]
        if needed:
            items.append("Sin estas funciones D3D el juego se compila sin el renderizador nativo y dibuja la GPU de "
                         "Xenia (va, pero más lenta en el móvil): " + ", ".join(f"`{n}`" for n in needed) + ". "
                         f"Buscarlas a mano y apuntarlas en `games/{slot}/xd3d_fixes.txt` (`<nombre> <dirección>`).")
        if not d["same_xdk"]:
            lacking = [n for n in d["missing"] + sorted(d["ambiguous"]) if not n.startswith("Lzx") and n not in D3D_CORE]
            items.append(f"Otro XDK ({d['xdk']}): los hooks ejecutan las funciones D3D del juego y avisan al renderizador "
                         "nativo. Los dibujos con vértices del título (DrawPrimitiveUP) le llegan desde `BeginVertices` "
                         "y los indexados desde `BeginIndexedVertices` si está en `xd3d_fixes.txt` (los vértices se "
                         "copian en la siguiente llamada D3D, aunque `EndVertices` esté inlined); no le llegan las "
                         "command lists (`RunCommandList`)." + (
                             " Sin hook por no encontradas o inlined: " + ", ".join(f"`{n}`" for n in lacking) + "."
                             if lacking else ""))
        guessed = list(d.get("by_calls", {})) + list(d.get("similar", {}))
        if guessed:
            items.append("Si la imagen sale mal, comprobar primero las funciones D3D sin firma exacta (encontradas por "
                         "sus llamadas o por parecido): " + ", ".join(f"`{n}`" for n in guessed) + ".")
        dev = d.get("device") or {}
        if dev.get("missing"):
            # cmake/rexglue.cmake: also without the native renderer.
            names = [str(n[0] if isinstance(n, list) else n) for n in dev["missing"]]
            items.append(f"Faltan {len(names)} campos del device D3D (otro XDK, {d['xdk']}), así que tampoco hay "
                         f"renderizador nativo: buscarlos a mano en `games/{slot}/xd3d_device.h` (" +
                         ", ".join(f"`{n}`" for n in names) + ").")
        if not d["same_xdk"]:
            items.append(f"El juego usa otro XDK ({d['xdk']}): si algo se dibuja mal, revisar primero los campos del device "
                         f"adivinados en `games/{slot}/xd3d_device.h` (" +
                         ", ".join(f"`{n}`" for n in dev.get("guessed", [])) + ")." if dev.get("guessed") else
                         f"El juego usa otro XDK ({d['xdk']}): si algo se dibuja mal, revisar `games/{slot}/xd3d_device.h`.")
    for key, label in (("build-pc", "el PC"), ("apk", "Android")):
        b = parts.get(key)
        if b and not b["ok"]:
            why = (" Faltan funciones D3D: " + ", ".join(f"`{n}`" for n in b["missing_d3d"]) + "." if b["missing_d3d"] else
                   " Símbolos sin resolver: " + ", ".join(f"`{n}`" for n in b["unresolved"][:8]) + "." if b["unresolved"] else
                   f" Primer error: `{b['errors'][0]}`" if b["errors"] else "")
            items.append(f"La compilación para {label} falla.{why}")
    for folder, where in (("boot", "en el PC"), ("phone", "en el móvil")):
        r = load_json(work, os.path.join(folder, "boot.json"))
        if not r:
            continue
        if r["ending"] == "crashed":
            items.append(f"Se cae {where} a los {r['seconds']} s ({r['crash'] or r['exception'] or r['exit_code']}): "
                         f"`{folder}/crash.txt` da la función invitada y la pila.")
        elif r["ending"] == "hung":
            items.append(f"Se cuelga {where} a los {r['seconds']} s: lo último de `{folder}/stdout.log` y `{folder}/run/xnative.log`.")
        elif r["ending"] == "exited":
            items.append(f"Se cierra solo {where} a los {r['seconds']} s (código {r['exit_code']}): el final de `{folder}/stdout.log`.")
        elif r["captures"]["unique"] <= 1 or r["captures"]["black"] >= r["captures"]["count"]:
            items.append(f"Corre {where} pero no dibuja: probar `--compare` (si la GPU de Xenia dibuja, el fallo es del "
                         "renderizador nativo o de las funciones D3D; si tampoco, es del juego o del runtime).")
        if r.get("shader_cache_misses"):
            items.append(f"{r['shader_cache_misses']} shaders no se dibujan {where} (ni en la caché ni traducidos): "
                         f"`shader cache miss` en `{folder}/run/xnative.log`.")
        if r.get("translation_failures"):
            items.append(f"{r['translation_failures']} shaders no se pudieron traducir {where}: XenosRecomp no los entiende.")
        if r.get("draws_rejected"):
            reasons = ", ".join(f"`{reason}`" for reason, _ in r.get("pipeline_rejects", [])) or ", ".join(f"`{reason}`" for reason in r["draws_rejected"])
            items.append(f"El renderizador rechaza {sum(r['draws_rejected'].values())} dibujos {where} ({reasons}): no se ven; "
                         f"`xnative-diag` en `{folder}/run/xnative.log`.")
        if folder == "phone" and r.get("fps") and r["fps"]["median"] < 28:
            perf = (r.get("device") or {}).get("performance")
            items.append(f"En el móvil va a {r['fps']['median']} fps de mediana. Lo limita "
                         + (perf["limit"] if perf else "algo sin medir (falta la sección Rendimiento de `phone/boot.md`)") + ".")
    stubs = (load_json(work, os.path.join("boot", "boot.json")) or {}).get("stub_calls")
    if stubs:
        items.append("Imports que el juego llama y el runtime solo tiene como stub (implementar en `runtime/kernel` "
                     "los que el juego necesite de verdad): " + ", ".join(f"`{n}`" for n in stubs) + ".")
    c = load_json(work, "compare.json")
    if c and c.get("first_divergence"):
        items.append(f"El renderizador nativo dibuja otra cosa desde `{c['native_cap']}/{c['first_divergence']['native']}`: "
                     "`compare.md`, y en el log nativo los render targets, resolves y shaders de ese momento.")
    a = parts.get("apk")
    if a and a.get("no_dxc"):
        items.append("Compilar DXC para Android una vez (`tools\\build-dxc-android.bat`): sin él el móvil solo dibuja "
                     "los shaders de la caché.")
    if not items:
        items.append("Nada bloquea. Siguiente: probarlo en el móvil (`--phone`) y jugar.")
    return ["## Qué hacer ahora", ""] + [f"{k}. {item}" for k, item in enumerate(items, 1)] + [""]


def report(work, slot, parts):
    lines = [f"# Port de `{slot}`", "", f"Generado por tools/port.py el {time.strftime('%Y-%m-%d %H:%M')}.", ""]
    lines += todo(work, slot, parts)
    lines += ["## Pasos", ""] + [f"- {name}: {state}" for name, state in parts["steps"]] + [""]
    recompile = os.path.join(work, "recompile.md")
    if os.path.exists(recompile):
        with open(recompile, encoding="utf-8") as f:
            text = f.read()
        lines += ["## Recompilación (tools/xport.py)", ""] + [("#" + l if l.startswith("#") else l) for l in text.splitlines()[1:]] + [""]
    s = parts.get("slot")
    if s:
        d = s["d3d"]
        lines += ["## Funciones D3D (tools/find_d3d.py)", "",
                  f"- {d['found']} de {d['total']} encontradas; datos {d['data_found']} de {d['data_total']}",
                  f"- XDK del juego {d['xdk']}, firmas de {d['reference_xdk']} ({'el mismo' if d['same_xdk'] else 'otro: revisar los offsets del device'})"]
        if d["missing"]:
            lines.append("- No encontradas: " + ", ".join(f"`{n}`" for n in d["missing"]))
        if d["ambiguous"]:
            lines.append("- Varias candidatas: " + ", ".join(f"`{n}`" for n in d["ambiguous"]))
        if d.get("by_calls"):
            lines.append("- Sin firma exacta, encontradas por sus llamadas (comprobarlas si algo falla): " +
                         ", ".join(f"`{n}` en sub_{hit['address']} ({hit['score']})" for n, hit in d["by_calls"].items()))
        if d.get("similar"):
            lines.append("- Sin firma exacta, encontradas por parecido (comprobarlas si algo falla): " +
                         ", ".join(f"`{n}` en sub_{hit['address']} ({hit['score']})" for n, hit in d["similar"].items()))
        if d.get("inlined"):
            lines.append("- Dentro de otras funciones (no se pueden enganchar solas): " +
                         ", ".join(f"`{n}` en " + ", ".join(f"sub_{a}" for a in where) for n, where in d["inlined"].items()))
        if d["data_missing"]:
            lines.append("- Datos no encontrados: " + ", ".join(f"`{n}`" for n in d["data_missing"]))
        lines += [f"- Ranura `{s['slot']}`, {s['imports']} imports en runtime-imports.txt" +
                  (f"; archivos nuevos: {', '.join(s['new_files'])}" if s["new_files"] else ""), ""]
    for key, title in (("shaders", "Shaders (caché del disco)"), ("shaders2", "Shaders (caché del APK, con el volcado)")):
        sh = parts.get(key)
        if sh:
            lines += [f"## {title}", "", f"- {sh['shaders']} shaders en la caché, {sh['failed']} que DXC no compiló", ""]
    for key, title in (("build-pc", "Compilación para el PC"), ("apk", "APK")):
        b = parts.get(key)
        if not b:
            continue
        lines += [f"## {title}", "", "- " + ("Hecho: " + os.path.relpath(b["exe"], ROOT) if b["ok"] else "**Falla**")]
        if b.get("no_dxc"):
            lines.append("- Sin `libdxcompiler.so` (tools\\build-dxc-android.bat): en el móvil solo se dibujan los shaders de la caché")
        if b["missing_d3d"]:
            lines.append("- Funciones D3D que los hooks necesitan y no se encontraron: " + ", ".join(f"`{n}`" for n in b["missing_d3d"]))
        if b["unresolved"]:
            lines.append("- Símbolos sin resolver: " + ", ".join(f"`{n}`" for n in b["unresolved"]))
        if not b["ok"] and b["errors"]:
            lines += ["", "```"] + b["errors"][:25] + ["```"]
        lines.append("")
    for path in (os.path.join(work, "boot", "boot.md"), os.path.join(work, "compare.md"),
                 os.path.join(work, "phone", "boot.md")):
        if os.path.exists(path):
            with open(path, encoding="utf-8") as f:
                lines += [f.read()]
    with open(os.path.join(work, "report.md"), "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")


def main():
    argv = sys.argv[1:]
    options = {"--label": None, "--seconds": "300", "--gpu": "native"}
    positional, i, no_build, no_apk, compare, phone = [], 0, False, False, False, False
    while i < len(argv):
        if argv[i] in options and i + 1 < len(argv):
            options[argv[i]] = argv[i + 1]
            i += 2
            continue
        if argv[i] == "--no-build":
            no_build = True
        elif argv[i] == "--no-apk":
            no_apk = True
        elif argv[i] == "--compare":
            compare = True
        elif argv[i] == "--phone":
            phone = True
        else:
            positional.append(argv[i])
        i += 1
    if len(positional) != 3:
        raise SystemExit(__doc__)
    source, work, slot = os.path.abspath(positional[0]), os.path.abspath(positional[1]), positional[2]
    os.makedirs(work, exist_ok=True)
    # The optional parts of an earlier run in this folder, so that report.md has only this run's.
    for stale in ("compare.md", "compare.json", os.path.join("phone", "boot.md"), os.path.join("phone", "boot.json")):
        if os.path.exists(os.path.join(work, stale)):
            os.remove(os.path.join(work, stale))
    sys.stdout.reconfigure(encoding="utf-8")
    parts = {"steps": []}

    def done(name, state):
        parts["steps"].append((name, state))
        report(work, slot, parts)
        step(f"{name}: {state}")

    step(f"1/7 Recompilando {source}")
    code, _ = python("xport", "xport.py", [source, work, "--extract"], work)
    if os.path.exists(os.path.join(work, "report.md")):
        shutil.move(os.path.join(work, "report.md"), os.path.join(work, "recompile.md"))
    if code != 0 or not os.path.exists(os.path.join(work, "ppc", "ppc_func_mapping.cpp")):
        return done("recompilación", f"falla (salida {code}, ver logs/xport.log)")
    done("recompilación", "hecha")

    step("2/7 Ranura del juego")
    args = [work, slot] + ([options["--label"]] if options["--label"] else [])
    code, text = python("slot", "slot.py", args, work)
    if code != 0:
        return done("ranura", f"falla (ver logs/slot.log): {text.strip().splitlines()[-1] if text.strip() else ''}")
    with open(os.path.join(work, "slot.json")) as f:
        parts["slot"] = json.load(f)
    done("ranura", text.strip().splitlines()[-1])

    step("3/7 Shaders del disco")
    folder = game_folder(source, work)

    def shaders(key, inputs):
        code, text = python(key, "shaders.py", ["build", slot, *inputs], work)
        m = re.search(r"(\d+) shaders, (\d+) DXC failures", text)
        parts[key] = {"shaders": int(m[1]) if m else 0, "failed": int(m[2]) if m else 0}
        return code, text

    code, text = shaders("shaders", [folder])
    done("shaders", f"{parts['shaders']['shaders']} en la caché" if code == 0 else "falla (ver logs/shaders.log)")
    if no_build:
        return

    step("4/7 Compilando para el PC")
    b = build(slot, work, "pc")
    parts["build-pc"] = b
    if not b["ok"]:
        return done("compilación para el PC", "falla (ver logs/build-pc.log)")
    done("compilación para el PC", "enlaza")

    step(f"5/7 Arrancando {options['--seconds']} s")
    boot_dir = os.path.join(work, "boot")
    python("boot", "boot_test.py",
           [b["exe"], folder, boot_dir, options["--seconds"], "--gpu", options["--gpu"], "--dump-shaders"], work)
    with open(os.path.join(boot_dir, "boot.json"), encoding="utf-8") as f:
        boot = json.load(f)
    done("arranque", f"{boot['ending']} a los {boot['seconds']} s, {boot['captures']['unique']} capturas distintas, "
         f"{boot.get('shaders_translated', 0)} shaders traducidos")
    if compare and options["--gpu"] == "native":
        # The same boot on Xenia's GPU: where the native renderer starts drawing something else.
        step(f"5b/7 Arrancando {options['--seconds']} s con la GPU de Xenia, para comparar")
        xenia_dir = os.path.join(work, "boot-xenia")
        python("boot-xenia", "boot_test.py", [b["exe"], folder, xenia_dir, options["--seconds"], "--gpu", "xenia"], work)
        python("compare", "compare_gpu.py", ["--analyze", work, boot_dir, xenia_dir], work)
        with open(os.path.join(work, "compare.json"), encoding="utf-8") as f:
            c = json.load(f)
        done("comparación con Xenia", f"{c.get('different', 0)} de {len(c['pairs'])} capturas distintas (compare.md)"
             if c["pairs"] else f"sin capturas (nativo {c['native_frames']}, Xenia {c['xenia_frames']})")

    # The shaders the game registered go into the cache the APK carries: the phone then translates
    # only the ones this boot did not reach.
    step("6/7 Caché de shaders para el APK")
    dump = os.path.join(boot_dir, "shaders")
    code, text = python("shaders-check", "shaders.py", ["check", slot, dump], work) if os.path.isdir(dump) else (1, "")
    m = re.search(r"(\d+) missing", text)
    if m and m[1] != "0":
        shaders("shaders2", [folder, dump])
        done("caché para el APK", f"{parts['shaders2']['shaders']} en la caché ({m[1]} del volcado)")
    else:
        done("caché para el APK", "la del disco ya los tiene todos")
    # The pipelines the boot compiled, as recipes: the build puts them in libgame.so and the
    # phone's first run compiles them before the game draws with them.
    recipes = pipeline_recipes(boot_dir)
    if recipes:
        shutil.copyfile(recipes, os.path.join(ROOT, "games", slot, "data", "pipeline_recipes.bin"))
        done("recetas de pipelines", f"{(os.path.getsize(recipes) - 40) // 1024} KiB para el primer arranque del móvil")
    if no_apk:
        return step(f"Informe: {os.path.join(work, 'report.md')}")

    step("7/7 APK")
    a = apk(slot, work)
    parts["apk"] = a
    done("APK", os.path.relpath(a["exe"], ROOT) if a["ok"] else "falla (ver logs/build-android.log y logs/build-apk.log)")
    if phone and a["ok"]:
        step(f"Probando el APK en el móvil {options['--seconds']} s")
        phone_dir = os.path.join(work, "phone")
        code, text = python("phone", "phone_test.py", [slot, folder, phone_dir, options["--seconds"]], work)
        if os.path.exists(os.path.join(phone_dir, "boot.json")):
            with open(os.path.join(phone_dir, "boot.json"), encoding="utf-8") as f:
                r = json.load(f)
            fps = f", {r['fps']['median']} fps" if r.get("fps") else ""
            done("móvil", f"{r['ending']} a los {r['seconds']} s, {r['captures']['unique']} capturas distintas{fps}")
        else:
            done("móvil", text.strip().splitlines()[-1] if text.strip() else f"falla (salida {code}, ver logs/phone.log)")
    step(f"Informe: {os.path.join(work, 'report.md')}")


if __name__ == "__main__":
    main()
