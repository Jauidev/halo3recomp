"""Finds code addresses the game reaches only through pointers (vtables, CRT init tables, callbacks,
thread start routines) that XenonRecomp did not emit as functions, so PPC_LOOKUP_FUNC returns null.
Usage: find_fnptrs.py <default.pe> <ppc_func_mapping.cpp>  -> TOML `functions` entries on stdout

Candidates: aligned 32-bit words in .rdata/.data and lis/addi(ori) pairs in code whose value lands
in .text/BINK. Kept when the target looks like an entry: the previous word ends a block (blr, bctr,
unconditional b, padding) or is a .pdata function start. Size runs to the next known function.
"""
import bisect, re, struct, sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script directory out
from pe_layout import code_ranges, sections

BASE = int(__import__("os").environ.get("XPORT_IMAGE_BASE", "0x82000000"), 16)  # Halo 3: its DLLs load elsewhere
pe = open(sys.argv[1], "rb").read()
mapping = sorted(int(a, 16) for a in re.findall(r"\{ 0x([0-9A-F]+), (?:sub_|__imp__)", open(sys.argv[2]).read()))  # __imp__: import thunks
known = set(mapping)

LAYOUT = sections(pe)
CODE = code_ranges(pe)
DATA = [(LAYOUT[n][0], LAYOUT[n][0] + LAYOUT[n][1]) for n in (".rdata", ".data")]


def word(a):
    return struct.unpack_from(">I", pe, a - BASE)[0]


def in_code(a):
    return any(lo <= a < hi for lo, hi in CODE)


pdata = set()
for i in range(LAYOUT[".pdata"][1] // 8):
    begin = struct.unpack_from(">I", pe, LAYOUT[".pdata"][0] - BASE + 8 * i)[0]
    pdata.add(begin)

candidates = {}
for lo, hi in DATA:
    for a in range(lo, hi - 3, 4):
        v = word(a)
        if v & 3 == 0 and in_code(v):
            candidates.setdefault(v, "data")

for lo, hi in CODE:
    w = struct.unpack_from(">%dI" % ((hi - lo) // 4), pe, lo - BASE)
    for i in range(len(w) - 1):
        x = w[i]
        if x >> 26 != 15 or (x >> 16) & 31 != 0:  # lis rD, imm
            continue
        rd = (x >> 21) & 31
        hi16 = (x & 0xFFFF) << 16
        for j in range(i + 1, min(i + 8, len(w))):
            y = w[j]
            op = y >> 26
            if op == 14 and (y >> 16) & 31 == rd:  # addi rX, rD, imm
                imm = y & 0xFFFF
                v = (hi16 + (imm - 0x10000 if imm & 0x8000 else imm)) & 0xFFFFFFFF
            elif op == 24 and (y >> 21) & 31 == rd:  # ori rX, rD, imm
                v = hi16 | (y & 0xFFFF)
            else:
                continue
            if v & 3 == 0 and in_code(v):
                # A label inside the function that computes it is a computed-jump base, not an entry.
                site = lo + 4 * i
                k1 = bisect.bisect_right(mapping, site) - 1
                k2 = bisect.bisect_right(mapping, v) - 1
                if k1 != k2:
                    candidates.setdefault(v, "code")
            break

BLOCK_END = {0x4E800020, 0x4E800420, 0x00000000}
out = []
for target in sorted(candidates):
    if target in known:
        continue
    prev = word(target - 4)
    is_entry = prev in BLOCK_END or (prev >> 26 == 18 and not prev & 1) or target in pdata
    # A leaf function right after a jump table, whose last word is a code address (the title 0x823A61F8).
    if candidates[target] != "code" and any(lo <= prev < hi for lo, hi in CODE):
        is_entry = True
    if not is_entry or word(target) == 0:
        continue
    k = bisect.bisect_right(mapping, target)
    end = mapping[k] if k < len(mapping) else max(hi for _, hi in CODE)
    out.append((target, end - target, candidates[target]))

for target, size, source in out:
    print("    { address = 0x%08X, size = 0x%X }, # %s pointer" % (target, size, source))
print("pointer targets: %d, missing entries: %d" % (len(candidates), len(out)), file=sys.stderr)
