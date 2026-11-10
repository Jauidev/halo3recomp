#pragma once

// DXT -> ETC2 transcoding for GPUs without BC sampling (the phone's Mali), so DXT
// textures stay compressed (4 or 8 bits per texel) instead of being decoded to A1R5G5B5 or
// R8G8B8A8. The decoded 4x4 block is encoded again:
//   - color: ETC1 differential mode of ETC2 RGB (individual only when the two subblock averages
//     are too far apart for a delta; ETC2 RGB8A1 with punchthrough alpha is differential only),
//     both flip orientations, base color = subblock average, best of the 8 modifier tables;
//   - alpha: EAC, every table with the multiplier from range/span and the centered base.
// The searches are kept small: the GPU runs this for every streamed DXT texture.
// texture_detile_cs.glsl has the GPU form with the same integer arithmetic and loop order, so
// both produce identical bytes (xnative_gpu_detile_validate compares them).
// Layouts: Khronos Data Format Specification 1.3, sections 22.1, 22.3 and 22.9. Texels are given
// row-major (index y * 4 + x); ETC2 numbers them column-major (x * 4 + y).

#include <algorithm>
#include <cstdint>

namespace rex::graphics::xnative::etc2 {

inline constexpr int32_t kColorModifiers[8][2] = {{2, 8},   {5, 17},  {9, 29},  {13, 42},
                                                  {18, 60}, {24, 80}, {33, 106}, {47, 183}};
inline constexpr int32_t kAlphaModifiers[16][8] = {
    {-3, -6, -9, -15, 2, 5, 8, 14}, {-3, -7, -10, -13, 2, 6, 9, 12},
    {-2, -5, -8, -13, 1, 4, 7, 12}, {-2, -4, -6, -13, 1, 3, 5, 12},
    {-3, -6, -8, -12, 2, 5, 7, 11}, {-3, -7, -9, -11, 2, 6, 8, 10},
    {-4, -7, -8, -11, 3, 6, 7, 10}, {-3, -5, -8, -11, 2, 4, 7, 10},
    {-2, -6, -8, -10, 1, 5, 7, 9},  {-2, -5, -8, -10, 1, 4, 7, 9},
    {-2, -4, -8, -10, 1, 3, 7, 9},  {-2, -5, -7, -10, 1, 4, 6, 9},
    {-3, -4, -7, -10, 2, 3, 6, 9},  {-1, -2, -3, -10, 0, 1, 2, 9},
    {-4, -6, -8, -9, 3, 5, 7, 8},   {-3, -5, -7, -9, 2, 4, 6, 8}};

inline int32_t Clamp255(int32_t value) { return std::clamp(value, 0, 255); }

// Weighted squared RGB distance (green counts most).
inline int32_t ColorError(int32_t r, int32_t g, int32_t b, const uint8_t* texel) {
  const int32_t dr = r - texel[0], dg = g - texel[1], db = b - texel[2];
  return 3 * dr * dr + 6 * dg * dg + db * db;
}

// Texels of subblock `sub` (0 or 1) for `flip`, as row-major indices.
inline void SubblockTexels(uint32_t flip, uint32_t sub, uint32_t out[8]) {
  for (uint32_t i = 0; i < 8; ++i) {
    const uint32_t x = flip ? (i & 3) : sub * 2 + (i >> 2);
    const uint32_t y = flip ? sub * 2 + (i >> 2) : (i & 3);
    out[i] = y * 4 + x;
  }
}

// Best table for one subblock with an expanded base color; returns the error, sets `table` and
// the 2-bit index of each of its texels (row-major `indices`). Transparent texels (punchthrough
// with `transparent_block`) take index 2 and add no error.
inline int32_t FitSubblock(const uint8_t texels[16][4], const uint32_t members[8], int32_t r,
                           int32_t g, int32_t b, bool punchthrough, bool transparent_block,
                           uint32_t& table, uint32_t indices[16]) {
  int32_t best = INT32_MAX;
  for (uint32_t t = 0; t < 8; ++t) {
    const int32_t inner = kColorModifiers[t][0], outer = kColorModifiers[t][1];
    // Index -> modifier: 0 +a, 1 +b, 2 -a, 3 -b; with transparency 0 is 0 and 2 is transparent.
    const int32_t modifiers[4] = {transparent_block ? 0 : inner, outer, -inner, -outer};
    int32_t error = 0;
    uint32_t chosen[8];
    for (uint32_t i = 0; i < 8; ++i) {
      const uint8_t* texel = texels[members[i]];
      if (punchthrough && texel[3] < 128) {
        chosen[i] = 2;
        continue;
      }
      int32_t texel_best = INT32_MAX;
      uint32_t texel_index = 0;
      for (uint32_t k = 0; k < 4; ++k) {
        if (transparent_block && k == 2) continue;
        const int32_t m = modifiers[k];
        const int32_t e = ColorError(Clamp255(r + m), Clamp255(g + m), Clamp255(b + m), texel);
        if (e < texel_best) {
          texel_best = e;
          texel_index = k;
        }
      }
      chosen[i] = texel_index;
      error += texel_best;
    }
    if (error < best) {
      best = error;
      table = t;
      for (uint32_t i = 0; i < 8; ++i) indices[members[i]] = chosen[i];
    }
  }
  return best;
}

inline int32_t Expand4(int32_t c) { return c * 17; }
inline int32_t Expand5(int32_t c) { return (c << 3) | (c >> 2); }

// One ETC2 RGB (punchthrough = false) or RGB8A1 (true) block, big-endian bytes.
inline void EncodeColorBlock(const uint8_t texels[16][4], bool punchthrough, uint8_t out[8]) {
  bool transparent_block = false;
  if (punchthrough) {
    for (uint32_t i = 0; i < 16; ++i) transparent_block |= texels[i][3] < 128;
  }
  int32_t best_error = INT32_MAX;
  uint32_t best_high = 0, best_low = 0;
  for (uint32_t flip = 0; flip < 2; ++flip) {
    uint32_t members[2][8];
    int32_t average[2][3];
    for (uint32_t sub = 0; sub < 2; ++sub) {
      SubblockTexels(flip, sub, members[sub]);
      int32_t sum[3] = {0, 0, 0}, count = 0;
      for (uint32_t i = 0; i < 8; ++i) {
        const uint8_t* texel = texels[members[sub][i]];
        if (punchthrough && texel[3] < 128) continue;
        for (uint32_t c = 0; c < 3; ++c) sum[c] += texel[c];
        ++count;
      }
      for (uint32_t c = 0; c < 3; ++c) average[sub][c] = count ? (sum[c] + count / 2) / count : 0;
    }
    // Mode 0 differential (5-bit base + 3-bit delta), mode 1 individual (two 4-bit bases), tried
    // only when the quantized averages do not fit a delta.
    bool delta_fits = true;
    for (uint32_t c = 0; c < 3; ++c) {
      const int32_t delta =
          (average[1][c] * 31 + 127) / 255 - (average[0][c] * 31 + 127) / 255;
      delta_fits &= delta >= -4 && delta <= 3;
    }
    for (uint32_t mode = 0; mode < (punchthrough || delta_fits ? 1u : 2u); ++mode) {
      const int32_t levels = mode == 0 ? 31 : 15;
      int32_t error = 0;
      int32_t base[2][3];
      uint32_t tables[2] = {0, 0};
      uint32_t indices[16] = {};
      for (uint32_t sub = 0; sub < 2; ++sub) {
        int32_t quantized[3];
        for (uint32_t c = 0; c < 3; ++c) {
          quantized[c] = (average[sub][c] * levels + 127) / 255;
          if (mode == 0 && sub == 1) {
            // The second differential base is the first plus a delta in [-4, 3].
            quantized[c] = std::clamp(quantized[c], std::max(base[0][c] - 4, 0),
                                      std::min(base[0][c] + 3, 31));
          }
        }
        int32_t expanded[3];
        for (uint32_t c = 0; c < 3; ++c) {
          base[sub][c] = quantized[c];
          expanded[c] = mode == 0 ? Expand5(quantized[c]) : Expand4(quantized[c]);
        }
        error += FitSubblock(texels, members[sub], expanded[0], expanded[1], expanded[2],
                             punchthrough, transparent_block, tables[sub], indices);
      }
      if (error >= best_error) continue;
      best_error = error;
      uint32_t high;
      if (mode == 0) {
        const auto delta = [&](uint32_t c) { return uint32_t(base[1][c] - base[0][c]) & 7u; };
        high = (uint32_t(base[0][0]) << 27) | (delta(0) << 24) | (uint32_t(base[0][1]) << 19) |
               (delta(1) << 16) | (uint32_t(base[0][2]) << 11) | (delta(2) << 8);
        // Bit 33: 'diff' in ETC2 RGB, 'opaque' in RGB8A1 (which is always differential).
        high |= punchthrough ? (transparent_block ? 0u : 2u) : 2u;
      } else {
        high = (uint32_t(base[0][0]) << 28) | (uint32_t(base[1][0]) << 24) |
               (uint32_t(base[0][1]) << 20) | (uint32_t(base[1][1]) << 16) |
               (uint32_t(base[0][2]) << 12) | (uint32_t(base[1][2]) << 8);
      }
      high |= (tables[0] << 5) | (tables[1] << 2) | flip;
      uint32_t low = 0;
      for (uint32_t i = 0; i < 16; ++i) {
        const uint32_t j = (i & 3) * 4 + (i >> 2);  // row-major -> ETC2's column-major
        low |= ((indices[i] >> 1) << (j + 16)) | ((indices[i] & 1) << j);
      }
      best_high = high;
      best_low = low;
    }
  }
  for (uint32_t i = 0; i < 4; ++i) {
    out[i] = uint8_t(best_high >> (24 - 8 * i));
    out[4 + i] = uint8_t(best_low >> (24 - 8 * i));
  }
}

// One EAC alpha block (the first half of an ETC2 RGBA8 block), big-endian bytes.
inline void EncodeAlphaBlock(const uint8_t alpha[16], uint8_t out[8]) {
  int32_t low = 255, high = 0;
  for (uint32_t i = 0; i < 16; ++i) {
    low = std::min<int32_t>(low, alpha[i]);
    high = std::max<int32_t>(high, alpha[i]);
  }
  int32_t best_error = INT32_MAX;
  uint32_t best_base = uint32_t(low), best_multiplier = 1, best_table = 13;
  uint32_t best_indices[16];
  for (uint32_t i = 0; i < 16; ++i) best_indices[i] = 4;  // table 13 modifier 0: exact if flat
  if (low != high) {
    for (uint32_t t = 0; t < 16; ++t) {
      const int32_t table_low = kAlphaModifiers[t][3], table_high = kAlphaModifiers[t][7];
      const int32_t span = table_high - table_low;
      const int32_t m = std::clamp((high - low + span / 2) / span, 1, 15);
      const int32_t base = Clamp255(((low + high) - (table_low + table_high) * m + 1) >> 1);
      int32_t error = 0;
      uint32_t chosen[16];
      for (uint32_t i = 0; i < 16; ++i) {
        int32_t texel_best = INT32_MAX;
        for (uint32_t k = 0; k < 8; ++k) {
          const int32_t d = Clamp255(base + kAlphaModifiers[t][k] * m) - alpha[i];
          if (d * d < texel_best) {
            texel_best = d * d;
            chosen[i] = k;
          }
        }
        error += texel_best;
      }
      if (error < best_error) {
        best_error = error;
        best_base = uint32_t(base);
        best_multiplier = uint32_t(m);
        best_table = t;
        for (uint32_t i = 0; i < 16; ++i) best_indices[i] = chosen[i];
      }
    }
  }
  uint64_t bits = (uint64_t(best_base) << 56) | (uint64_t(best_multiplier) << 52) |
                  (uint64_t(best_table) << 48);
  for (uint32_t i = 0; i < 16; ++i) {
    const uint32_t j = (i & 3) * 4 + (i >> 2);
    bits |= uint64_t(best_indices[i]) << (45 - 3 * j);
  }
  for (uint32_t i = 0; i < 8; ++i) out[i] = uint8_t(bits >> (56 - 8 * i));
}

}  // namespace rex::graphics::xnative::etc2
