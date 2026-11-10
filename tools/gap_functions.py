"""Leaf functions in the gaps between .pdata functions, with their real extent.
A leaf function (no stack frame) has no .pdata entry, so XenonAnalyse finds it by scanning the gap and
cuts it at its first return. Halo 3 has big leaf functions with several returns and jump tables (a
character-class test at 0x821AB630: four tables, cases shared between them): cut that way, each table
lands in another "function", fix_switch.py then declares functions that start on table data, and the
rounds never settle (521 switch errors, the function's code duplicated).
Here each gap is split by reachability instead: a function starts at the first non-zero word, runs over
everything its branches and its jump tables (switch_tables.toml, trimmed) reach inside the gap, and the
next one starts after it. Branches out of the gap are tail calls. Prints TOML `functions` entries.
Usage: gap_functions.py <default.pe> <switch_tables.toml>
"""
import re, struct, sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script directory out
from pe_layout import code_ranges, sections

BASE = int(__import__("os").environ.get("XPORT_IMAGE_BASE", "0x82000000"), 16)  # Halo 3: its DLLs load elsewhere
pe = open(sys.argv[1], "rb").read()
CODE = code_ranges(pe)
LAYOUT = sections(pe)


def word(a):
    return struct.unpack_from(">I", pe, a - BASE)[0]


def sext(v, bits):
    return v - (1 << bits) if v & (1 << (bits - 1)) else v


# .pdata functions (any code section)
pb, ps = LAYOUT[".pdata"]
pdata = {}
for i in range(ps // 8):
    begin, packed = struct.unpack_from(">II", pe, pb - BASE + 8 * i)
    if any(lo <= begin < hi for lo, hi in CODE):
        pdata[begin] = ((packed >> 8) & 0x3FFFFF) * 4

# jump tables: bctr address -> labels
bctr_labels = {}
for m in re.finditer(r"base = 0x([0-9A-F]+)\s*\nr = \d+\s*\ndefault = 0x[0-9A-F]+\s*\nlabels = \[(.*?)\]",
                     open(sys.argv[2]).read(), re.S):
    base = int(m.group(1), 16)
    labels = [int(x, 16) for x in re.findall(r"0x([0-9A-F]+)", m.group(2))]
    for k in range(16):
        a = base + 4 * k
        if not any(lo <= a < hi for lo, hi in CODE):
            break
        if word(a) == 0x4E800420:
            bctr_labels.setdefault(a, []).extend(labels)
            break

# direct call targets: a function's own start, so a b to one is a tail call, not this function's code
called = set()
for lo, hi in CODE:
    for a in range(lo, hi, 4):
        x = word(a)
        if x >> 26 == 18 and (x & 3) == 1:
            called.add(a + sext(x & 0x03FFFFFC, 26))

# the gaps: code not covered by any .pdata function
gaps = []
for lo, hi in CODE:
    pos = lo
    for start in sorted(s for s in pdata if lo <= s < hi):
        if start > pos:
            gaps.append((pos, start))
        pos = max(pos, start + pdata[start])
    if pos < hi:
        gaps.append((pos, hi))


def extent(start, end):
    """Highest address reached from start without leaving [start, end), plus 4."""
    seen, work, top = set(), [start], start
    while work:
        a = work.pop()
        while start <= a < end and a not in seen and (a == start or a not in called):
            seen.add(a)
            top = max(top, a)
            x = word(a)
            op = x >> 26
            if x == 0:  # padding: not code
                break
            if op == 18:  # b, bl, ba
                if x & 1:  # bl: a call, falls through
                    a += 4
                    continue
                target = a + sext(x & 0x03FFFFFC, 26)
                if not x & 2 and target not in called:
                    work.append(target)
                break
            if op == 16:  # bc
                target = a + sext(x & 0xFFFC, 16)
                if not x & 2 and target not in called:
                    work.append(target)
                bo = (x >> 21) & 31
                if (bo & 0x14) == 0x14 and not x & 1:  # branch always
                    break
                a += 4
                continue
            if op == 19:
                xo = (x >> 1) & 0x3FF
                bo = (x >> 21) & 31
                if xo in (16, 528) and not x & 1 and (bo & 0x14) == 0x14:  # blr / bctr
                    if xo == 528 and a in bctr_labels:
                        work.extend(bctr_labels[a])
                    break
            a += 4
    return top + 4


NOP, BLR = 0x60000000, 0x4E800020


def import_thunk(a):
    """An XEX import thunk as the extracted PE has it: nop, nop, nop, blr. XenonRecomp binds it to the
    runtime's __imp__ function; declared here as well, every call went to a recompiled empty body
    instead (Halo 3's first build: NtAllocateVirtualMemory did nothing and the CRT heap landed at 0)."""
    return a + 16 <= hi and [word(a + 4 * k) for k in range(4)] == [NOP, NOP, NOP, BLR]


out = []
thunks = 0
for lo, hi in gaps:
    pos = lo
    while pos < hi:
        while pos < hi and (word(pos) == 0 or import_thunk(pos)):
            if word(pos) == 0:
                pos += 4
            else:
                pos += 16
                thunks += 1
        if pos >= hi:
            break
        end = extent(pos, hi)
        if end - pos > 4:  # a lone word is no function worth declaring
            out.append((pos, end - pos))
        pos = end

for a, size in out:
    print("    { address = 0x%08X, size = 0x%X }," % (a, size))
big = sum(1 for _, s in out if s >= 0x100)
print(f"gaps: {len(gaps)}, leaf functions: {len(out)} ({big} of 0x100 bytes or more), "
      f"import thunks left to XenonRecomp: {thunks}", file=sys.stderr)
