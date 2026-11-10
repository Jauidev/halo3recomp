#version 450
#extension GL_EXT_buffer_reference : require
#extension GL_EXT_buffer_reference_uvec2 : require

// GPU form of the texture capture's block copy (CaptureTextureResourceUncached:
// GetTiledOffset2D/3D or linear addressing, then CopySwapBlock): one
// invocation per destination block of one subresource. The source is the
// subresource's raw guest bytes; the destination is laid out exactly like the
// CPU path's payload, so the image copies that follow are unchanged. Blocks of
// 4, 8 or 16 bytes only (the CPU path keeps the others).

layout(local_size_x = 8, local_size_y = 8, local_size_z = 1) in;

layout(buffer_reference, std430, buffer_reference_align = 4) readonly buffer SourceWords {
  uint words[];
};
layout(buffer_reference, std430, buffer_reference_align = 4) writeonly buffer DestinationWords {
  uint words[];
};

layout(push_constant) uniform DetileJob {
  uvec2 source;             // raw subresource bytes (device address)
  uvec2 destination;        // converted subresource bytes (device address)
  uint block_width;         // blocks per row to write
  uint block_height;        // rows to write
  uint depth;               // slices (3D), else 1
  uint pitch_h;             // guest pitch in blocks
  uint pitch_v;             // guest vertical pitch in blocks
  uint flags;               // bit 0 tiled, bit 1 3D, bits 4-5 endian, bits 8-12 log2(block bytes)
  uint packed_x;            // packed-mip block offset in the guest tile
  uint packed_y;
  uint destination_pitch;   // host row pitch in blocks
  uint destination_slice;   // host slice size in blocks
} job;

// texture_util::GetTiledOffset2D.
int TiledOffset2D(int x, int y, uint pitch, int log2_bytes) {
  int p = int((pitch + 31u) & ~31u);
  int macro_bits = ((x >> 5) + (y >> 5) * (p >> 5)) << (log2_bytes + 7);
  int micro = ((x & 7) + ((y & 0xE) << 2)) << log2_bytes;
  int offset = macro_bits + ((micro & ~0xF) << 1) + (micro & 0xF) + ((y & 1) << 4);
  return ((offset & ~0x1FF) << 3) + ((y & 16) << 7) + ((offset & 0x1C0) << 2) +
         (((((y & 8) >> 2) + (x >> 3)) & 3) << 6) + (offset & 0x3F);
}

// texture_util::GetTiledOffset3D.
int TiledOffset3D(int x, int y, int z, uint pitch, uint height, int log2_bytes) {
  int p = int((pitch + 31u) & ~31u);
  int h = int((height + 31u) & ~31u);
  int macro_outer = ((y >> 4) + (z >> 2) * (h >> 4)) * (p >> 5);
  int macro_bits = ((((x >> 5) + macro_outer) << (log2_bytes + 6)) & 0xFFFFFFF) << 1;
  int micro = (((x & 7) + ((y & 6) << 2)) << (log2_bytes + 6)) >> 6;
  int offset_outer = ((y >> 3) + (z >> 2)) & 1;
  int offset1 = offset_outer + ((((x >> 3) + (offset_outer << 1)) & 3) << 1);
  int offset2 = ((macro_bits + (micro & ~15)) << 1) + (micro & 15) +
                ((z & 3) << (log2_bytes + 6)) + ((y & 1) << 4);
  int address = (offset1 & 1) << 3;
  address += (offset2 >> 6) & 7;
  address <<= 3;
  address += offset1 & ~1;
  address <<= 2;
  address += offset2 & ~511;
  address <<= 3;
  address += offset2 & 63;
  return address;
}

