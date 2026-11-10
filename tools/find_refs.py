"""Finds code that builds the given addresses with lis + addi/ori/lwz/stw... (same base register, within 64 instructions).
Usage: py -3 -I find_refs.py <default.pe> <xexinfo.txt> <hex address>...
Prints each reference and the function containing it (from the sub_ names in ppc/).
"""
import bisect, glob, os, re, struct, sys

BASE = int(__import__("os").environ.get("XPORT_IMAGE_BASE", "0x82000000"), 16)  # Halo 3: its DLLs load elsewhere
pe = open(sys.argv[1], "rb").read()
info = open(sys.argv[2], encoding="utf-8-sig").read()
targets = {int(a, 16) for a in sys.argv[3:]}
sec = re.search(r"section \.text\s+base=0x([0-9A-F]+) size=0x([0-9A-F]+)", info)
tb, ts = int(sec.group(1), 16), int(sec.group(2), 16)
w = struct.unpack_from(">%dI" % (ts // 4), pe, tb - BASE)
starts = set()
ppc = os.path.join(os.path.dirname(os.path.abspath(sys.argv[1])), "..", "ppc")
for f in glob.glob(os.path.join(ppc, "*.cpp")):
    for m in re.finditer(r"PPC_FUNC_IMPL\(__imp__sub_([0-9A-F]{8})\)", open(f, encoding="utf-8", errors="replace").read()):
        starts.add(int(m.group(1), 16))
starts = sorted(starts)
for i, x in enumerate(w):
    if x >> 26 != 15 or (x >> 16) & 31 != 0:  # lis rD = addis rD, 0, imm
        continue
    rd, hi = (x >> 21) & 31, x & 0xFFFF
    for j in range(i + 1, min(i + 64, len(w))):
        y = w[j]
        op, ra = y >> 26, (y >> 16) & 31
        if ra == rd and op in (14, 24, 32, 34, 36, 38, 40, 44, 48, 50, 52, 54):  # addi ori lwz lbz stw stb lhz sth lfs lfd stfs stfd
            lo = y & 0xFFFF
            addr = ((hi << 16) + (lo if op == 24 else (lo - 0x10000 if lo & 0x8000 else lo))) & 0xFFFFFFFF
            if addr in targets:
                at = tb + 4 * j
                k = bisect.bisect_right(starts, at) - 1
                print(f"{addr:08X} used at {at:08X} in sub_{starts[k]:08X}" if k >= 0 else f"{addr:08X} at {at:08X}")
        if (y >> 21) & 31 == rd and op in (14, 15, 24, 32, 34, 40):  # rD overwritten (approximation)
            if not (op in (14, 24) and ra == rd):
                break
