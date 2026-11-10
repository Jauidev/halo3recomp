#version 460
#extension GL_EXT_samplerless_texture_functions : require
// direct resolve from a single-sampled k_32_FLOAT / k_32_32_FLOAT render target, whose
// transfer view is integer (the bits of the floats). See xport_direct_resolve_float_1x.ps.glsl.

layout(set = 0, binding = 0) uniform utexture2D xe_source;

layout(push_constant) uniform XeDirectResolveConstants {
  ivec2 source_offset;
  uint flags;
  float exp_bias_factor;
} xe_constants;

layout(location = 0) out vec4 xe_out;

void main() {
  uvec4 bits = texelFetch(xe_source, ivec2(gl_FragCoord.xy) + xe_constants.source_offset, 0);
  xe_out = vec4(uintBitsToFloat(bits.x), uintBitsToFloat(bits.y), 0.0, 1.0) *
           xe_constants.exp_bias_factor;
}