// CopySwapBlock on one little-endian 32-bit word (xenos::Endian).
uint Swap(uint word, uint endian) {
  if (endian == 1u) return ((word & 0x00FF00FFu) << 8) | ((word >> 8) & 0x00FF00FFu);  // 8in16
  if (endian == 2u) {                                                                   // 8in32
    return (word << 24) | ((word & 0xFF00u) << 8) | ((word >> 8) & 0xFF00u) | (word >> 24);
  }
  if (endian == 3u) return (word << 16) | (word >> 16);                                 // 16in32
  return word;
}

// DXT decoding for GPUs without BC sampling (the phone's Mali), the GPU form of
// texture_conversion's ConvertTexelDXT* (same integer arithmetic, so the texels are identical).
// flags bits 16-19: 1 DXT1 -> R8G8B8A8, 2 DXT1 -> A1R5G5B5, 3 DXT2/3 -> R8G8B8A8,
// 4 DXT4/5 -> R8G8B8A8, 5 DXT3A -> R8G8B8A8. One invocation per 4x4 guest block; the
// destination pitch and slice are in texels.
uvec4 Expand565(uint color) {
  uint r = (color >> 11) & 31u, g = (color >> 5) & 63u, b = color & 31u;
  return uvec4((r << 3) | (r >> 2), (g << 2) | (g >> 4), (b << 3) | (b >> 2), 255u);
}

// DecodeBC1Colors: the color of one texel of the 8-byte color block (c = endpoints word,
// s = selector word).
uvec4 DecodeBC1(uint c, uint s, bool three_color_mode_allowed, uint pixel) {
  uint c0 = c & 0xFFFFu, c1 = c >> 16;
  uvec4 p0 = Expand565(c0), p1 = Expand565(c1);
  uint selector = (s >> (pixel * 2u)) & 3u;
  if (selector == 0u) return p0;
  if (selector == 1u) return p1;
  if (c0 > c1 || !three_color_mode_allowed) {
    return selector == 2u ? uvec4((2u * p0.rgb + p1.rgb + 1u) / 3u, 255u)
                          : uvec4((p0.rgb + 2u * p1.rgb + 1u) / 3u, 255u);
  }
  return selector == 2u ? uvec4((p0.rgb + p1.rgb) / 2u, 255u) : uvec4(0u);
}

// ConvertTexelBC4ToR8 on an already swapped alpha block (words a0, a1).
uint DecodeBC4(uint a0, uint a1, uint pixel) {
  uint e0 = a0 & 0xFFu, e1 = (a0 >> 8) & 0xFFu;
  uint lo = (a0 >> 16) | (a1 << 16), hi = a1 >> 16;  // the 48 selector bits
  uint bit = pixel * 3u;
  uint selector;
  if (bit + 3u <= 32u) {
    selector = (lo >> bit) & 7u;
  } else if (bit >= 32u) {
    selector = (hi >> (bit - 32u)) & 7u;
  } else {
    selector = ((lo >> bit) | (hi << (32u - bit))) & 7u;
  }
  if (selector == 0u) return e0;
  if (selector == 1u) return e1;
  if (e0 > e1) return ((8u - selector) * e0 + (selector - 1u) * e1) / 7u;
  if (selector == 6u) return 0u;
  if (selector == 7u) return 255u;
  return ((6u - selector) * e0 + (selector - 1u) * e1) / 5u;
}

uint PackRGBA8(uvec4 c) { return c.r | (c.g << 8) | (c.b << 16) | (c.a << 24); }

uint PackA1R5G5B5(uvec4 c) {
  return (c.a != 0u ? 0x8000u : 0u) | ((c.r >> 3) << 10) | ((c.g >> 3) << 5) | (c.b >> 3);
}

// DXT -> ETC2 transcoding (flags bits 16-19: 6 DXT1 -> ETC2 RGB8A1, 7 DXT2/3,
// 8 DXT4/5 and 9 DXT3A -> ETC2 RGBA8), the GPU form of etc2_encode.h: same integer arithmetic
// and loop order, so the bytes match the CPU encoder's. Texels are packed RGBA8 words,
// row-major (y * 4 + x); ETC2 numbers them column-major. The destination pitch and slice are
// in blocks.
const int kColorModifiers[16] = int[16](2, 8, 5, 17, 9, 29, 13, 42, 18, 60, 24, 80, 33, 106,
                                        47, 183);
