"""Boots a built game on the PC for a few minutes and says how it went (step 6 of a port).

  boot_test.py <game exe> <game folder> <out dir> [seconds] [--gpu native|xenia] [--dump-shaders]
  boot_test.py --analyze <out dir>     the report of a run already made, again

The exe and its DLLs are copied into <out dir>/run and started there with the game folder, a save
folder, frame captures (XNATIVE_CAPTURE, or XPORT_XENIA_CAPTURE with Xenia's GPU) and START pressed for the title
screens (XPORT_AUTOPRESS). With --dump-shaders every shader container the game registers lands in
<out dir>/shaders (tools/shaders.py build <game> <out dir>/shaders).

Writes <out dir>/boot.json and boot.md: how it ended (still running, exited, crashed, hung), the
crash report, how many captures differ and which are blank, the log's errors grouped, and the
imports the game called that the runtime only stubs ("not implemented").
"""
import hashlib, json, os, re, shutil, subprocess, sys, time, zlib
from collections import Counter

HANG_SECONDS = 90  # no new log line and no new capture for this long: hung
EXCEPTIONS = {
    0xC0000005: "access violation", 0xC000001D: "illegal instruction", 0xC00000FD: "stack overflow",
    0xC0000409: "fail fast / abort()", 0x80000003: "breakpoint", 0xC0000094: "integer divide by zero",
    0xC0000374: "heap corruption", 0xE06D7363: "uncaught C++ exception", 0xC0000135: "missing DLL",
    0xC0000142: "DLL failed to initialize",
}


