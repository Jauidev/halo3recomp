#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>

#include <rex/graphics/xnative/title_commands.h>
#include <rex/graphics/xenos.h>

namespace rex::graphics::xnative {

struct GuestSurfaceView {
  uint32_t placement_base_tiles = 0;
  uint32_t sample_pitch = 0;
  uint32_t sample_width = 0;
  uint32_t sample_height = 0;
  xenos::MsaaSamples msaa_samples = xenos::MsaaSamples::k1X;
  bool depth = false;
  uint32_t format_class = 0;

  bool operator==(const GuestSurfaceView&) const = default;
};

struct GuestPlacementKey {
  uint32_t placement_base_tiles = 0;
  uint32_t sample_pitch = 0;
  uint32_t sample_width = 0;
  uint32_t sample_height = 0;
  bool depth = false;
  uint32_t format_class = 0;

  bool operator==(const GuestPlacementKey&) const = default;
};

struct GuestPlacementKeyHash {
  size_t operator()(const GuestPlacementKey& key) const noexcept {
    size_t hash = key.placement_base_tiles;
    hash = hash * size_t(16777619u) ^ key.sample_pitch;
    hash = hash * size_t(16777619u) ^ key.sample_width;
    hash = hash * size_t(16777619u) ^ key.sample_height;
    hash = hash * size_t(16777619u) ^ size_t(key.depth);
    hash = hash * size_t(16777619u) ^ key.format_class;
    return hash;
  }
};

constexpr uint32_t DecodeSurfaceSampleType(uint32_t surface_info_word) {
  return (surface_info_word >> 16) & 3u;
}

constexpr uint32_t GuestSampleScaleX(xenos::MsaaSamples samples) {
  return samples >= xenos::MsaaSamples::k4X ? 2u : 1u;
}

constexpr uint32_t GuestSampleScaleY(xenos::MsaaSamples samples) {
  return samples >= xenos::MsaaSamples::k2X ? 2u : 1u;
}

constexpr uint32_t CalculateNativeMipExtent(uint32_t base_extent, uint32_t level) {
  return level >= 32u ? 1u : std::max(1u, base_extent >> level);
}

constexpr bool DecodeGuestSurfaceView(const SurfaceDescriptor& descriptor, bool depth,
                                      GuestSurfaceView& view_out) {
  const uint32_t sample_type = DecodeSurfaceSampleType(descriptor.base);
  if (!descriptor.handle || !descriptor.width || !descriptor.height || sample_type > 2u) {
    view_out = {};
    return false;
  }
  const xenos::MsaaSamples samples = xenos::MsaaSamples(sample_type);
  const uint32_t scale_x = GuestSampleScaleX(samples);
  const uint32_t scale_y = GuestSampleScaleY(samples);
  view_out.placement_base_tiles = descriptor.address & 0x7FFu;
  view_out.sample_pitch = (descriptor.base & 0x3FFFu) * scale_x;
  view_out.sample_width = descriptor.width * scale_x;
  view_out.sample_height = descriptor.height * scale_y;
  view_out.msaa_samples = samples;
  view_out.depth = depth;
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
  // Gears of War 3 draws other formats over its scene's EDRAM, resolves them, and where they did not
  // draw gets the scene's bits back. A host conversion would lose them: each format has its image.
  // The 7e3 scene and its 2_10_10_10 views are the same bits (RecordSurfaceMaterialization converts).
  if (!depth) {
    const uint32_t edram_format = (descriptor.address >> 16) & 0xFu;
    view_out.format_class = edram_format == 2u || edram_format == 3u || edram_format == 10u || edram_format == 12u
                                ? 16u
                                : edram_format;
#if (XPORT_TITLE_ID == 0x4D5307E6)
    // Halo 3's second scene target is k_8_8_8_8 (4000F0E0) in the lighting pass and
    // k_8_8_8_8_GAMMA (4000F170) for the sun shadows and transparents at the same EDRAM, then
    // resolved through the k_8_8_8_8 view (bloom source 8282BAE8). Both are R8G8B8A8_UNORM here
    // (gamma kept linear): one image, or the shadows', sky's and transparents' writes are lost.
    if (edram_format == 1u) view_out.format_class = 0u;
#endif
  }
#endif
  return view_out.sample_pitch != 0;
}

constexpr GuestPlacementKey GetGuestPlacementKey(const GuestSurfaceView& view) {
  return {view.placement_base_tiles, view.sample_pitch, view.sample_width,
          view.sample_height, view.depth, view.format_class};
}

constexpr uint32_t NormalizeResolveSampleFlags(uint32_t flags, uint32_t sample_type) {
  if (flags & 0x70u) {
    return flags;
  }
  if (sample_type == uint32_t(xenos::MsaaSamples::k1X)) {
    return flags | 0x10u;
  }
  if (sample_type == uint32_t(xenos::MsaaSamples::k2X)) {
    return flags | 0x50u;
  }
  return flags | 0x70u;
}

constexpr xenos::CopySampleSelect DecodeResolveSampleSelect(uint32_t normalized_flags) {
  const uint32_t encoded = (normalized_flags >> 4) & 7u;
  const uint32_t decoded = encoded ? encoded - 1u : 0u;
  return xenos::CopySampleSelect(decoded <= uint32_t(xenos::CopySampleSelect::k0123)
                                     ? decoded
                                     : uint32_t(xenos::CopySampleSelect::k0123));
}

constexpr xenos::CopySampleSelect SanitizeGuestCopySampleSelect(
    xenos::CopySampleSelect sample_select, xenos::MsaaSamples samples, bool depth) {
  if (samples >= xenos::MsaaSamples::k4X) {
    if (sample_select > xenos::CopySampleSelect::k0123) {
      sample_select = xenos::CopySampleSelect::k0123;
    }
    if (depth) {
      if (sample_select == xenos::CopySampleSelect::k01 ||
          sample_select == xenos::CopySampleSelect::k0123) {
        sample_select = xenos::CopySampleSelect::k0;
      } else if (sample_select == xenos::CopySampleSelect::k23) {
        sample_select = xenos::CopySampleSelect::k2;
      }
    }
  } else if (samples >= xenos::MsaaSamples::k2X) {
    if (sample_select == xenos::CopySampleSelect::k2) {
      sample_select = xenos::CopySampleSelect::k0;
    } else if (sample_select == xenos::CopySampleSelect::k3) {
      sample_select = xenos::CopySampleSelect::k1;
    } else if (sample_select > xenos::CopySampleSelect::k01) {
      sample_select = xenos::CopySampleSelect::k01;
    }
    if (depth && sample_select == xenos::CopySampleSelect::k01) {
      sample_select = xenos::CopySampleSelect::k0;
    }
  } else {
    sample_select = xenos::CopySampleSelect::k0;
  }
  return sample_select;
}

}  // namespace rex::graphics::xnative
