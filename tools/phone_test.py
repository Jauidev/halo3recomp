"""The APK on a phone over adb for a few minutes, and how it went (the same report as boot_test.py).

  phone_test.py <slot> <game folder> <out dir> [seconds]

Installs build/apk-<slot>/xport-<slot>.apk, copies the game folder to the app's files/game when the
phone lacks it (the first time only: it is the whole disc; from another copy of the same game on the
phone when there is one, ON_PHONE, else over USB), starts the game without the launcher and
with START pressed on the title screens (XPORT_LAUNCHER=0 XPORT_AUTOPRESS=1 in the intent's env),
takes a screenshot every 10 s (in the second half also each thread's CPU time and the GPU's clock),
and at the end pulls the log (logcat tags xport and xnative) and crash.txt. <out dir> gets
boot_test.py's layout (stdout.log, run/xnative.log, cap/, crash.txt) and its boot.json and boot.md,
with the phone, its frame rate, what limits it (performance()) and the battery temperature before
and after.
"""
import hashlib, os, re, shutil, subprocess, sys, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import boot_test  # noqa: E402

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
ADB = os.path.join(os.environ.get("LOCALAPPDATA", ""), "Android", "Sdk", "platform-tools", "adb.exe")
SHOT_SECONDS = 10
LEVELS = {"E": "[error] ", "F": "[critical] ", "W": "[warning] ", "I": "", "D": "", "V": ""}
COMPLETE = ".xport-copied"
# What limits the frame rate. Threads: CPU time per thread from /proc (utime + stime, in clock
# ticks of 1/100 s) between screenshots in the second half of the run; a thread at SATURATED of a
# core or more holds the game back. GPU: MediaTek closes its load counters to adb, so the GPU's
# clock (devfreq) stands in for it: at its top frequency in GPU_BUSY of the samples, it is at its limit.
TICKS, SATURATED, GPU_BUSY = 100, 85, 0.8
GPU_FREQ = "/sys/class/devfreq/*.mali/cur_freq /sys/class/devfreq/*.gpu/cur_freq /sys/class/kgsl/kgsl-3d0/gpuclk"
GPU_MAX = "/sys/class/devfreq/*.mali/max_freq /sys/class/devfreq/*.gpu/max_freq /sys/class/kgsl/kgsl-3d0/max_gpuclk"
# Where a game may be on the phone already (with the same default.xex it is the same game).
ON_PHONE = ["/data/local/tmp/*/game", "/sdcard/Android/data/com.xport*/files/game"]


def adb(*args, timeout=120):
    result = subprocess.run([ADB, *args], capture_output=True, timeout=timeout)
    return result.returncode, result.stdout


def shell(command, timeout=60):
    return adb("shell", command, timeout=timeout)[1].decode("utf-8", "replace").strip()


def devices():
    if not os.path.exists(ADB):
        return []
    text = adb("devices")[1].decode("utf-8", "replace")
    return [line.split()[0] for line in text.splitlines()[1:] if line.strip().endswith("\tdevice")]


def battery_temperature():
    m = re.search(r"temperature: (\d+)", shell("dumpsys battery"))
    return int(m[1]) / 10 if m else None


def thread_times(pid):
    """{tid: (name, CPU ticks so far, core it last ran on)} of the game's threads."""
    times = {}
    for line in shell(f"cat /proc/{pid}/task/*/stat 2>/dev/null", timeout=30).splitlines():
        m = re.match(r"(\d+) \((.*)\) (.*)", line)
        fields = m[3].split() if m else []
        if len(fields) > 36:  # after the name: state is field 3, utime 14, stime 15, processor 39
            times[int(m[1])] = (m[2], int(fields[11]) + int(fields[12]), int(fields[36]))
    return times


def gpu_clock(paths):
    values = shell(f"cat {paths} 2>/dev/null").split()
    return int(values[0]) if values and values[0].isdigit() else None


