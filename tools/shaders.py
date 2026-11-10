"""Builds a game's native shader cache with XenosRecomp (step 4 of a port).

The native renderer draws only shaders in the game's SPIR-V cache, games/<game>/data/shader/
shader_cache.cpp, looked up by the XXH3 hash of the whole Xenos shader container. XenosRecomp
finds containers inside any file, so the inputs are the extracted disc and/or the containers the
game registered at run time (XPORT_SHADER_DUMP=<dir> writes <dir>/xport_containers): archives
that keep their shaders compressed only give theirs through the dump.

  shaders.py build <game> <input dir>...   writes shader/shader_cache.{cpp,h}
  shaders.py check <game> <dump dir>       the dumped containers the game's cache lacks
  shaders.py empty <game>                  an empty cache and override table (a new slot builds)

Each game also needs data/shader_override_cache.cpp and data/shader_overrides/
shader_override_cache.h (hand-written replacement shaders, by hash); `empty` writes ones with none.
The renderer's ABI header is tools/shaders/xenos_shader_common.h.
"""
import os, re, shutil, subprocess, sys, tempfile

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
SHARED = os.path.join(ROOT, "tools", "shaders")
TOOL = os.path.join(ROOT, "build", "host", "XenosRecomp", "XenosRecomp.exe")
ENTRY = re.compile(r'^\t\{ 0x([0-9A-F]+), ')
MAX_FILE = 256 << 20

EMPTY_CACHE = """#include "shader_cache.h"
// No shaders yet: tools/shaders.py build <game> <inputs>.
ShaderCacheEntry g_shaderCacheEntries[1] = {};
const uint8_t g_compressedSpirvCache[1] = {};
const size_t g_spirvCacheCompressedSize = 0;
const size_t g_spirvCacheDecompressedSize = 0;
const size_t g_shaderCacheEntryCount = 0;
"""
EMPTY_OVERRIDES = """#include <shader_overrides/shader_override_cache.h>
// No replacement shaders (games/gta4/tools/gta4_shader_override_compiler.py writes GTA IV's).
const ShaderOverrideCacheEntry g_shaderOverrideEntries[1] = {};
const size_t g_shaderOverrideEntryCount = 0;
"""


def data_dir(game):
    path = os.path.join(ROOT, "games", game, "data")
    if not os.path.isdir(os.path.join(ROOT, "games", game)):
        raise SystemExit(f"no games/{game}: make the slot first (tools/xport.py)")
    return path


