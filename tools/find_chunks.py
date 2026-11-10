"""Finds code fragments that sit after a .pdata function's end but branch back into it
(compiler-outlined chunks). XenonAnalyse loops forever on them (conditional branch before
base underflows), so the owning function must be declared with the extended size.
Usage: find_chunks.py <default.pe>  -> prints TOML `functions` entries and stats on stderr
"""
import struct, sys, bisect

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script directory out
from pe_layout import sections

BASE = int(__import__("os").environ.get("XPORT_IMAGE_BASE", "0x82000000"), 16)  # Halo 3: its DLLs load elsewhere
pe = open(sys.argv[1], "rb").read()
LAYOUT = sections(pe)
TB, TS = LAYOUT[".text"]
w = struct.unpack_from(">%dI" % (TS // 4), pe, TB - BASE)
word = lambda a: w[(a - TB) // 4]

pb, ps = LAYOUT[".pdata"]
funcs = {}
for i in range(ps // 8):
    begin, packed = struct.unpack_from(">II", pe, pb - BASE + 8 * i)
    if TB <= begin < TB + TS:
        funcs[begin] = ((packed >> 8) & 0x3FFFFF) * 4
starts = sorted(funcs)


def branch_target(a, x):
    op = x >> 26
    if op == 18 and not (x & 1) and not (x & 2):
        li = x & 0x03FFFFFC
        return a + (li - 0x04000000 if li & 0x02000000 else li)
    if op == 16 and not (x & 1) and not (x & 2):
        bd = x & 0xFFFC
        return a + (bd - 0x10000 if bd & 0x8000 else bd)
    return None


extend = {}  # owner start -> new end
for k, s in enumerate(starts):
    end = s + funcs[s]
    nxt = starts[k + 1] if k + 1 < len(starts) else TB + TS
    # walk the gap after this function, following fragments that branch back into [s, end)
    a = end
    owner_end = end
    while a < nxt:
        x = word(a)
        if x == 0:
            a += 4
            continue
        # scan a straight-line fragment up to an unconditional exit
        b = a
        back = False
        while b < nxt:
            y = word(b)
            t = branch_target(b, y)
            # a branch to the owner's first instruction is a tail call, not a chunk
            if t is not None and s < t < owner_end:
                back = True
            b += 4
            if (y >> 26) == 18 and not (y & 1) or y == 0x4E800020 or y == 0x4E800420 or y == 0:
                break
        if not back:
            break
        owner_end = b
        a = b
    if owner_end != end:
        extend[s] = owner_end

for s in sorted(extend):
    print("    { address = 0x%08X, size = 0x%X }," % (s, extend[s] - s))
print("chunks found: %d" % len(extend), file=sys.stderr)
