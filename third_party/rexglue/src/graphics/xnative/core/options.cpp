#include <rex/graphics/xnative/options.h>



REXCVAR_DEFINE_BOOL(xnative_spatial_aa, true, "Xcompiler/Graphics/Anti-Aliasing",
                    "Legacy compatibility toggle for the native spatial edge resolve");

REXCVAR_DEFINE_BOOL(xnative_output_dither, true, "Xcompiler/Graphics/Post-Processing",
                    "Apply stable display-space dithering to reduce output banding");

REXCVAR_DEFINE_BOOL(xnative_hdr_high_precision, true, "Xcompiler/Graphics/HDR",
                    "Preserve the final display-ready resolve in FP16 while HDR is active");

REXCVAR_DEFINE_STRING(xport_texture_filtering, "trilinear", "Xcompiler/Graphics/Texture Filtering",
                      "Material texture filtering: bilinear or trilinear")
    .allowed({"bilinear", "trilinear"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(xport_anisotropic_filtering, "1x", "Xcompiler/Graphics/Texture Filtering",
                      "Material anisotropic filtering: 1x, 2x, 4x, 8x, or 16x")
    .allowed({"1x", "2x", "4x", "8x", "16x"})
    .lifecycle(rex::cvar::Lifecycle::kHotReload);

REXCVAR_DEFINE_STRING(xnative_msaa, "4x", "Xcompiler/Graphics/Anti-Aliasing",
                      "Legacy deferred-scene multisampling compatibility setting")
    .allowed({"off", "original", "2x", "4x"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);

REXCVAR_DEFINE_STRING(
    xnative_light_overrides, "pair", "Xcompiler/Graphics/Native Renderer",
    "Shader override selection: stock modules, legacy stage selection, or approved pipeline pairs")
    .allowed({"stock", "stage", "pair"})
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);



REXCVAR_DEFINE_UINT32(xnative_frames_in_flight, 2, "Xcompiler/Graphics/Native Renderer",
                      "Native renderer frame-resource slots")
    .range(1, 2)
    .lifecycle(rex::cvar::Lifecycle::kRequiresRestart);
