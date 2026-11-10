"""Reads Xbox 360 STFS packages (CON, LIVE, PIRS: Xbox Live Arcade games, demos, DLC).

  stfs.py info <package>              title, media ID, version, content type, files
  stfs.py list <package>              one "F|D size path" line per entry
  stfs.py extract <package> <dir>     every file, with its folders

Layout as in Free60's STFS notes and Velocity: 0x1000-byte blocks after the header, with hash
tables between them (every 0xAA data blocks one level-0 table, every 0x70E4 one level-1 table; one
copy of each in a "female" package, two in a "male" one, the live copy chosen by a status bit).
Games on Demand packages (SVOD, descriptor type 1) are a different layout and are refused.
"""
import os, struct, sys

BLOCK = 0x1000
MAGICS = (b"CON ", b"LIVE", b"PIRS")


def u24le(b, o):
    return b[o] | b[o + 1] << 8 | b[o + 2] << 16


def u24be(b, o):
    return b[o] << 16 | b[o + 1] << 8 | b[o + 2]


class Package:
    def __init__(self, path):
        self.f = open(path, "rb")
        h = self.f.read(0xA000)
        if h[:4] not in MAGICS:
            raise ValueError("not an STFS package (no CON/LIVE/PIRS magic)")
        self.magic = h[:4].decode()
        self.header_size, self.content_type = struct.unpack_from(">II", h, 0x340)
        self.media_id, self.version, self.base_version, self.title_id = struct.unpack_from(">IIII", h, 0x354)
        self.descriptor_type = struct.unpack_from(">I", h, 0x3A9)[0] if len(h) > 0x3AD else 0
        self.display_name = h[0x411:0x411 + 0x80].decode("utf-16-be", "replace").split("\0")[0]
        if self.descriptor_type == 1:
            raise ValueError("SVOD package (Games on Demand): not supported, only STFS")
        vd = 0x379
        self.block_separation = h[vd + 2]
        self.table_blocks = struct.unpack_from("<H", h, vd + 3)[0]
        self.table_start = u24le(h, vd + 5)
        self.allocated = struct.unpack_from(">I", h, vd + 0x1C)[0]
        self.sex = (~self.block_separation) & 1          # 0 female (one table a level), 1 male (two)
        self.step = (0xAB, 0x718F) if self.sex == 0 else (0xAC, 0x723A)
        self.first_table = (self.header_size + 0xFFF) & 0xFFFFF000
        self.top = 0 if self.allocated <= 0xAA else 1 if self.allocated <= 0x70E4 else 2
        self.entries = self._read_entries()

    def _read(self, offset, size):
        self.f.seek(offset)
        return self.f.read(size)

    # ---- block numbers to file offsets (Velocity's StfsPackage)
    def _backing_block(self, n):
        r = (((n + 0xAA) // 0xAA) << self.sex) + n
        if n < 0xAA:
            return r
        if n < 0x70E4:
            return r + (((n + 0x70E4) // 0x70E4) << self.sex)
        return (1 << self.sex) + r + (((n + 0x70E4) // 0x70E4) << self.sex)

    def block_offset(self, n):
        return (self._backing_block(n) << 12) + self.first_table

    def _table_block(self, level, n):
        if level == 0:
            if n < 0xAA:
                return 0
            k = (n // 0xAA) * self.step[0] + (((n // 0x70E4) + 1) << self.sex)
            return k if n // 0x70E4 == 0 else k + (1 << self.sex)
        if level == 1:
            return self.step[0] if n < 0x70E4 else (1 << self.sex) + (n // 0x70E4) * self.step[1]
        return self.step[1]

    def _hash_entry(self, level, n):
        """Offset of block n's entry in its level-`level` hash table (the live copy)."""
        index = n % 0xAA if level == 0 else (n // 0xAA) % 0xAA if level == 1 else n // 0x70E4
        table = (self._table_block(level, n) << 12) + self.first_table
        if self.sex:
            if level == self.top:
                table += (self.block_separation & 2) << 11
            else:
                status = self._read(self._hash_entry(level + 1, n) + 0x14, 1)[0]
                table += (status & 0x40) << 6
        return table + index * 0x18

    def next_block(self, n):
        return u24be(self._read(self._hash_entry(0, n) + 0x15, 3), 0)

    def _blocks(self, start, count, consecutive):
        n = start
        for i in range(count):
            yield n
            n = n + 1 if consecutive else self.next_block(n)

    # ---- file table
    def _read_entries(self):
        entries = []
        for block in self._blocks(self.table_start, self.table_blocks, False):
            data = self._read(self.block_offset(block), BLOCK)
            for i in range(0, BLOCK, 0x40):
                e = data[i:i + 0x40]
                flags = e[0x28]
                length = flags & 0x3F
                if length == 0:
                    return entries
                entries.append({
                    "name": e[:length].decode("latin-1"), "dir": bool(flags & 0x80), "consecutive": bool(flags & 0x40),
                    "blocks": u24le(e, 0x29), "start": u24le(e, 0x2F),
                    "parent": struct.unpack_from(">h", e, 0x32)[0], "size": struct.unpack_from(">I", e, 0x34)[0]})
        return entries

    def path(self, index):
        parts, seen = [], set()
        while index >= 0 and index not in seen:
            seen.add(index)
            parts.append(self.entries[index]["name"])
            index = self.entries[index]["parent"]
        return "/".join(reversed(parts))

    def read_file(self, entry, out):
        left = entry["size"]
        for block in self._blocks(entry["start"], entry["blocks"], entry["consecutive"]):
            if left <= 0:
                break
            chunk = self._read(self.block_offset(block), min(BLOCK, left))
            out.write(chunk)
            left -= len(chunk)
        if left:
            raise IOError(f"{entry['name']}: {left} bytes past the end of the package")

    def extract(self, folder):
        os.makedirs(folder, exist_ok=True)
        for i, e in enumerate(self.entries):
            target = os.path.join(folder, *self.path(i).split("/"))
            if e["dir"]:
                os.makedirs(target, exist_ok=True)
                continue
            os.makedirs(os.path.dirname(target), exist_ok=True)
            with open(target, "wb") as out:
                self.read_file(e, out)


def is_package(path):
    try:
        with open(path, "rb") as f:
            return f.read(4) in MAGICS
    except OSError:
        return False


def main():
    if len(sys.argv) < 3 or sys.argv[1] not in ("info", "list", "extract") or (sys.argv[1] == "extract") != (len(sys.argv) == 4):
        raise SystemExit(__doc__)
    p = Package(sys.argv[2])
    if sys.argv[1] == "info":
        print(f"{p.magic} package: {p.display_name}")
        print(f"title {p.title_id:08X} media {p.media_id:08X} version {p.version:08X} content type {p.content_type:08X}")
        print(f"{len(p.entries)} entries, {p.allocated} blocks, {'male' if p.sex else 'female'}, hash levels {p.top + 1}")
    elif sys.argv[1] == "list":
        for i, e in enumerate(p.entries):
            print(("D" if e["dir"] else "F"), e["size"], "/" + p.path(i))
    else:
        p.extract(sys.argv[3])
        print(f"{sum(not e['dir'] for e in p.entries)} files extracted to {sys.argv[3]}")


if __name__ == "__main__":
    main()