const int kAlphaModifiers[128] = int[128](
    -3, -6, -9, -15, 2, 5, 8, 14, -3, -7, -10, -13, 2, 6, 9, 12,
    -2, -5, -8, -13, 1, 4, 7, 12, -2, -4, -6, -13, 1, 3, 5, 12,
    -3, -6, -8, -12, 2, 5, 7, 11, -3, -7, -9, -11, 2, 6, 8, 10,
    -4, -7, -8, -11, 3, 6, 7, 10, -3, -5, -8, -11, 2, 4, 7, 10,
    -2, -6, -8, -10, 1, 5, 7, 9, -2, -5, -8, -10, 1, 4, 7, 9,
    -2, -4, -8, -10, 1, 3, 7, 9, -2, -5, -7, -10, 1, 4, 6, 9,
    -3, -4, -7, -10, 2, 3, 6, 9, -1, -2, -3, -10, 0, 1, 2, 9,
    -4, -6, -8, -9, 3, 5, 7, 8, -3, -5, -7, -9, 2, 4, 6, 8);

uint g_texels[16];

int Clamp255(int value) { return clamp(value, 0, 255); }

int ColorError(int r, int g, int b, uint texel) {
  int dr = r - int(texel & 0xFFu), dg = g - int((texel >> 8) & 0xFFu),
      db = b - int((texel >> 16) & 0xFFu);
  return 3 * dr * dr + 6 * dg * dg + db * db;
}

bool Transparent(uint texel) { return (texel >> 24) < 128u; }

uint SubblockTexel(uint flip, uint sub, uint i) {
  uint x = flip != 0u ? (i & 3u) : sub * 2u + (i >> 2);
  uint y = flip != 0u ? sub * 2u + (i >> 2) : (i & 3u);
  return y * 4u + x;
}

// FitSubblock: returns the error; table in `table`, the 2-bit indices of the subblock's texels
// in `chosen` (subblock order, 2 bits each).
int FitSubblock(uint flip, uint sub, int r, int g, int b, bool punchthrough,
                bool transparent_block, out uint table, out uint chosen) {
  int best = 0x7FFFFFFF;
  table = 0u;
  chosen = 0u;
  for (uint t = 0u; t < 8u; ++t) {
    int inner = kColorModifiers[t * 2u], outer = kColorModifiers[t * 2u + 1u];
    int modifiers[4] = int[4](transparent_block ? 0 : inner, outer, -inner, -outer);
    int error = 0;
    uint trial = 0u;
    for (uint i = 0u; i < 8u; ++i) {
      uint texel = g_texels[SubblockTexel(flip, sub, i)];
      if (punchthrough && Transparent(texel)) {
        trial |= 2u << (i * 2u);
        continue;
      }
      int texel_best = 0x7FFFFFFF;
      uint texel_index = 0u;
      for (uint k = 0u; k < 4u; ++k) {
        if (transparent_block && k == 2u) continue;
        int m = modifiers[k];
        int e = ColorError(Clamp255(r + m), Clamp255(g + m), Clamp255(b + m), texel);
        if (e < texel_best) {
          texel_best = e;
          texel_index = k;
        }
      }
      trial |= texel_index << (i * 2u);
      error += texel_best;
    }
    if (error < best) {
      best = error;
      table = t;
      chosen = trial;
    }
  }
  return best;
}

int Expand4(int c) { return c * 17; }
int Expand5(int c) { return (c << 3) | (c >> 2); }

