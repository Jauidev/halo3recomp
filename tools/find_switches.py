"""Finds jump tables XenonAnalyse missed. The compiler's dispatch sequence is
    lis r12,hi ; addi r12,r12,lo ; rlwinm r0,rN,2,0,29 ; lwzx r0,r12,r0 ; mtctr r0 ; bctr
and tables reached without a bounds check (e.g. an index that is always 0-3) are not recognised:
the bctr becomes an indirect call to a case label that is no function (null host pointer at
run time). The table usually sits right after the bctr; its length runs to the first label.
Usage: find_switches.py <default.pe> <switch_tables.toml>  -> [[switch]] entries on stdout
"""
import re, struct, sys

import os
sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))  # -I leaves the script directory out
from pe_layout import code_ranges

BASE = int(__import__("os").environ.get("XPORT_IMAGE_BASE", "0x82000000"), 16)  # Halo 3: its DLLs load elsewhere

pe = open(sys.argv[1], "rb").read()
CODE = code_ranges(pe)
known = {int(b, 16) for b in re.findall(r"base = 0x([0-9A-Fa-f]+)", open(sys.argv[2]).read())}


def word(a):
    return struct.unpack_from(">I", pe, a - BASE)[0]


def in_code(a):
    return any(lo <= a < hi for lo, hi in CODE)


def simm(v):
    v &= 0xFFFF
    return v - 0x10000 if v & 0x8000 else v


def is_lis(w):  # lis r12,hi
    return w >> 16 == 0x3D80


def is_addi(w):  # addi r12,r12,lo
    return w >> 16 == 0x398C


def is_rlwinm(w):  # rlwinm r0,rN,2,0,29 (54 0x 10 3A with rS in bits 21-25)
    return (w & 0xFC1FFFFF) == 0x5400103A


found = 0
for lo, hi in CODE:
    for a in range(lo, hi - 24, 4):
        w3, w4, w5 = word(a + 12), word(a + 16), word(a + 20)
        # lwzx r0,r12,r0 ; mtctr r0 ; bctr
        if w3 != 0x7C0C002E or w4 != 0x7C0903A6 or w5 != 0x4E800420:
            continue
        # Before them lis, addi and rlwinm in any order the data allows (lis before its addi):
        # GTA IV and Gears of War have lis/addi/rlwinm, XDK 2.0.20500's compiler (Gears of War 3)
        # schedules lis/rlwinm/addi, so its jump tables became indirect calls to case labels.
        first3 = [word(a), word(a + 4), word(a + 8)]
        lis = [w for w in first3 if is_lis(w)]
        addi = [w for w in first3 if is_addi(w)]
        rlwinm = [w for w in first3 if is_rlwinm(w)]
        if len(lis) != 1 or len(addi) != 1 or len(rlwinm) != 1 or first3.index(lis[0]) > first3.index(addi[0]):
            continue
        if a in known:
            continue

        w0, w1, w2 = lis[0], addi[0], rlwinm[0]
        index_reg = (w2 >> 21) & 31
        table = ((w0 & 0xFFFF) << 16) + simm(w1)
        labels = []
        t = table
        first = None
        while in_code(word(t)) and word(t) & 3 == 0:
            label = word(t)
            if first is None or label < first:
                first = label
            labels.append(label)
            t += 4
            # The table ends where the code it points to begins.
            if first is not None and t >= first:
                break
        if not labels:
            continue

        # Bounds check, if any: cmplwi crX,rN,imm, then a bgt on crX -> default. The compiler may put
        # other instructions between them (Gears of War 3: two sth).
        default = labels[0]
        for b in range(a - 4, a - 64, -4):
            w = word(b)
            if (w >> 26) == 10 and ((w >> 16) & 31) == index_reg:  # cmpli
                field = (w >> 23) & 7
                for c in range(b + 4, a, 4):
                    nxt = word(c)
                    if (nxt >> 26) == 16 and ((nxt >> 16) & 31) // 4 == field:  # bc on crX
                        default = c + simm(nxt & 0xFFFC)
                        break
                break

        found += 1
        print("\n# missed by XenonAnalyse (tools/find_switches.py)")
        print("[[switch]]")
        print("base = 0x%08X" % a)
        print("r = %d" % index_reg)
        print("default = 0x%08X" % default)
        print("labels = [")
        for label in labels:
            print("    0x%08X," % label)
        print("]")

