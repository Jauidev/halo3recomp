"""Writes a SPIR-V binary as the C++ header format of the native renderer's built-in shaders
(third_party/rexglue/src/graphics/xnative/*_cs.h).

usage: py -3 -I tools/spirv_to_header.py <shader.spv> <output.h> <array name> <source name>
"""
import struct, sys

spv, out, name, source = sys.argv[1:5]
data = open(spv, "rb").read()
words = struct.unpack(f"<{len(data) // 4}I", data)
lines = ["#pragma once", "#include <cstdint>",
         f"// Generated from {source}; validated for Vulkan 1.2.",
         f"const uint32_t {name}[] = {{"]
for i in range(0, len(words), 8):
    lines.append("    " + " ".join(f"0x{w:08x}," for w in words[i:i + 8]))
lines.append("};")
open(out, "w", encoding="utf-8", newline="\n").write("\n".join(lines) + "\n")
print(f"{len(words)} words")
