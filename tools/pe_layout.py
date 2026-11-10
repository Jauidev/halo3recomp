"""Section layout of a dumped XEX image (xexinfo's <out.pe>), so the scan tools work on any title.
The headers are the original little-endian PE's; sections as (virtual address, virtual size)."""
import struct

BASE = int(__import__("os").environ.get("XPORT_IMAGE_BASE", "0x82000000"), 16)  # Halo 3: its DLLs load elsewhere


def _entries(pe):
    nt = struct.unpack_from("<I", pe, 0x3C)[0]
    count = struct.unpack_from("<H", pe, nt + 6)[0]
    table = nt + 24 + struct.unpack_from("<H", pe, nt + 20)[0]
    for i in range(count):
        entry = table + 40 * i
        name = pe[entry:entry + 8].rstrip(b"\0").decode("latin-1")
        size, address = struct.unpack_from("<II", pe, entry + 8)
        flags = struct.unpack_from("<I", pe, entry + 36)[0]
        yield name, BASE + address, size, flags


def sections(pe):
    return {name: (address, size) for name, address, size, _ in _entries(pe)}


def code_ranges(pe):
    """(start, end) of the executable sections: .text in every title, BINK and others only in some."""
    return [(address, address + size) for _, address, size, flags in _entries(pe) if flags & 0x20000020]
