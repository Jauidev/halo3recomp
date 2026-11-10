"""Cuts jump tables that XenonAnalyse read past their end. With no bounds check before the dispatch
(an index the code knows is small, a function's return value), XenonAnalyse cannot tell the length
and keeps reading: the code after the table comes out as labels (instruction words such as
0x38600000, li r3,0). XenonRecomp then emits jumps to those, and tools/fix_switch.py extends the
owner to them (Halo 3: sizes of 0x7997E1F0 and a crash of XenonRecomp in the next round).
The table is the words at the lis/addi address; it ends where the first case label that comes
after it starts (the compiler puts the cases right behind the table), or at a word that is not
an address in code. Rewrites <switch_tables.toml> in place; stats on stderr.
Usage: trim_tables.py <default.pe> <switch_tables.toml>
"""
import re, struct, sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script directory out
from pe_layout import code_ranges

BASE = int(__import__("os").environ.get("XPORT_IMAGE_BASE", "0x82000000"), 16)  # Halo 3: its DLLs load elsewhere

pe = open(sys.argv[1], "rb").read()
CODE = code_ranges(pe)


def word(a):
    return struct.unpack_from(">I", pe, a - BASE)[0]


def in_code(a):
    return any(lo <= a < hi for lo, hi in CODE)


def simm(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def table_address(base):
    """lis rX,hi then addi/ori rX,rX,lo within a few instructions: the table's address."""
    first = word(base)
    if first >> 26 != 15 or (first >> 16) & 31 != 0:  # lis = addis rD,0,imm
        return None
    reg = (first >> 21) & 31
    high = (first & 0xFFFF) << 16
    for k in range(1, 6):
        x = word(base + 4 * k)
        op = x >> 26
        if op == 14 and (x >> 21) & 31 == reg and (x >> 16) & 31 == reg:  # addi
            return (high + simm(x)) & 0xFFFFFFFF
        if op == 24 and (x >> 21) & 31 == reg and (x >> 16) & 31 == reg:  # ori rA,rS,imm
            return (high | (x & 0xFFFF)) & 0xFFFFFFFF
    return None


path = sys.argv[2]
text = open(path).read()
block = re.compile(r"(base = 0x([0-9A-F]+)\s*\nr = \d+\s*\ndefault = 0x[0-9A-F]+\s*\nlabels = \[)(.*?)(\])", re.S)
trimmed = []


def trim(m):
    base = int(m.group(2), 16)
    labels = [int(x, 16) for x in re.findall(r"0x([0-9A-F]+)", m.group(3))]
    table = table_address(base)
    if table is None or not labels:
        return m.group(0)
    keep = []
    end = None  # the first code after the table: the lowest label above it
    for i, label in enumerate(labels):
        at = table + 4 * i
        if (end is not None and at >= end) or not in_code(label) or word(at) != label:
            break
        keep.append(label)
        if label > table and (end is None or label < end):
            end = label
    if len(keep) == len(labels) or not keep:
        return m.group(0)
    trimmed.append((base, len(labels), len(keep)))
    body = "\n" + "".join(f"    0x{l:08X},\n" for l in keep)
    return m.group(1) + body + m.group(4)


text = block.sub(trim, text)
open(path, "w").write(text)
for base, before, after in trimmed:
    print(f"table at {base:08X}: {before} -> {after} labels", file=sys.stderr)
print(f"tables trimmed: {len(trimmed)}", file=sys.stderr)