def offset_tables():
    """Jump tables of offsets from a base label: bytes or halfwords, scaled by 4 or not, added to the
    base. XDK 2.0.20500's compiler (Gears of War 3) puts a nop next to the base's addi, and
    XenonAnalyse finds none of these (nor any other table in that game):
      lis r12,T ; addi r12,r12,T ; lbzx r0,r12,rN ; [rlwinm r0,r0,2,0,29] ; lis r12,B ; nop ; addi r12,r12,B
      lis r12,T ; rlwinm r0,rN,1,0,30 ; addi r12,r12,T ; lhzx r0,r12,r0 ; lis r12,B ; addi r12,r12,B ; nop
    and then add r12,r12,r0 ; mtctr r12 ; bctr. Yields (base, index register, default, labels)."""
    LBZX, LHZX = 0x7C0C00AE, 0x7C0C022E  # l?zx r0,r12,rB (rB masked)
    for lo, hi in CODE:
        for a in range(lo + 48, hi - 8, 4):
            # add r12,r12,r0 ; mtctr r12 ; bctr
            if word(a) != 0x7D8C0214 or word(a + 4) != 0x7D8903A6 or word(a + 8) != 0x4E800420:
                continue
            body = [(b, word(b)) for b in range(a - 48, a, 4) if word(b) != 0x60000000]
            loads = [k for k, (_, w) in enumerate(body) if (w & 0xFFFF07FF) in (LBZX, LHZX)]
            if not loads:
                continue
            k = loads[-1]
            load_at, load = body[k]
            after = [w for _, w in body[k + 1:]]
            before = [w for _, w in body[:k]]
            # after the load only the base (one lis r12, then one addi r12,r12) and the optional
            # scale (rlwinm r0,r0,2,0,29)
            lis_after = [j for j, w in enumerate(after) if is_lis(w)]
            addi_after = [j for j, w in enumerate(after) if is_addi(w)]
            if len(lis_after) != 1 or len(addi_after) != 1 or lis_after[0] > addi_after[0]:
                continue
            if any(not (is_lis(w) or is_addi(w) or w == 0x5400103A) for w in after):
                continue
            base_lis, base_addi = after[lis_after[0]], after[addi_after[0]]
            scale = 4 if 0x5400103A in after else 1
            # table: the nearest lis r12 / addi r12,r12 before the load (lis first)
            table_addi = next((j for j in range(len(before) - 1, -1, -1) if is_addi(before[j])), None)
            table_lis = next((j for j in range(table_addi - 1, -1, -1) if is_lis(before[j])), None) if table_addi is not None else None
            if table_lis is None:
                continue
            size = 2 if (load & 0xFFFF07FF) == LHZX else 1
            index_reg = (load >> 11) & 31
            start = body[table_lis][0]
            if size == 2 or index_reg == 0:
                # the index comes scaled into r0: rlwinm r0,rN,1,0,30 (halfwords) between lis and load
                shifts = [w for w in before[table_lis:] if (w & 0xFC1FFFFF) == 0x5400083C]
                if len(shifts) != 1 or index_reg != 0:
                    continue
                index_reg = (shifts[0] >> 21) & 31
            table = ((before[table_lis] & 0xFFFF) << 16) + simm(before[table_addi])
            base = ((base_lis & 0xFFFF) << 16) + simm(base_addi)
            if any(start <= b <= a + 8 for b in known):
                continue
            # the length comes from the bounds check: cmplwi crX,rN,last ; ... ; bgt crX,default.
            # It may test a copy of the index (mr r31,r4 ; cmplwi cr6,r4,18 ; ... rlwinm r0,r31,1).
            aliases = {index_reg}
            for b in range(start - 4, start - 64, -4):
                w = word(b)
                if (w >> 26) == 31 and ((w >> 1) & 0x3FF) == 444 and ((w >> 21) & 31) == ((w >> 11) & 31):  # mr rA,rS
                    dst, src = (w >> 16) & 31, (w >> 21) & 31
                    if dst in aliases or src in aliases:
                        aliases |= {dst, src}
            count, default = None, None
            for b in range(start - 4, start - 64, -4):
                w = word(b)
                if (w >> 26) == 10 and ((w >> 16) & 31) in aliases:
                    count = (w & 0xFFFF) + 1
                    field = (w >> 23) & 7
                    for c in range(b + 4, start, 4):
                        nxt = word(c)
                        if (nxt >> 26) == 16 and ((nxt >> 16) & 31) // 4 == field:
                            default = c + simm(nxt & 0xFFFC)
                            break
                    break
            if not count or count > 1024:
                print("offset jump table at 0x%08X: no bounds check, left out" % a, file=sys.stderr)
                continue
            fmt = ">B" if size == 1 else ">H"
            labels = [base + scale * struct.unpack_from(fmt, pe, table - BASE + size * i)[0] for i in range(count)]
            if not all(in_code(t) and t & 3 == 0 for t in labels):
                print("offset jump table at 0x%08X: a case outside the code, left out" % a, file=sys.stderr)
                continue
            yield start, index_reg, default if default is not None else labels[0], labels


for start, index_reg, default, labels in offset_tables():
    found += 1
    print("\n# offset table, missed by XenonAnalyse (tools/find_switches.py)")
    print("[[switch]]")
    print("base = 0x%08X" % start)
    print("r = %d" % index_reg)
    print("default = 0x%08X" % default)
    print("labels = [")
    for label in labels:
        print("    0x%08X," % label)
    print("]")

print("missed jump tables: %d" % found, file=sys.stderr)
