#version 460
#extension GL_EXT_samplerless_texture_functions : require
// direct resolve of a single-sampled 20e4 (float24) depth render target into a
// k_24_8_FLOAT texture (R32_SFLOAT on the host). Reproduces dump + resolve + texture load: the
// host depth holds guest depth / 2 ([0, 2) -> [0, 1)), the guest stores it as 20e4 (rounding to
// the nearest even if bit 1 of flags is set, else towards zero), the texture load converts it back
// to float32. Same arithmetic as XeFloat32To20e4 / XeFloat20e4To32 (pixel_formats.xesli).

layout(set = 0, binding = 0) uniform texture2D xe_depth;

layout(push_constant) uniform XeDirectResolveConstants {
  ivec2 source_offset;
  uint flags;
  float exp_bias_factor;
} xe_constants;

layout(location = 0) out vec4 xe_out;

uint XeFloat32To20e4(uint f32u32, bool round_to_nearest_even) {
  f32u32 = min((f32u32 <= 0x7FFFFFFFu) ? f32u32 : 0u, 0x3FFFFFF8u);
  uint denormalized =
      ((f32u32 & 0x7FFFFFu) | 0x800000u) >> min(113u - (f32u32 >> 23u), 24u);
  uint f24u32 = (f32u32 < 0x38800000u) ? denormalized : (f32u32 + 0xC8000000u);
  if (round_to_nearest_even) {
    f24u32 += 3u + ((f24u32 >> 3u) & 1u);
  }
  return (f24u32 >> 3u) & 0xFFFFFFu;
}

float XeFloat20e4To32(uint f24u32) {
  if (f24u32 == 0u) {
    return 0.0;
  }
  uint mantissa = f24u32 & 0xFFFFFu;
  uint exponent = f24u32 >> 20u;
  if (exponent == 0u) {
    uint mantissa_lzcnt = 20u - uint(findMSB(mantissa));
    exponent = 1u - mantissa_lzcnt;
    mantissa = (mantissa << mantissa_lzcnt) & 0xFFFFFu;
  }
  return uintBitsToFloat(((exponent + 112u) << 23u) | (mantissa << 3u));
}

void main() {
  float host_depth =
      texelFetch(xe_depth, ivec2(gl_FragCoord.xy) + xe_constants.source_offset, 0).r;
  uint f24 = XeFloat32To20e4(floatBitsToUint(host_depth * 2.0), (xe_constants.flags & 2u) != 0u);
  xe_out = vec4(XeFloat20e4To32(f24), 0.0, 0.0, 1.0);
}
