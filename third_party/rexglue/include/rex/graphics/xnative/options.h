#pragma once

#include <cstdint>
#include <string>
#include <rex/cvar.h>

// One registry owner in rexruntime; renderer plugins only consume these values.
REXCVAR_DECLARE(bool, xnative_spatial_aa);
REXCVAR_DECLARE(bool, xnative_output_dither);
REXCVAR_DECLARE(bool, xnative_hdr_high_precision);
REXCVAR_DECLARE(std::string, xport_texture_filtering);
REXCVAR_DECLARE(std::string, xport_anisotropic_filtering);
REXCVAR_DECLARE(std::string, xnative_msaa);
REXCVAR_DECLARE(std::string, xnative_light_overrides);
REXCVAR_DECLARE(uint32_t, xnative_frames_in_flight);
