"""Puts title DLLs recompiled with tools/xport.py next to a game's code (Halo 3 loads Waves' audio
DLLs with XexLoadImage: WaveShell-Xbox.dll, WavesLibDLL.dll, waves/L360.dll, waves/Q10.dll; the game
asserts without them).

  slot_dlls.py <slot> <dll work dir> [<dll work dir> ...]

Each work dir (an xport.py output for one DLL XEX) becomes games/<slot>/dlls/<name>/, <name> being
the folder's name:
  - its ppc/ files, rewritten only when they change, with the game's own ppc_config.h (indirect
    calls go through the game's function table, which the DLLs' addresses fit in: the whole guest
    space is mapped) and its mapping renamed PPCFuncMappings_<name>;
Then, for all of them together:
  - dlls/modules.inc (included by the slot's game.cpp): the mapping tables, by load address, and
    the __imp__<Library>_<ordinal> functions a DLL imports from another one, each calling the
    exporting DLL's function (its XEX export table gives the address);
  - dlls/dlls.cmake: XPORT_EXTRA_PPC_DIRS, the folders CMakeLists.txt builds one library from each;
  - runtime-imports.txt gains the kernel/XAM functions the DLLs import, for tools/gen_stubs.py.
The runtime (runtime/loader.cpp) loads a DLL whose entry point has a recompiled function: it maps
the XEX, binds its imports, runs its entry point and answers XexGetProcedureAddress from the XEX
export table.
"""
import filecmp, os, re, shutil, struct, sys

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))


def sync_text(path, text):
    """Writes the file only when its content changes (ninja rebuilds what is touched)."""
    if os.path.exists(path):
        with open(path, encoding="utf-8", errors="replace", newline="") as f:
            if f.read() == text:
                return False
    with open(path, "w", encoding="utf-8", newline="") as f:
        f.write(text)
    return True


def sync_file(src, dst):
    if os.path.exists(dst) and filecmp.cmp(src, dst, shallow=False):
        return False
    shutil.copyfile(src, dst)
    return True


def xex_exports(xex, pe, base):
    """ordinal -> address, from the XEX export table (security info +0x160, an image address)."""
    security = struct.unpack_from(">I", xex, 0x10)[0]
    table = struct.unpack_from(">I", xex, security + 0x160)[0]
    if not table:
        return {}
    at = table - base
    if struct.unpack_from(">3I", pe, at) != (0x48000000, 0x00485645, 0x48000000):
        raise SystemExit(f"export table at 0x{table:08X}: unexpected magic")
    image_base, count, first = struct.unpack_from(">3I", pe, at + 32)
    offsets = struct.unpack_from(f">{count}I", pe, at + 44)
    return {first + i: (image_base << 16) + v for i, v in enumerate(offsets) if v}


