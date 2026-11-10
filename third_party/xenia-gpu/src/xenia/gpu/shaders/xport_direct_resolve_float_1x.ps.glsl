#version 460
#extension GL_EXT_samplerless_texture_functions : require
// direct resolve (vulkan_render_target_cache.cc, XportDirectResolve): writes a host render
// target straight into the destination texture's image, doing what the guest resolve and the
// texture load would do together (exp bias, red/blue swap), without going through guest memory.
// Single-sampled float/unorm source (sampled through its transfer view).

layout(set = 0, binding = 0) uniform texture2D xe_source;

layout(push_constant) uniform XeDirectResolveConstants {
  ivec2 source_offset;  // source pixel = destination pixel + source_offset
  uint flags;           // bit 0: swap red and blue
  float exp_bias_factor;
} xe_constants;

layout(location = 0) out vec4 xe_out;

void main() {
  vec4 color = texelFetch(xe_source, ivec2(gl_FragCoord.xy) + xe_constants.source_offset, 0);
  color *= xe_constants.exp_bias_factor;
  if ((xe_constants.flags & 1u) != 0u) {
    color = color.bgra;
  }
  xe_out = color;
}
