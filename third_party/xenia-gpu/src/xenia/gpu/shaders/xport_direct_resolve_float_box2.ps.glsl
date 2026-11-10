#version 460
#extension GL_EXT_samplerless_texture_functions : require
// direct resolve of a single-sampled render target that the guest resolves as 4x MSAA
// with all samples averaged: in EDRAM a 4x pixel covers 2x2 pixels of the 1x surface, so the
// result is a 2x2 box downsample (the title's cheap half-resolution copy). source_offset is in the
// 4x pixel grid. See xport_direct_resolve_float_1x.ps.glsl.

layout(set = 0, binding = 0) uniform texture2D xe_source;

layout(push_constant) uniform XeDirectResolveConstants {
  ivec2 source_offset;
  uint flags;
  float exp_bias_factor;
} xe_constants;

layout(location = 0) out vec4 xe_out;

void main() {
  ivec2 coord = (ivec2(gl_FragCoord.xy) + xe_constants.source_offset) * 2;
  vec4 color = texelFetch(xe_source, coord, 0) + texelFetch(xe_source, coord + ivec2(1, 0), 0) +
               texelFetch(xe_source, coord + ivec2(0, 1), 0) + texelFetch(xe_source, coord + ivec2(1, 1), 0);
  color *= xe_constants.exp_bias_factor * 0.25;
  if ((xe_constants.flags & 1u) != 0u) {
    color = color.bgra;
  }
  xe_out = color;
}