def write(path, text):
    os.makedirs(os.path.dirname(path), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        f.write(text)


def install_headers(data):
    for name, sub in (("shader_cache.h", "shader"), ("shader_override_cache.h", "shader_overrides")):
        target = os.path.join(data, sub, name)
        if not os.path.exists(target):
            os.makedirs(os.path.dirname(target), exist_ok=True)
            shutil.copyfile(os.path.join(SHARED, name), target)


def empty(game):
    data = data_dir(game)
    install_headers(data)
    write(os.path.join(data, "shader", "shader_cache.cpp"), EMPTY_CACHE)
    overrides = os.path.join(data, "shader_override_cache.cpp")
    if not os.path.exists(overrides):
        write(overrides, EMPTY_OVERRIDES)
    print(f"games/{game}/data: empty shader cache")


def title_id(game):
    """XPORT_TITLE_ID from games/<game>/game.cmake ("" if it has none)."""
    try:
        with open(os.path.join(ROOT, "games", game, "game.cmake"), encoding="utf-8") as f:
            m = re.search(r"set\(XPORT_TITLE_ID\s+(0x[0-9A-Fa-f]+)\)", f.read())
            return m[1] if m else ""
    except OSError:
        return ""


def ensure_tool(game):
    """XenosRecomp built for the game's title id. shader_recompiler.cpp has title-gated paths (Halo 3:
    256 pixel constants and jump flags) that the game's runtime translator gets from its build; a
    cache made by a tool built for another title holds different, wrong translations of them."""
    title = title_id(game)
    stamp = os.path.join(os.path.dirname(TOOL), "title_id.txt")
    built = open(stamp, encoding="utf-8").read().strip() if os.path.exists(stamp) else None
    # Also when its sources changed after it was built: a cache made by an older tool lacks the
    # translator's later fixes (Halo 3's six clip planes were left out that way, 2026-11-06).
    sources = os.path.join(ROOT, "third_party", "rexglue", "thirdparty", "XenosRecomp", "XenosRecomp")
    newest = max((os.path.getmtime(os.path.join(sources, name)) for name in os.listdir(sources)
                  if name.endswith((".cpp", ".h"))), default=0) if os.path.isdir(sources) else 0
    stale = os.path.exists(TOOL) and os.path.getmtime(TOOL) < newest
    if not os.path.exists(TOOL) or built != title or stale:
        print(f"building XenosRecomp for title {title or '(none)'} (tools/build-xenosrecomp.bat)")
        # vcvars64.bat needs vswhere.exe, which a Git Bash PATH may lack.
        installer = os.path.join(os.environ.get("ProgramFiles(x86)", r"C:\Program Files (x86)"),
                                 "Microsoft Visual Studio", "Installer")
        env = dict(os.environ, XPORT_TITLE_ID=title)
        env["PATH"] = installer + os.pathsep + env.get("PATH", "")
        subprocess.run(["cmd", "/c", os.path.join(ROOT, "tools", "build-xenosrecomp.bat")], check=True,
                       env=env)


def stage_inputs(inputs, stage):
    """One folder for XenosRecomp: hard links to the inputs' files (copies across drives).
    XenosRecomp reads each file whole and keeps the ones with shaders in memory, so files over
    MAX_FILE are left out: shaders inside big archives come from the run-time dump instead."""
    count, skipped = 0, []
    for k, top in enumerate(inputs):
        for folder, _, files in os.walk(top):
            rel = os.path.relpath(folder, top)
            target_dir = os.path.join(stage, f"in{k}", rel)
            os.makedirs(target_dir, exist_ok=True)
            for name in files:
                source, target = os.path.join(folder, name), os.path.join(target_dir, name)
                if os.path.getsize(source) > MAX_FILE:
                    skipped.append(os.path.relpath(source, top))
                    continue
                try:
                    os.link(source, target)
                except OSError:
                    shutil.copyfile(source, target)
                count += 1
    if skipped:
        print(f"{len(skipped)} files over {MAX_FILE >> 20} MB left out: " + ", ".join(skipped[:8]))
    return count


def cache_hashes(path):
    hashes = set()
    if os.path.exists(path):
        with open(path, encoding="utf-8", errors="replace") as f:
            for line in f:
                m = ENTRY.match(line)
                if m:
                    hashes.add(int(m[1], 16))
                elif line.startswith("const uint8_t"):
                    break
    return hashes


def build(game, inputs):
    data = data_dir(game)
    ensure_tool(game)
    install_headers(data)
    out = os.path.join(data, "shader", "shader_cache.cpp")
    with tempfile.TemporaryDirectory(prefix="xport-shaders-") as stage:
        files = stage_inputs(inputs, stage)
        print(f"{files} files from {len(inputs)} input folder(s)")
        temp_out = os.path.join(stage, "shader_cache.cpp")
        run = subprocess.run([TOOL, stage, temp_out, os.path.join(SHARED, "xenos_shader_common.h")],
                             cwd=os.path.dirname(TOOL), capture_output=True, text=True, errors="replace")
        log = run.stdout + run.stderr
        with open(os.path.join(data, "shader", "xenosrecomp.log"), "w", encoding="utf-8") as f:
            f.write(log)
        failed = [l for l in log.splitlines() if l.startswith("error:")]
        if run.returncode or not os.path.exists(temp_out):
            raise SystemExit(f"XenosRecomp failed ({run.returncode}): see games/{game}/data/shader/xenosrecomp.log")
        count = len(cache_hashes(temp_out))
        if not count:
            print("no shader containers found in the inputs: the cache stays empty")
            empty(game)
            return {"shaders": 0, "failed": len(failed)}
        shutil.move(temp_out, out)
    overrides = os.path.join(data, "shader_override_cache.cpp")
    if not os.path.exists(overrides):
        write(overrides, EMPTY_OVERRIDES)
    print(f"games/{game}/data/shader/shader_cache.cpp: {count} shaders, {len(failed)} DXC failures")
    for line in failed[:20]:
        print(" ", line)
    return {"shaders": count, "failed": len(failed)}


def check(game, dump):
    cached = cache_hashes(os.path.join(data_dir(game), "shader", "shader_cache.cpp"))
    folder = os.path.join(dump, "xport_containers") if os.path.isdir(os.path.join(dump, "xport_containers")) else dump
    dumped = {}
    for name in os.listdir(folder):
        m = re.match(r"xport_([0-9A-F]{16})\.container\.(vert|frag)$", name)
        if m:
            dumped[int(m[1], 16)] = m[2]
    missing = sorted(h for h in dumped if h not in cached)
    print(f"{len(dumped)} containers dumped, {len(dumped) - len(missing)} in the cache, {len(missing)} missing")
    for h in missing[:40]:
        print(f"  {h:016X} {dumped[h]}")
    return {"dumped": len(dumped), "missing": [f"{h:016X}" for h in missing]}


def main():
    a = sys.argv[1:]
    if len(a) >= 3 and a[0] == "build":
        build(a[1], a[2:])
    elif len(a) == 3 and a[0] == "check":
        check(a[1], a[2])
    elif len(a) == 2 and a[0] == "empty":
        empty(a[1])
    else:
        raise SystemExit(__doc__)


if __name__ == "__main__":
    main()
