"""Minimal XDVDFS (Xbox/Xbox 360 disc) reader: list tree and extract files.
Usage: xdvdfs.py <iso> list
       xdvdfs.py <iso> extract <path-in-iso> <out-file>
       xdvdfs.py <iso> extractall <out-dir>      (skips files already extracted with the right size)
"""
import os, struct, sys

SECTOR = 2048
MAGIC = b"MICROSOFT*XBOX*MEDIA"
# Game partition offsets: XGD3, XGD2, XGD1, plain XISO
OFFSETS = [0x2080000, 0xFD90000, 0x18300000, 0]


def find_partition(f):
    for off in OFFSETS:
        f.seek(off + 32 * SECTOR)
        if f.read(20) == MAGIC:
            f.seek(off + 32 * SECTOR + 20)
            root_sec, root_size = struct.unpack("<II", f.read(8))
            return off, root_sec, root_size
    raise SystemExit("no XDVDFS partition found")


def walk_dir(f, base, sector, size, prefix, out):
    if size == 0:
        return
    f.seek(base + sector * SECTOR)
    data = f.read(size)
    stack = [0]
    seen = set()
    while stack:
        pos = stack.pop()
        if pos in seen or pos * 4 + 14 > len(data):
            continue
        seen.add(pos)
        o = pos * 4
        left, right, sec, sz, attr, nlen = struct.unpack_from("<HHIIBB", data, o)
        if left == 0xFFFF:
            continue
        name = data[o + 14:o + 14 + nlen].decode("latin-1")
        path = prefix + "/" + name
        if attr & 0x10:
            out.append((path, sec, sz, True))
            walk_dir(f, base, sec, sz, path, out)
        else:
            out.append((path, sec, sz, False))
        if left:
            stack.append(left)
        if right:
            stack.append(right)


def main():
    iso = sys.argv[1]
    with open(iso, "rb") as f:
        base, rs, rz = find_partition(f)
        entries = []
        walk_dir(f, base, rs, rz, "", entries)
        if sys.argv[2] == "extractall":
            out = sys.argv[3]
            total = sum(sz for _, _, sz, d in entries if not d)
            done = 0
            for p, sec, sz, d in sorted(entries):
                dest = os.path.join(out, *p.strip("/").split("/"))
                if d:
                    os.makedirs(dest, exist_ok=True)
                    continue
                os.makedirs(os.path.dirname(dest), exist_ok=True)
                if not (os.path.exists(dest) and os.path.getsize(dest) == sz):
                    f.seek(base + sec * SECTOR)
                    with open(dest, "wb") as o:
                        left = sz
                        while left:
                            chunk = f.read(min(left, 8 << 20))
                            o.write(chunk)
                            left -= len(chunk)
                done += sz
                print(f"{100 * done / total:5.1f}%  {p}", flush=True)
            return
        if sys.argv[2] == "list":
            print(f"# partition offset 0x{base:X}")
            for p, sec, sz, d in sorted(entries):
                print(f"{'D' if d else 'F'} {sz:>12} {p}")
        elif sys.argv[2] == "extract":
            want = sys.argv[3].lower()
            for p, sec, sz, d in entries:
                if p.lower() == want:
                    f.seek(base + sec * SECTOR)
                    with open(sys.argv[4], "wb") as o:
                        left = sz
                        while left:
                            chunk = f.read(min(left, 1 << 20))
                            o.write(chunk)
                            left -= len(chunk)
                    print(f"extracted {p} ({sz} bytes)")
                    return
            raise SystemExit("not found: " + sys.argv[3])


main()