// EncodeColorBlock: the block as (high, low) 32-bit halves of the big-endian 64-bit word.
uvec2 EncodeColorBlock(bool punchthrough) {
  bool transparent_block = false;
  if (punchthrough) {
    for (uint i = 0u; i < 16u; ++i) transparent_block = transparent_block || Transparent(g_texels[i]);
  }
  int best_error = 0x7FFFFFFF;
  uvec2 best_block = uvec2(0u);
  for (uint flip = 0u; flip < 2u; ++flip) {
    ivec3 average[2];
    for (uint sub = 0u; sub < 2u; ++sub) {
      ivec3 sum = ivec3(0);
      int count = 0;
      for (uint i = 0u; i < 8u; ++i) {
        uint texel = g_texels[SubblockTexel(flip, sub, i)];
        if (punchthrough && Transparent(texel)) continue;
        sum += ivec3(int(texel & 0xFFu), int((texel >> 8) & 0xFFu), int((texel >> 16) & 0xFFu));
        ++count;
      }
      average[sub] = count != 0 ? (sum + count / 2) / count : ivec3(0);
    }
    ivec3 delta_q = (average[1] * 31 + 127) / 255 - (average[0] * 31 + 127) / 255;
    bool delta_fits =
        all(greaterThanEqual(delta_q, ivec3(-4))) && all(lessThanEqual(delta_q, ivec3(3)));
    for (uint mode = 0u; mode < (punchthrough || delta_fits ? 1u : 2u); ++mode) {
      int levels = mode == 0u ? 31 : 15;
      int error = 0;
      ivec3 base[2] = ivec3[2](ivec3(0), ivec3(0));
      uint tables[2] = uint[2](0u, 0u);
      uint chosen[2] = uint[2](0u, 0u);
      for (uint sub = 0u; sub < 2u; ++sub) {
        bool tied = mode == 0u && sub == 1u;
        ivec3 low = tied ? max(base[0] - 4, ivec3(0)) : ivec3(0);
        ivec3 high = tied ? min(base[0] + 3, ivec3(31)) : ivec3(levels);
        ivec3 quantized = (average[sub] * levels + 127) / 255;
        if (tied) quantized = clamp(quantized, low, high);
        ivec3 expanded = mode == 0u
            ? ivec3(Expand5(quantized.x), Expand5(quantized.y), Expand5(quantized.z))
            : ivec3(Expand4(quantized.x), Expand4(quantized.y), Expand4(quantized.z));
        uint table, trial;
        error += FitSubblock(flip, sub, expanded.x, expanded.y, expanded.z, punchthrough,
                             transparent_block, table, trial);
        tables[sub] = table;
        chosen[sub] = trial;
        base[sub] = quantized;
      }
      if (error >= best_error) continue;
      best_error = error;
      uint high_word;
      if (mode == 0u) {
        uvec3 delta = uvec3(base[1] - base[0]) & 7u;
        high_word = (uint(base[0].x) << 27) | (delta.x << 24) | (uint(base[0].y) << 19) |
                    (delta.y << 16) | (uint(base[0].z) << 11) | (delta.z << 8);
        high_word |= punchthrough ? (transparent_block ? 0u : 2u) : 2u;
      } else {
        high_word = (uint(base[0].x) << 28) | (uint(base[1].x) << 24) |
                    (uint(base[0].y) << 20) | (uint(base[1].y) << 16) |
                    (uint(base[0].z) << 12) | (uint(base[1].z) << 8);
      }
      high_word |= (tables[0] << 5) | (tables[1] << 2) | flip;
      uint low_word = 0u;
      for (uint sub = 0u; sub < 2u; ++sub) {
        for (uint i = 0u; i < 8u; ++i) {
          uint texel = SubblockTexel(flip, sub, i);
          uint j = (texel & 3u) * 4u + (texel >> 2);
          uint index = (chosen[sub] >> (i * 2u)) & 3u;
          low_word |= ((index >> 1) << (j + 16u)) | ((index & 1u) << j);
        }
      }
      best_block = uvec2(high_word, low_word);
    }
  }
  return best_block;
}