def main():
    if len(sys.argv) < 3:
        raise SystemExit(__doc__)
    slot = os.path.join(ROOT, "games", sys.argv[1])
    game_config = os.path.join(slot, "ppc", "ppc_config.h")
    if not os.path.exists(game_config):
        raise SystemExit(f"{game_config}: make the game's slot first (tools/slot.py)")
    out = os.path.join(slot, "dlls")
    os.makedirs(out, exist_ok=True)

    dlls = []
    for work in sys.argv[2:]:
        name = os.path.basename(os.path.normpath(work))
        if not re.fullmatch(r"[A-Za-z0-9_]+", name):
            raise SystemExit(f"{work}: the folder's name ({name}) names C++ symbols: letters, digits, _")
        info = open(os.path.join(work, "xexinfo.txt"), encoding="utf-8", errors="replace").read()
        base = int(re.search(r"^load_address = (0x[0-9A-Fa-f]+)", info, re.M)[1], 16)
        pe_name = re.search(r"^original_pe_name = (.+)$", info, re.M)[1].strip()
        xex = open(os.path.join(work, "default.xex"), "rb").read()
        pe = open(os.path.join(work, "default.pe"), "rb").read()
        ppc = os.path.join(work, "ppc")
        mapping = open(os.path.join(ppc, "ppc_func_mapping.cpp"), encoding="utf-8").read()
        # Functions named from symbols (_xstart, __savegprlr_14, __restfpr_20, ...) carry the same
        # names in the game and in every DLL, and XenonRecomp emits them weak: the linker kept
        # one (the game's), so WaveShell's entry point ran the game's _xstart and started the
        # game again. Each DLL's get its folder's name appended; __imp__ imports stay shared.
        named = sorted({n for n in re.findall(r"\{ 0x[0-9A-F]+, ([A-Za-z_]\w*) \}", mapping)
                        if not n.startswith("sub_") and not n.startswith("__imp__")}, key=len, reverse=True)
        rename_re = re.compile(r"(?<![A-Za-z0-9_])(__imp__)?(" + "|".join(map(re.escape, named)) + r")(?![A-Za-z0-9_])") if named else None

        def rename(text, suffix=name):
            return rename_re.sub(lambda m: (m[1] or "") + m[2] + "_" + suffix, text) if rename_re else text

        mapping = rename(mapping)
        functions = {int(a, 16): n for a, n in re.findall(r"\{ 0x([0-9A-F]+), ([A-Za-z_]\w*) \}", mapping)
                     if not n.startswith("__imp__")}
        imports = re.findall(r"^(\S+) (func|var)\s+0x([0-9A-F]+) (\S+)", info, re.M)
        dlls.append({"name": name, "base": base, "pe_name": pe_name, "exports": xex_exports(xex, pe, base),
                     "functions": functions, "imports": imports, "ppc": ppc, "mapping": mapping})

        # The DLL's code, with the game's config and its own mapping name.
        dst = os.path.join(out, name)
        os.makedirs(dst, exist_ok=True)
        wanted, changed = set(), 0
        for entry in sorted(os.listdir(ppc)):
            wanted.add(entry)
            src, target = os.path.join(ppc, entry), os.path.join(dst, entry)
            if entry == "ppc_config.h":
                changed += sync_file(game_config, target)
            elif entry == "ppc_func_mapping.cpp":
                text = mapping.replace("PPCFuncMapping PPCFuncMappings[]", f"PPCFuncMapping PPCFuncMappings_{name}[]")
                changed += sync_text(target, text)
            elif entry.endswith((".cpp", ".h")) and rename_re:
                with open(src, encoding="utf-8", newline="") as f:
                    changed += sync_text(target, rename(f.read()))
            else:
                changed += sync_file(src, target)
        for entry in os.listdir(dst):
            if entry not in wanted:
                os.remove(os.path.join(dst, entry))
        print(f"{name}: {pe_name} at 0x{base:08X}, {len(functions)} functions, "
              f"{len(dlls[-1]['exports'])} exports, {changed} files written", file=sys.stderr)

    # Imports from another DLL of the title: forwarders to its export.
    by_stem = {}
    for d in dlls:
        stem = re.sub(r"[^A-Za-z0-9]", "_", d["pe_name"].split(".")[0])
        by_stem[stem] = d
    lines = ["// Generated by tools/slot_dlls.py: the title DLLs recompiled into this port (runtime/loader.cpp",
             "// loads them when the game asks), and the functions they import from each other.", ""]
    for d in dlls:
        lines.append(f"extern PPCFuncMapping PPCFuncMappings_{d['name']}[];")
    lines += ["", "struct XportTitleDll", "{", "    const char* name;", "    uint32_t base;",
              "    const PPCFuncMapping* functions;", "};", "", "static const XportTitleDll kTitleDlls[] =", "{"]
    for d in dlls:
        lines.append(f"    {{ \"{d['pe_name']}\", 0x{d['base']:08X}, PPCFuncMappings_{d['name']} }},")
    lines += ["};", ""]
    forwarded, missing = set(), []
    for d in dlls:
        for name in sorted(set(re.findall(r"\b(__imp__([A-Za-z0-9_]+?)_(\d+))\b", d["mapping"]))):
            symbol, stem, ordinal = name[0], name[1], int(name[2])
            if symbol in forwarded or stem not in by_stem:
                continue
            target = by_stem[stem]
            address = target["exports"].get(ordinal)
            forwarded.add(symbol)
            if address is None:
                missing.append(symbol)
                lines.append(f"PPC_FUNC({symbol}) {{ LOGF_ERROR(\"{target['pe_name']} exports no ordinal {ordinal}\"); }}")
            elif address in target["functions"]:
                lines.append(f"PPC_EXTERN_FUNC({target['functions'][address]});")
                lines.append(f"PPC_FUNC({symbol}) {{ {target['functions'][address]}(ctx, base); }}")
            else:
                lines.append(f"PPC_FUNC({symbol}) {{ PPC_CALL_INDIRECT_FUNC(0x{address:08X}u); }}")
    sync_text(os.path.join(out, "modules.inc"), "\n".join(lines) + "\n")
    print(f"forwarders: {len(forwarded)}, to missing exports: {len(missing)}", file=sys.stderr)

    folders = " ".join(f"${{XPORT_GAME_ROOT}}/dlls/{d['name']}" for d in dlls)
    sync_text(os.path.join(out, "dlls.cmake"),
              "# Generated by tools/slot_dlls.py: the title DLLs' recompiled code, one library each (CMakeLists.txt).\n"
              f"set(XPORT_EXTRA_PPC_DIRS {folders})\n")

    # Their kernel/XAM functions, for the stubs of the ones the runtime lacks.
    listing = os.path.join(slot, "runtime-imports.txt")
    have = [l.strip() for l in open(listing)] if os.path.exists(listing) else []
    added = []
    for d in dlls:
        for library, kind, _, symbol in d["imports"]:
            name = symbol[len("__imp__"):] if symbol.startswith("__imp__") else symbol
            if kind == "func" and library in ("xboxkrnl.exe", "xam.xex") and name not in have and name not in added:
                added.append(name)
    if added:
        with open(listing, "a") as f:
            f.write("".join(n + "\n" for n in added))
    print(f"runtime-imports.txt: {len(added)} more ({', '.join(added) or 'none'})", file=sys.stderr)


if __name__ == "__main__":
    main()