def png_info(path):
    """(width, height, uniform, black): uniform when the image data compresses to almost nothing."""
    with open(path, "rb") as f:
        data = f.read()
    if data[:8] != b"\x89PNG\r\n\x1a\n":
        return None
    pos, idat, width = 8, [], 0
    while pos + 8 <= len(data):
        length, kind = int.from_bytes(data[pos:pos + 4], "big"), data[pos + 4:pos + 8]
        body = data[pos + 8:pos + 8 + length]
        if kind == b"IHDR":
            width, height, depth, color = int.from_bytes(body[:4], "big"), int.from_bytes(body[4:8], "big"), body[8], body[9]
        elif kind == b"IDAT":
            idat.append(body)
        pos += 12 + length
    if not width or depth != 8 or color not in (2, 6):
        return {"width": width, "uniform": None, "black": None}
    # Filtered rows of a flat image are almost all zeros (the capture writer compresses too weakly
    # to tell by size: a black 1280x720 frame takes 37 KB).
    raw, bpp = zlib.decompress(b"".join(idat)), 4 if color == 6 else 3
    uniform = len(raw) - raw.count(0) < len(raw) // 100
    black = False
    if uniform:
        # The first row, unfiltered, gives the colour.
        stride = width * bpp
        previous = bytearray(stride)
        for y in range(1):
            line = raw[y * (stride + 1):(y + 1) * (stride + 1)]
            kind, row = line[0], bytearray(line[1:])
            for i in range(stride):
                a = row[i - bpp] if i >= bpp else 0
                b = previous[i]
                c = previous[i - bpp] if i >= bpp else 0
                if kind == 1:
                    row[i] = (row[i] + a) & 255
                elif kind == 2:
                    row[i] = (row[i] + b) & 255
                elif kind == 3:
                    row[i] = (row[i] + (a + b) // 2) & 255
                elif kind == 4:
                    p = a + b - c
                    pa, pb, pc = abs(p - a), abs(p - b), abs(p - c)
                    row[i] = (row[i] + (a if pa <= pb and pa <= pc else b if pb <= pc else c)) & 255
            previous = row
        black = max(v for i, v in enumerate(previous) if color != 6 or i % 4 != 3) < 16
    return {"width": width, "height": height, "uniform": uniform, "black": black}


def normalize(line):
    """A log line without its timestamp, thread and numbers, to group repeats."""
    line = re.sub(r"^\[[^]]*\] ", "", line)
    line = re.sub(r"\[t\d+\] ", "", line)
    line = re.sub(r"\b\d+x\d+\b|\b0x[0-9A-Fa-f]+|\b[0-9A-F]{6,16}\b|\b\d+\b", "#", line)
    return line.strip()[:100]


def read_text(path):
    try:
        with open(path, encoding="utf-8", errors="replace") as f:
            return f.read()
    except OSError:
        return ""


def boot(exe, game_dir, out, seconds=300, gpu="native", dump_shaders=False):
    run_dir, save, cap = os.path.join(out, "run"), os.path.join(out, "save"), os.path.join(out, "cap")
    translated = os.path.join(out, "translated")
    for d in (run_dir, cap, translated):
        shutil.rmtree(d, ignore_errors=True)
    for d in (run_dir, save, cap):
        os.makedirs(d, exist_ok=True)
    for name in os.listdir(os.path.dirname(exe)):
        if name.lower().endswith(".dll") or name == os.path.basename(exe):
            shutil.copy2(os.path.join(os.path.dirname(exe), name), run_dir)
    crash_file = os.path.join(out, "crash.txt")
    if os.path.exists(crash_file):
        os.remove(crash_file)
    # Shaders translated while the game runs (the cache lacks them) go to <out>/translated, empty
    # on every run, so the test starts from the cache alone.
    env = dict(os.environ, XPORT_GAME_DIR=game_dir, XPORT_SAVE_DIR=save, XPORT_GPU=gpu, XPORT_AUTOPRESS="1",
               XNATIVE_SHADER_CACHE_DIR=translated)
    # Xenia's capture too when asked for native: a game built without the native renderer
    # (D3D functions missing) draws with Xenia's GPU anyway. Only the GPU that runs writes.
    env["XPORT_XENIA_CAPTURE"] = cap
    if gpu == "native":
        env["XNATIVE_CAPTURE"] = cap
    if dump_shaders:
        env["XPORT_SHADER_DUMP"] = os.path.join(out, "shaders")
    # Where each guest thread is shortly before the end (runtime/os/thread_dump.cpp): what a game
    # that stopped presenting waits for.
    env.setdefault("XPORT_DUMP_AFTER", str(max(10, seconds - 20)))
    log_path = os.path.join(run_dir, "xnative.log")
    stdout_path = os.path.join(out, "stdout.log")
    started = time.time()
    last_change, last_size, last_caps = started, -1, -1
    ending = "running"
    with open(stdout_path, "wb") as stdout:
        process = subprocess.Popen([os.path.join(run_dir, os.path.basename(exe))], cwd=run_dir, env=env,
                                   stdout=stdout, stderr=subprocess.STDOUT)
        while True:
            try:
                code = process.wait(timeout=5)
                ending = "exited"
                break
            except subprocess.TimeoutExpired:
                pass
            now = time.time()
            size = sum(os.path.getsize(p) for p in (log_path, stdout_path) if os.path.exists(p))
            caps = len(os.listdir(cap))
            if size != last_size or caps != last_caps:
                last_change, last_size, last_caps = now, size, caps
            if now - started >= seconds:
                break
            if now - last_change >= HANG_SECONDS:
                ending = "hung"
                break
        if ending != "exited":
            process.kill()
            code = process.wait()
    return analyze(out, ending, round(time.time() - started), code)


def analyze(out, ending, seconds, code, device=None):
    """What a finished run in <out dir> left: boot.json and boot.md. device: the phone's details
    (tools/phone_test.py), None on the PC."""
    cap = os.path.join(out, "cap")
    log = read_text(os.path.join(out, "run", "xnative.log"))
    output = read_text(os.path.join(out, "stdout.log"))
    crash = read_text(os.path.join(out, "crash.txt")) or "\n".join(l for l in output.splitlines() if "*** crash" in l)
    if ending == "exited":
        unsigned = code & 0xFFFFFFFF
        if crash or unsigned >= 0x80000000:
            ending = "crashed"
    result = {"ending": ending, "seconds": seconds, "exit_code": f"0x{code & 0xFFFFFFFF:08X}" if code else "0",
              "exception": EXCEPTIONS.get(code & 0xFFFFFFFF) if code else None}
    first = re.search(r"\*\*\* crash: (.+)", crash)
    result["crash"] = first[1] if first else None
    result["crash_report"] = crash.strip().splitlines()[:40]

    captures = sorted(os.listdir(cap)) if os.path.isdir(cap) else []
    hashes, blank, black, seen = Counter(), [], [], {}
    for name in captures:
        path = os.path.join(cap, name)
        with open(path, "rb") as f:
            digest = hashlib.md5(f.read()).hexdigest()
        hashes[digest] += 1
        if digest not in seen:
            seen[digest] = png_info(path) if name.endswith(".png") else None
        info = seen[digest]
        if info and info["uniform"]:
            (black if info["black"] else blank).append(name)
    result["captures"] = {"count": len(captures), "unique": len(hashes), "black": len(black), "uniform": len(blank),
                          "first_black": black[:5], "last": captures[-1] if captures else None}

    errors = Counter(normalize(l) for l in log.splitlines() if re.search(r"\[(error|critical)\]", l))
    warnings = Counter(normalize(l) for l in log.splitlines() if "[warning]" in l)
    stubs = Counter(re.findall(r"Unimplemented_(\w+): not implemented", output + log))
    result["errors"] = errors.most_common(25)
    result["warnings"] = warnings.most_common(10)
    result["stub_calls"] = sorted(stubs)
    # Draws the renderer refused: the log is sampled, so the highest #N of each kind is the total.
    rejected = {}
    for count, reason in re.findall(r"xnative-diag: draw(?:-up|-indexed)? failure #(\d+) reason=([\w-]+)", log):
        rejected[reason] = max(rejected.get(reason, 0), int(count))
    result["draws_rejected"] = rejected
    result["pipeline_rejects"] = Counter(re.findall(r"xnative-diag: pipeline reject #\d+ reason=([\w-]+)", log)).most_common(8)
    result["shader_cache_misses"] = len(set(re.findall(r"shader cache miss for \w+ shader ([0-9A-F]{16})", log)))
    result["shaders_translated"] = len(re.findall(r"xnative: translated \w+ shader", log))
    result["translation_failures"] = len(re.findall(r"runtime shader translation failed|DXC rejected runtime", log))
    result["translator_missing"] = "runtime shader translator unavailable" in log
    folder = os.path.join(out, "shaders", "xport_containers")
    if os.path.isdir(folder):
        result["shaders_dumped"] = len([n for n in os.listdir(folder) if ".container." in n])
    vblank = [l for l in output.splitlines() if l.startswith("VblankThread:")]
    result["last_status"] = vblank[-3:]
    result["fps"] = frame_rate(vblank)
    dump = re.search(r"\*\*\* thread dump after (\d+) s: (\d+) guest threads", output)
    result["thread_dump"] = {"seconds": int(dump[1]), "threads": int(dump[2])} if dump else None
    if device:
        result["device"] = device

    with open(os.path.join(out, "boot.json"), "w", encoding="utf-8") as f:
        json.dump(result, f, indent=1)
    with open(os.path.join(out, "boot.md"), "w", encoding="utf-8") as f:
        f.write(markdown(result))
    return result


def frame_rate(vblank):
    """Frames the game presented per second over the second half of the run, as its status lines
    say: their median and their worst tenth. On the PC the GPU is not the limit, so this is how fast
    the game's CPU side runs. (Not from the interrupt count: it goes up twice per vblank.)"""
    rates = [float(m[1]) for line in vblank if (m := re.search(r"swaps \(([\d.]+) fps\)", line))]
    rates = rates[len(rates) // 2:]
    if not rates:
        return None
    rates.sort()
    return {"median": round(rates[len(rates) // 2], 1), "low": round(rates[len(rates) // 10], 1)}


def verdict(r):
    if r["ending"] == "running":
        if r["captures"]["unique"] > 1 and r["captures"]["black"] < r["captures"]["count"]:
            return f"Sigue en marcha a los {r['seconds']} s y la imagen cambia."
        return f"Sigue en marcha a los {r['seconds']} s, pero sin imagen que cambie (pantalla negra o fija)."
    if r["ending"] == "hung":
        return f"Colgado: ni log ni capturas nuevas en {HANG_SECONDS} s (a los {r['seconds']} s)."
    if r["ending"] == "crashed":
        return f"Se cae a los {r['seconds']} s: {r['crash'] or r['exception'] or r['exit_code']}."
    return f"Termina solo a los {r['seconds']} s con código {r['exit_code']}."


def markdown(r):
    c, device = r["captures"], r.get("device")
    lines = ["## Prueba en el móvil" if device else "## Prueba de arranque", "", f"**{verdict(r)}**", ""]
    if device:
        lines.append(f"- {device['model']} ({device['soc']}, Android {device['android']}); batería a "
                     f"{device['temperature_start']} °C al empezar y {device['temperature_end']} °C al acabar")
    lines.append(f"- Capturas: {c['count']}, distintas {c['unique']}, negras {c['black']}, de un solo color {c['uniform']}")
    if r.get("shaders_translated") or r.get("translation_failures"):
        lines.append(f"- Shaders traducidos durante la partida (no estaban en la caché): {r['shaders_translated']}"
                     + (f", {r['translation_failures']} que no se pudieron traducir" if r["translation_failures"] else ""))
    if r.get("translator_missing"):
        lines.append("- El traductor de shaders no cargó DXC (dxcompiler.dll / libdxcompiler.so)")
    if r.get("shader_cache_misses"):
        lines.append(f"- Shaders que no se dibujan (ni en la caché ni traducidos): {r['shader_cache_misses']}")
    if r.get("draws_rejected"):
        reasons = ", ".join(f"{n} por `{reason}`" for reason, n in r["draws_rejected"].items())
        lines.append(f"- **Dibujos que el renderizador rechaza (no se ven): {reasons}**")
        if r.get("pipeline_rejects"):
            lines.append("  - Pipelines: " + ", ".join(f"`{reason}`" for reason, _ in r["pipeline_rejects"]))
    if r.get("fps") and device:
        lines.append(f"- Fotogramas por segundo (segunda mitad): mediana {r['fps']['median']}, el peor 10% {r['fps']['low']}")
    elif r.get("fps"):
        lines.append(f"- Fotogramas por segundo en el PC (segunda mitad): mediana {r['fps']['median']}, "
                     f"el peor 10% {r['fps']['low']}. En el PC limita la CPU del juego; en el móvil, divide entre 3-4")
    if "shaders_dumped" in r:
        lines.append(f"- Shaders volcados: {r['shaders_dumped']}")
    if r["stub_calls"]:
        lines.append(f"- Imports llamados que el runtime solo tiene como stub ({len(r['stub_calls'])}): "
                     + ", ".join(f"`{n}`" for n in r["stub_calls"]))
    perf = (device or {}).get("performance")
    if perf:
        lines += ["", "### Rendimiento (segunda mitad)", "", f"- Lo limita {perf['limit']}"]
        if perf.get("gpu"):
            g = perf["gpu"]
            lines.append(f"- GPU: a su frecuencia máxima ({g['top_mhz']} MHz) el {g['at_top']:.0%} del tiempo, "
                         f"{g['average_mhz']} MHz de media")
        lines.append(f"- CPU: el juego usa en total el {perf['total']}% de un núcleo; los hilos que más "
                     "(media / pico, % de un núcleo, núcleo donde corría):")
        lines += [f"  - `{t['name']}`: {t['average']} / {t['peak']}, núcleo {t['core']}" for t in perf["threads"]]
        lines.append("")
    if r.get("thread_dump"):
        d = r["thread_dump"]
        lines.append(f"- Volcado de los {d['threads']} hilos del juego a los {d['seconds']} s (dónde espera cada uno): "
                     "`*** thread dump` en `boot/stdout.log`")
    if r["last_status"]:
        lines += ["- Último estado:", "", "```"] + r["last_status"] + ["```"]
    if r["crash_report"]:
        lines += ["", "### Informe del fallo", "", "```"] + r["crash_report"] + ["```"]
    if r["errors"]:
        lines += ["", "### Errores del log (agrupados)", ""] + [f"- ×{n} `{e}`" for e, n in r["errors"]]
    return "\n".join(lines) + "\n"


def main():
    if len(sys.argv) == 3 and sys.argv[1] == "--analyze":
        # The report of a run already made, again (after a change to this script).
        out = os.path.abspath(sys.argv[2])
        with open(os.path.join(out, "boot.json"), encoding="utf-8") as f:
            old = json.load(f)
        r = analyze(out, old["ending"] if old["ending"] != "crashed" else "exited", old["seconds"], int(old["exit_code"], 16),
                    old.get("device"))
        sys.stdout.reconfigure(encoding="utf-8")
        print(markdown(r))
        return
    args = [a for a in sys.argv[1:] if not a.startswith("--")]
    flags = [a for a in sys.argv[1:] if a.startswith("--")]
    gpu = "native"
    for i, a in enumerate(sys.argv):
        if a == "--gpu" and i + 1 < len(sys.argv):
            gpu = sys.argv[i + 1]
            args.remove(gpu)
    if len(args) not in (3, 4):
        raise SystemExit(__doc__)
    exe, game_dir, out = (os.path.abspath(a) for a in args[:3])
    r = boot(exe, game_dir, out, int(args[3]) if len(args) == 4 else 300, gpu, "--dump-shaders" in flags)
    sys.stdout.reconfigure(encoding="utf-8")
    print(markdown(r))


if __name__ == "__main__":
    main()