// EncodeAlphaBlock (EAC) on the alpha of g_texels.
uvec2 EncodeAlphaBlock() {
  int low = 255, high = 0;
  for (uint i = 0u; i < 16u; ++i) {
    int a = int(g_texels[i] >> 24);
    low = min(low, a);
    high = max(high, a);
  }
  int best_error = 0x7FFFFFFF;
  uint best_base = uint(low), best_multiplier = 1u, best_table = 13u;
  uint best_indices_high = 0x9249u, best_indices_low = 0x24924924u;  // all index 4 (table 13: 0)
  if (low != high) {
    for (uint t = 0u; t < 16u; ++t) {
      int table_low = kAlphaModifiers[t * 8u + 3u], table_high = kAlphaModifiers[t * 8u + 7u];
      int span = table_high - table_low;
      {
        int m = clamp((high - low + span / 2) / span, 1, 15);
        int bases[1] = int[1](Clamp255(((low + high) - (table_low + table_high) * m + 1) >> 1));
        for (uint b = 0u; b < 1u; ++b) {
          int error = 0;
          uint indices_high = 0u, indices_low = 0u;  // 32 + 16 bits of the 48-bit index field
          for (uint i = 0u; i < 16u; ++i) {
            int a = int(g_texels[i] >> 24);
            int texel_best = 0x7FFFFFFF;
            uint chosen = 0u;
            for (uint k = 0u; k < 8u; ++k) {
              int d = Clamp255(bases[b] + kAlphaModifiers[t * 8u + k] * m) - a;
              if (d * d < texel_best) {
                texel_best = d * d;
                chosen = k;
              }
            }
            error += texel_best;
            uint j = (i & 3u) * 4u + (i >> 2);
            uint shift = 45u - 3u * j;  // bit position in the 64-bit word
            if (shift >= 32u) {
              indices_high |= chosen << (shift - 32u);
            } else {
              indices_low |= chosen << shift;
              if (shift > 29u) indices_high |= chosen >> (32u - shift);
            }
          }
          if (error < best_error) {
            best_error = error;
            best_base = uint(bases[b]);
            best_multiplier = uint(m);
            best_table = t;
            best_indices_high = indices_high;
            best_indices_low = indices_low;
          }
        }
      }
    }
  }
  return uvec2((best_base << 24) | (best_multiplier << 20) | (best_table << 16) |
                   (best_indices_high & 0xFFFFu),
               best_indices_low);
}

uint ByteSwap(uint word) {
  return (word << 24) | ((word & 0xFF00u) << 8) | ((word >> 8) & 0xFF00u) | (word >> 24);
}

// DXN, CTX1 and DXT5A -> EAC (flags bits 16-19: 10 DXN -> EAC RG11, 11 CTX1 -> EAC RG11,
// 12 DXT5A -> EAC R11), so these stay compressed instead of becoming R8G8 / R8 texels (half the
// memory and half the bytes the GPU reads). An EAC R11 block has the layout of the ETC2 RGBA8
// alpha block, and what EncodeAlphaBlock picks for an 8-bit value v reads back as the 11-bit
// 8 * v + 4 (within half an 8-bit step), so it encodes each channel; RG11 is the R block, then
// the G block. The decoded values are ConvertTexelDXNToR8G8's, ConvertTexelDXT5AToR8's and
// ConvertTexelCTX1ToR8G8's (with integer thirds). The CPU form is in CaptureTextureResourceUncached.
void TranscodeEacBlock(uint mode, uvec3 id, uint s, uint endian, SourceWords source,
                       DestinationWords destination) {
  uint w0 = Swap(source.words[s], endian), w1 = Swap(source.words[s + 1u], endian);
  uint w2 = 0u, w3 = 0u;
  if (mode == 10u) {
    w2 = Swap(source.words[s + 2u], endian);
    w3 = Swap(source.words[s + 3u], endian);
  }
  uint block = id.z * job.destination_slice + id.y * job.destination_pitch + id.x;
  uint channels = mode == 12u ? 1u : 2u;
  for (uint channel = 0u; channel < channels; ++channel) {
    for (uint pixel = 0u; pixel < 16u; ++pixel) {
      uint value;
      if (mode == 11u) {
        // CTX1: bytes g0, r0, g1, r1, then 2-bit selectors (R in the higher bits).
        uint e0 = channel == 0u ? (w0 >> 8) & 0xFFu : w0 & 0xFFu;
        uint e1 = channel == 0u ? w0 >> 24 : (w0 >> 16) & 0xFFu;
        uint selector = (w1 >> (pixel * 2u)) & 3u;
        value = selector == 0u   ? e0
                : selector == 1u ? e1
                : selector == 2u ? (2u * e0 + e1) / 3u
                                 : (e0 + 2u * e1) / 3u;
      } else {
        value = channel == 0u ? DecodeBC4(w0, w1, pixel) : DecodeBC4(w2, w3, pixel);
      }
      g_texels[pixel] = value << 24;
    }
    uvec2 eac = EncodeAlphaBlock();
    uint word = (block * channels + channel) * 2u;
    destination.words[word] = ByteSwap(eac.x);
    destination.words[word + 1u] = ByteSwap(eac.y);
  }
}

