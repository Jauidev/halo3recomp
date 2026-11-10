#version 460
#extension GL_EXT_samplerless_texture_functions : require
// direct resolve from a multisampled float/unorm render target (see
// xport_direct_resolve_float_1x.ps.glsl). Bits 8-11 of flags: first sample, bits 12-15: number of
// samples averaged (the guest copy sample select).

layout(set = 0, binding = 0) uniform texture2DMS xe_source;

layout(push_constant) uniform XeDirectResolveConstants {
  ivec2 source_offset;
  uint flags;
  float exp_bias_factor;
} xe_constants;

layout(location = 0) out vec4 xe_out;

void main() {
  ivec2 coord = ivec2(gl_FragCoord.xy) + xe_constants.source_offset;
  int first_sample = int((xe_constants.flags >> 8u) & 15u);
  int sample_count = int((xe_constants.flags >> 12u) & 15u);
  vec4 color = vec4(0.0);
  for (int i = 0; i < sample_count; ++i) {
    color += texelFetch(xe_source, coord, first_sample + i);
  }
  color *= xe_constants.exp_bias_factor / float(sample_count);
  if ((xe_constants.flags & 1u) != 0u) {
    color = color.bgra;
  }
  xe_out = color;
}
