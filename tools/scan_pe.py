"""Static scans over the dumped in-memory XEX image (big-endian PPC).
Usage: scan_pe.py <default.pe> <xexinfo.txt>
Prints: register save/restore helper addresses, import call-site counts, setjmp/longjmp
candidates, bctr/jump-table counts, MMIO constants and interesting opcode counts.
"""
import re, struct, sys
from collections import Counter, defaultdict

BASE = int(__import__("os").environ.get("XPORT_IMAGE_BASE", "0x82000000"), 16)  # Halo 3: its DLLs load elsewhere
pe = open(sys.argv[1], "rb").read()
info = open(sys.argv[2], encoding="utf-8-sig").read()

sections = {}
for m in re.finditer(r"section (\S+)\s+base=0x([0-9A-F]+) size=0x([0-9A-F]+) (\w+)", info):
    sections[m.group(1)] = (int(m.group(2), 16), int(m.group(3), 16), m.group(4))

CODE = [name for name, (_, _, kind) in sections.items() if kind == "code"]

thunks = {}
for m in re.finditer(r"^(\S+) func 0x([0-9A-F]+) (\S+) var=0x[0-9A-F]+ thunk=0x([0-9A-F]+)", info, re.M):
    thunks[int(m.group(4), 16)] = m.group(3).replace("__imp__", "")


def words(name):
    b, s, _ = sections[name]
    off = b - BASE
    n = s // 4
    return b, struct.unpack_from(">%dI" % n, pe, off)


def find_seq(seq, secs=(".text",)):
    out = []
    for name in secs:
        b, w = words(name)
        first = seq[0]
        for i, x in enumerate(w):
            if x == first and tuple(w[i:i + len(seq)]) == tuple(seq):
                out.append(b + 4 * i)
    return out


print("== register save/restore helpers ==")
pats = {
    "restgprlr_14": [0xE9C1FF68],
    "savegprlr_14": [0xF9C1FF68],
    "restfpr_14": [0xC9CCFF70],
    "savefpr_14": [0xD9CCFF70],
    "restvmx_14": [0x3960FEE0, 0x7DCB60CE],
    "savevmx_14": [0x3960FEE0, 0x7DCB61CE],
    "restvmx_64": [0x3960FC00, 0x100B60CB],
    "savevmx_64": [0x3960FC00, 0x100B61CB],
}
helpers = {}
for k, p in pats.items():
    hits = find_seq(p, CODE)
    helpers[k] = hits
    print(f"{k}_address = {', '.join('0x%08X' % h for h in hits) or 'NOT FOUND'}")

# Branch-and-link call sites per target
calls = Counter()
callers = defaultdict(set)
mtctr = bctr = bctrl = 0
mmio = Counter()
ops = Counter()
for name in CODE:
    b, w = words(name)
    for i, x in enumerate(w):
        addr = b + 4 * i
        op = x >> 26
        if op == 18 and (x & 1):  # bl / bla
            li = x & 0x03FFFFFC
            if li & 0x02000000:
                li -= 0x04000000
            tgt = li if (x & 2) else addr + li
            calls[tgt] += 1
            callers[tgt].add(addr)
        elif x == 0x7C0903A6 or (x & 0xFC1FFFFF) == 0x7C0903A6:
            mtctr += 1
        elif x == 0x4E800420:
            bctr += 1
        elif x == 0x4E800421:
            bctrl += 1
        elif x == 0x44000002:
            ops["sc"] += 1
        elif op == 3:
            ops["twi"] += 1
        elif (x & 0xFC0007FE) == 0x7C000008:
            ops["tw"] += 1
        elif (x & 0xFC0007FE) == 0x7C0000A6:
            ops["mfmsr"] += 1
        elif (x & 0xFC0007FE) == 0x7C000164:
            ops["mtmsrd"] += 1
        elif (x & 0xFC0007FE) == 0x7C2007EC:
            ops["dcbz128"] += 1
        elif (x & 0xFC0007FE) == 0x7C0007EC:
            ops["dcbz"] += 1
        elif (x & 0xFC0007FE) == 0x7C0006AC:
            ops["eieio"] += 1
        elif (x & 0xFC0007FE) == 0x7C0002E6:
            ops["mftb"] += 1
        elif (x & 0xFC0007FE) == 0x7C0000A8:
            ops["ldarx"] += 1
        elif (x & 0xFC0007FE) == 0x7C000028:
            ops["lwarx"] += 1
        elif (x & 0xFC0007FF) == 0x7C0001AD:
            ops["stdcx."] += 1
        elif (x & 0xFC0007FF) == 0x7C00012D:
            ops["stwcx."] += 1
        if op == 15:  # lis rD, imm
            imm = x & 0xFFFF
            if 0x7FC8 <= imm <= 0x7FFF:
                mmio[imm] += 1
        if op == 4:
            ops["vmx(op4)"] += 1

print("\n== control flow ==")
print(f"bl call sites: {sum(calls.values())}, distinct targets: {len(calls)}")
print(f"mtctr: {mtctr}  bctr: {bctr}  bctrl: {bctrl}")
print("opcodes:", dict(ops))
print("lis 0x7FC8..0x7FFF (GPU/MMIO hi halves):", {hex(k): v for k, v in sorted(mmio.items())})

print("\n== import call sites (direct bl to thunk) ==")
rows = sorted(((calls.get(a, 0), n, a) for a, n in thunks.items()), reverse=True)
for c, n, a in rows:
    print(f"{c:5d}  {n}  (thunk 0x{a:08X})")

# setjmp/longjmp: longjmp calls RtlUnwind on 360 CRT; setjmp is usually just after it.
print("\n== setjmp/longjmp candidates ==")
for a, n in thunks.items():
    if n in ("RtlUnwind", "RtlCaptureContext", "RtlRaiseException", "__C_specific_handler"):
        print(n, "callers:", ", ".join("0x%08X" % c for c in sorted(callers.get(a, []))))

# .pdata: 8-byte entries (BeginAddress, packed length/flags)
pb, ps, _ = sections[".pdata"]
n = ps // 8
funcs = []
for i in range(n):
    begin, packed = struct.unpack_from(">II", pe, pb - BASE + i * 8)
    if begin == 0:
        continue
    length = ((packed >> 8) & 0x3FFFFF) * 4
    funcs.append((begin, length))
cover = sum(l for _, l in funcs)
tb, ts, _ = sections[".text"]
print(f"\n== .pdata == entries: {len(funcs)}  bytes covered: {cover} of .text {ts} ({100*cover/ts:.1f}%)")
big = sorted(funcs, key=lambda f: -f[1])[:10]
print("largest:", ", ".join("0x%08X(+0x%X)" % f for f in big))
print("bl targets not at a .pdata start:", len(set(calls) - {f[0] for f in funcs}))
