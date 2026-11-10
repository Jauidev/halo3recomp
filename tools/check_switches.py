"""Checks the absolute jump tables of a switch_tables.toml against the code they come from, and fixes
the ones XenonAnalyse got wrong. The compiler's dispatch is
    lis r12,hi ; addi r12,r12,lo ; rlwinm r0,rN,2,0,29 ; lwzx r0,r12,r0 ; mtctr r0 ; bctr
(lis/addi/rlwinm in any order). Halo 3 has tables whose index has no bounds check (kind = top three
bits of a word, rlwinm r10,r10,3,29,31): XenonAnalyse took the register and the length from an
unrelated compare before them (sub_82086990: r11 and one label instead of r10 and four), so the
recompiled switch jumped on the wrong value and skipped the resource pointer fixups (a texture
definition pointer left null -> read of guest 0x20 at boot).
Usage: check_switches.py <default.pe> <switch_tables.toml> [--fix]
  Prints each wrong entry; with --fix rewrites the TOML (a copy is kept as .antes-check)."""
import os, re, shutil, struct, sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script directory out
from pe_layout import code_ranges

BASE = int(os.environ.get("XPORT_IMAGE_BASE", "0x82000000"), 16)

pe = open(sys.argv[1], "rb").read()
CODE = code_ranges(pe)
toml_path = sys.argv[2]
fix = "--fix" in sys.argv[3:]
text = open(toml_path, encoding="utf-8").read()


def word(a):
    return struct.unpack_from(">I", pe, a - BASE)[0]


def in_code(a):
    return any(lo <= a < hi for lo, hi in CODE)


def simm(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def is_lis(w):
    return w >> 16 == 0x3D80


def is_addi(w):
    return w >> 16 == 0x398C


def is_rlwinm(w):
    return (w & 0xFC1FFFFF) == 0x5400103A


def decode(a):
    """(index register, table labels) of the absolute dispatch at a, or None."""
    if word(a + 12) != 0x7C0C002E or word(a + 16) != 0x7C0903A6 or word(a + 20) != 0x4E800420:
        return None
    first3 = [word(a), word(a + 4), word(a + 8)]
    lis = [w for w in first3 if is_lis(w)]
    addi = [w for w in first3 if is_addi(w)]
    rlw = [w for w in first3 if is_rlwinm(w)]
    if len(lis) != 1 or len(addi) != 1 or len(rlw) != 1:
        return None
    reg = (rlw[0] >> 21) & 31
    table = ((lis[0] & 0xFFFF) << 16) + simm(addi[0])
    # The table ends where the code after it begins: the lowest label past the table (labels before
    # it, e.g. a loop's head, say nothing about its length).
    labels, first, t = [], None, table
    while in_code(word(t)) and word(t) & 3 == 0:
        label = word(t)
        if label > table:
            first = label if first is None else min(first, label)
        labels.append(label)
        t += 4
        if first is not None and t >= first:
            break
        if len(labels) > 1024:
            return None
    return reg, labels


def bound(a, reg):
    """The count of the bounds check before the dispatch, or None: cmplwi crX,reg,N followed by a bgt
    on crX (to the default). A compare of the register with no bgt after it (a null test of the
    value the index is computed from) is no bounds check."""
    for b in range(a - 4, a - 64, -4):
        w = word(b)
        if (w >> 26) == 10 and ((w >> 16) & 31) == reg:
            field = (w >> 23) & 7
            for c in range(b + 4, a, 4):
                nxt = word(c)
                if (nxt >> 26) == 16 and ((nxt >> 16) & 31) == field * 4 + 1 and ((nxt >> 21) & 0x1C) == 0xC:
                    return (w & 0xFFFF) + 1
            return None
    return None


entries = list(re.finditer(r"\[\[switch\]\]\s*base = 0x([0-9A-Fa-f]+)\s*r = (\d+)\s*default = 0x([0-9A-Fa-f]+)\s*labels = \[([^\]]*)\]", text))
wrong = []
for m in entries:
    a, r = int(m.group(1), 16), int(m.group(2))
    labels = [int(x, 16) for x in re.findall(r"0x([0-9A-Fa-f]+)", m.group(4))]
    d = decode(a)
    if d is None:
        continue
    reg, table = d
    if not table:
        continue
    count = bound(a, reg)
    # With a bounds check the length is the check's; without one, the table runs to its first label.
    want = table[:count] if count and count <= len(table) else table
    if reg != r or labels != want:
        wrong.append((m, a, r, labels, reg, want))

for m, a, r, labels, reg, want in wrong:
    print("0x%08X: r%d %d labels -> r%d %d labels %s" % (a, r, len(labels), reg, len(want), " ".join("%08X" % x for x in want)))
print("%d tables checked, %d wrong" % (len(entries), len(wrong)), file=sys.stderr)

if fix and wrong:
    shutil.copyfile(toml_path, toml_path + ".antes-check")
    out, last = [], 0
    for m, a, r, labels, reg, want in sorted(wrong, key=lambda x: x[0].start()):
        default = int(m.group(3), 16)
        block = ["[[switch]]", "base = 0x%08X" % a, "r = %d" % reg, "default = 0x%08X" % default, "labels = ["]
        block += ["    0x%08X," % x for x in want] + ["]"]
        out.append(text[last:m.start()])
        out.append("# corrected by tools/check_switches.py\n" + "\n".join(block))
        last = m.end()
    out.append(text[last:])
    open(toml_path, "w", encoding="utf-8", newline="\n").write("".join(out))
    print("rewrote %s" % toml_path, file=sys.stderr)
