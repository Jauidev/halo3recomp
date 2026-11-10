#version 460
#extension GL_EXT_samplerless_texture_functions : require
// direct resolve of a 20e4 depth render target into a k_8_8_8_8 texture aliasing the
// same memory as the k_24_8_FLOAT one (the title reads the stencil and the depth bytes this way). The
// guest word is (20e4 depth << 8) | stencil, which the texture sees as R = stencil, G/B/A = the
// depth bytes from low to high. See xport_direct_resolve_depth_float24.ps.glsl.

layout(set = 0, binding = 0) uniform texture2D xe_depth;
layout(set = 0, binding = 1) uniform utexture2D xe_stencil;

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

void main() {
  ivec2 coord = ivec2(gl_FragCoord.xy) + xe_constants.source_offset;
  float host_depth = texelFetch(xe_depth, coord, 0).r;
  uint f24 = XeFloat32To20e4(floatBitsToUint(host_depth * 2.0), (xe_constants.flags & 2u) != 0u);
  uint stencil = texelFetch(xe_stencil, coord, 0).r & 0xFFu;
  xe_out = vec4(float(stencil), float(f24 & 0xFFu), float((f24 >> 8u) & 0xFFu),
                float(f24 >> 16u)) * (1.0 / 255.0);
}