void TranscodeBlock(uint mode, uvec3 id, uint s, uint endian, SourceWords source,
                    DestinationWords destination) {
  uint decode = mode == 6u ? 1u : mode == 7u ? 3u : mode == 8u ? 4u : 5u;
  uint w0 = Swap(source.words[s], endian), w1 = Swap(source.words[s + 1u], endian);
  uint w2 = 0u, w3 = 0u;
  if (decode == 3u || decode == 4u) {
    w2 = Swap(source.words[s + 2u], endian);
    w3 = Swap(source.words[s + 3u], endian);
  }
  for (uint pixel = 0u; pixel < 16u; ++pixel) {
    uvec4 c;
    if (decode == 1u) {
      c = DecodeBC1(w0, w1, true, pixel);
    } else if (decode == 3u) {
      c = DecodeBC1(w2, w3, false, pixel);
      uint nibble = pixel < 8u ? (w0 >> (pixel * 4u)) : (w1 >> ((pixel - 8u) * 4u));
      c.a = (nibble & 15u) * 17u;
    } else if (decode == 4u) {
      c = DecodeBC1(w2, w3, false, pixel);
      c.a = DecodeBC4(w0, w1, pixel);
    } else {
      uint nibble = pixel < 8u ? (w0 >> (pixel * 4u)) : (w1 >> ((pixel - 8u) * 4u));
      c = uvec4(0u, 0u, 0u, (nibble & 15u) * 17u);
    }
    g_texels[pixel] = PackRGBA8(c);
  }
  uint block = id.z * job.destination_slice + id.y * job.destination_pitch + id.x;
  if (mode == 6u) {
    uvec2 color = EncodeColorBlock(true);
    destination.words[block * 2u] = ByteSwap(color.x);
    destination.words[block * 2u + 1u] = ByteSwap(color.y);
    return;
  }
  uvec2 alpha = EncodeAlphaBlock();
  uvec2 color = EncodeColorBlock(false);
  destination.words[block * 4u] = ByteSwap(alpha.x);
  destination.words[block * 4u + 1u] = ByteSwap(alpha.y);
  destination.words[block * 4u + 2u] = ByteSwap(color.x);
  destination.words[block * 4u + 3u] = ByteSwap(color.y);
}