def performance(samples, clocks, top):
    """What the CPU and the GPU did in the second half of the run, and which one limits it."""
    if len(samples) < 2:
        return None
    (t0, first), (t1, last) = samples[0], samples[-1]
    threads = []
    for tid, (name, ticks, core) in last.items():
        if tid not in first:
            continue
        steps = [(b[tid][1] - a[tid][1]) / TICKS / (tb - ta) * 100
                 for (ta, a), (tb, b) in zip(samples, samples[1:]) if tid in a and tid in b and tb > ta]
        threads.append({"name": name, "average": round((ticks - first[tid][1]) / TICKS / (t1 - t0) * 100),
                        "peak": round(max(steps, default=0)), "core": core})
    threads.sort(key=lambda t: -t["average"])
    result = {"threads": threads[:8], "total": sum(t["average"] for t in threads)}
    if clocks and top:
        result["gpu"] = {"at_top": round(sum(1 for c in clocks if c >= 0.95 * top) / len(clocks), 2),
                         "top_mhz": top // 1_000_000, "average_mhz": sum(clocks) // len(clocks) // 1_000_000}
    busiest = threads[0] if threads else None
    if result.get("gpu") and result["gpu"]["at_top"] >= GPU_BUSY:
        result["limit"] = (f"la GPU: a su frecuencia máxima el {result['gpu']['at_top']:.0%} del tiempo. "
                           "Bajar la resolución o lo que cuestan los shaders")
    elif busiest and busiest["average"] >= SATURATED:
        result["limit"] = (f"la CPU: el hilo `{busiest['name']}` usa el {busiest['average']}% de su núcleo "
                           f"(el {busiest['core']}). Si es del juego, que vaya a un núcleo grande; si es del "
                           "renderizador, lo que hace por fotograma")
    elif result.get("gpu"):
        result["limit"] = ("ni la CPU ni la GPU están al límite: el juego va a su tope de FPS o espera "
                           "sincronizaciones")
    else:
        result["limit"] = ("ningún hilo está al límite y la frecuencia de la GPU no se pudo leer: "
                           "probablemente la GPU")
    return result


def logcat(tag, raw):
    """The run's lines of one tag; the renderer's with boot_test.py's [error]/[warning] marks."""
    text = adb("logcat", "-d", "-v", "raw" if raw else "tag", "-s", tag, timeout=120)[1].decode("utf-8", "replace")
    lines = []
    for line in text.splitlines():
        if line.startswith("--------- beginning of"):
            continue
        if not raw and (m := re.match(r"([EFWIDV])/[^:]*: ?(.*)", line)):
            line = LEVELS[m[1]] + m[2]
        lines.append(line)
    return "\n".join(lines) + "\n"


def run(slot, game, out, seconds):
    phones = devices()
    if len(phones) != 1:
        raise SystemExit("Conecta un móvil (solo uno) con la depuración USB activada: adb devices "
                         f"ve {len(phones)}.")
    apk = os.path.join(ROOT, "build", f"apk-{slot}", f"xport-{slot}.apk")
    if not os.path.exists(apk):
        raise SystemExit(f"No está el APK: {apk}")
    package = f"com.xport.{slot}"
    files = f"/sdcard/Android/data/{package}/files"
    code, text = adb("install", "-r", apk, timeout=900)
    if code != 0:
        raise SystemExit("adb install falla: " + text.decode("utf-8", "replace").strip())
    # COMPLETE marks a copy that finished: default.xex arrives first, so it says nothing. A game the
    # app's launcher imported (from the ISO) has no mark: the same default.xex and nearly the whole size.
    with open(os.path.join(game, "default.xex"), "rb") as f:
        xex = hashlib.md5(f.read()).hexdigest()
    size = sum(os.path.getsize(os.path.join(d, n)) for d, _, names in os.walk(game) for n in names) // 1024
    imported = shell(f"md5sum {files}/game/default.xex 2>/dev/null").split()[:1] == [xex] and \
        int((shell(f"du -sk {files}/game").split() or ["0"])[0]) >= 0.95 * size
    if not imported and not shell(f"ls {files}/game/{COMPLETE} 2>/dev/null"):
        shell(f"rm -rf {files}/game; mkdir -p {files}")
        # The same game already on the phone (another slot, an adb shell run): copied there.
        on_phone = next((d for d in shell(f"ls -d {' '.join(ON_PHONE)} 2>/dev/null").split()
                         if d != f"{files}/game" and shell(f"md5sum {d}/default.xex 2>/dev/null").split()[:1] == [xex]), None)
        if on_phone:
            print(f"Copiando el juego desde {on_phone}, dentro del móvil...", flush=True)
            shell(f"cp -r {on_phone} {files}/game", timeout=4 * 3600)
        else:
            print(f"Copiando el juego a {files}/game por USB (la primera vez; tarda)...", flush=True)
            code, text = adb("push", game, f"{files}/game", timeout=4 * 3600)
            if code != 0:
                raise SystemExit("adb push falla: " + text.decode("utf-8", "replace").strip()[-400:])
        if not shell(f"ls {files}/game/default.xex 2>/dev/null"):
            raise SystemExit(f"La copia del juego no dejó {files}/game/default.xex")
        shell(f"touch {files}/game/{COMPLETE}")

    cap, run_dir = os.path.join(out, "cap"), os.path.join(out, "run")
    for d in (cap, run_dir):
        shutil.rmtree(d, ignore_errors=True)
        os.makedirs(d)
    for name in ("crash.txt", "stdout.log"):
        if os.path.exists(os.path.join(out, name)):
            os.remove(os.path.join(out, name))
    shell(f"am force-stop {package}; rm -f {files}/crash.txt")
    adb("logcat", "-c")
    device = {"model": shell("getprop ro.product.model"), "soc": shell("getprop ro.soc.model") or shell("getprop ro.hardware"),
              "android": shell("getprop ro.build.version.release"), "temperature_start": battery_temperature()}
    shell(f"am start -n {package}/com.xport.MainActivity --es env 'XPORT_LAUNCHER=0 XPORT_AUTOPRESS=1'")

    started = time.time()
    last_change, last_state, shot, ending = started, None, 0, "running"
    samples, clocks, gpu_top = [], [], gpu_clock(GPU_MAX)
    while time.time() - started < seconds:
        time.sleep(SHOT_SECONDS)
        pid = shell(f"pidof {package}").split()
        if not pid:
            ending = "exited"
            break
        if time.time() - started >= seconds / 2:
            samples.append((time.time(), thread_times(pid[0])))
            if (clock := gpu_clock(GPU_FREQ)) is not None:
                clocks.append(clock)
        png = adb("exec-out", "screencap", "-p", timeout=30)[1]
        if png.startswith(b"\x89PNG"):
            with open(os.path.join(cap, f"{shot:04d}.png"), "wb") as f:
                f.write(png)
            shot += 1
        # Hung: neither the log nor the screen changes for boot_test.HANG_SECONDS.
        size = len(adb("logcat", "-d", "-s", "xport", "xnative", timeout=120)[1])
        state = (size, hashlib.md5(png).hexdigest())
        now = time.time()
        if state != last_state:
            last_change, last_state = now, state
        elif now - last_change >= boot_test.HANG_SECONDS:
            ending = "hung"
            break
    elapsed = round(time.time() - started)
    device["temperature_end"] = battery_temperature()
    device["performance"] = performance(samples, clocks, gpu_top)

    with open(os.path.join(out, "stdout.log"), "w", encoding="utf-8") as f:
        f.write(logcat("xport", raw=True))
    with open(os.path.join(run_dir, "xnative.log"), "w", encoding="utf-8") as f:
        f.write(logcat("xnative", raw=False))
    adb("pull", f"{files}/crash.txt", os.path.join(out, "crash.txt"))
    if ending != "exited":
        shell(f"am force-stop {package}")
    result = boot_test.analyze(out, ending, elapsed, 0, device)
    return result


def main():
    argv = sys.argv[1:]
    sys.stdout.reconfigure(encoding="utf-8")
    sys.stderr.reconfigure(encoding="utf-8")
    if len(argv) not in (3, 4):
        raise SystemExit(__doc__)
    slot, game, out = argv[0], os.path.abspath(argv[1]), os.path.abspath(argv[2])
    r = run(slot, game, out, int(argv[3]) if len(argv) == 4 else 300)
    print(boot_test.markdown(r))


if __name__ == "__main__":
    main()