void DecodeBlock(uint mode, uvec3 id, uint s, uint endian, SourceWords source,
                 DestinationWords destination) {
  uint w0 = Swap(source.words[s], endian), w1 = Swap(source.words[s + 1u], endian);
  uint w2 = 0u, w3 = 0u;
  if (mode >= 3u && mode <= 4u) {
    w2 = Swap(source.words[s + 2u], endian);
    w3 = Swap(source.words[s + 3u], endian);
  }
  uint texel_bytes = mode == 2u ? 2u : 4u;
  for (uint row = 0u; row < 4u; ++row) {
    uint texel = id.z * job.destination_slice + (id.y * 4u + row) * job.destination_pitch + id.x * 4u;
    uvec4 c[4];
    for (uint x = 0u; x < 4u; ++x) {
      uint pixel = row * 4u + x;
      if (mode == 1u || mode == 2u) {
        c[x] = DecodeBC1(w0, w1, true, pixel);
      } else if (mode == 3u) {
        c[x] = DecodeBC1(w2, w3, false, pixel);
        uint nibble = pixel < 8u ? (w0 >> (pixel * 4u)) : (w1 >> ((pixel - 8u) * 4u));
        c[x].a = (nibble & 15u) * 17u;
      } else if (mode == 4u) {
        c[x] = DecodeBC1(w2, w3, false, pixel);
        c[x].a = DecodeBC4(w0, w1, pixel);
      } else {
        uint nibble = pixel < 8u ? (w0 >> (pixel * 4u)) : (w1 >> ((pixel - 8u) * 4u));
        c[x] = uvec4(0u, 0u, 0u, (nibble & 15u) * 17u);
      }
    }
    if (texel_bytes == 4u) {
      for (uint x = 0u; x < 4u; ++x) destination.words[texel + x] = PackRGBA8(c[x]);
    } else {
      uint d = texel >> 1;  // two 16-bit texels per word; texel is a multiple of 4
      destination.words[d] = PackA1R5G5B5(c[0]) | (PackA1R5G5B5(c[1]) << 16);
      destination.words[d + 1u] = PackA1R5G5B5(c[2]) | (PackA1R5G5B5(c[3]) << 16);
    }
  }
}

void main() {
  uvec3 id = gl_GlobalInvocationID;
  if (id.x >= job.block_width || id.y >= job.block_height || id.z >= job.depth) return;
  int log2_bytes = int((job.flags >> 8) & 31u);
  int sx = int(job.packed_x + id.x);
  int sy = int(job.packed_y + id.y);
  int sz = int(id.z);
  int source_offset;
  if ((job.flags & 1u) != 0u) {
    source_offset = (job.flags & 2u) != 0u
                        ? TiledOffset3D(sx, sy, sz, job.pitch_h, job.pitch_v, log2_bytes)
                        : TiledOffset2D(sx, sy, job.pitch_h, log2_bytes);
  } else {
    source_offset = int(((id.z * job.pitch_v + uint(sy)) * job.pitch_h + uint(sx)) << log2_bytes);
  }
  uint endian = (job.flags >> 4) & 3u;
  uint decode_mode = (job.flags >> 16) & 15u;
  if (decode_mode >= 10u) {
    TranscodeEacBlock(decode_mode, id, uint(source_offset) >> 2, endian, SourceWords(job.source),
                      DestinationWords(job.destination));
    return;
  }
  if (decode_mode >= 6u) {
    TranscodeBlock(decode_mode, id, uint(source_offset) >> 2, endian, SourceWords(job.source),
                   DestinationWords(job.destination));
    return;
  }
  if (decode_mode != 0u) {
    DecodeBlock(decode_mode, id, uint(source_offset) >> 2, endian, SourceWords(job.source),
                DestinationWords(job.destination));
    return;
  }
  uint destination_offset =
      (id.z * job.destination_slice + id.y * job.destination_pitch + id.x) << uint(log2_bytes);
  uint word_count = (1u << uint(log2_bytes)) >> 2;
  SourceWords source = SourceWords(job.source);
  DestinationWords destination = DestinationWords(job.destination);
  uint s = uint(source_offset) >> 2;
  uint d = destination_offset >> 2;
  for (uint i = 0u; i < word_count; ++i) {
    destination.words[d + i] = Swap(source.words[s + i], endian);
  }
}
