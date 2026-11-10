// the graphics settings and the anti-aliasing / HDR controllers the native renderer and
// its hooks read, taken verbatim from LibertyRecomp's xport_app.cpp (GPL-3) lines 108-565; the
// rest of that file is their application (window, installer, multiplayer) and is not used.
#include <algorithm>
#include <array>
#include <atomic>
#include <mutex>
#include <string>
#include <string_view>

#include <rex/cvar.h>
#include <rex/platform.h>
#include <rex/logging.h>
#include <rex/graphics/xnative/anti_aliasing_policy.h>
#include <rex/graphics/xnative/hdr_policy.h>
#include <rex/graphics/xnative/supersampling_policy.h>
#include <rex/graphics/xnative/title_commands.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/system/interfaces/graphics.h>
#include <rex/ui/window.h>

#include "rexglue_glue.h"



// xport, Android: 30 FPS (2 refreshes of the 60 Hz panel), held by the dynamic draw distance
// below. Below the cap frames reach the panel after 2, 3 or 4 refreshes and driving judders; 20
// (files/frame_limit) keeps every frame at 3 refreshes when 30 is out of reach.
REXCVAR_DEFINE_UINT32(xport_frame_limit, REX_PLATFORM_ANDROID ? 30 : 60, "Xcompiler/Graphics/Display",
                      "Maximum game and host presentation rate: 0 is unlocked; caps are 20, 30, 40, "
                      "60, and 120 FPS")
    .allowed({"0", "20", "30", "40", "60", "120"});
REXCVAR_DEFINE_STRING(xnative_hdr_mode, "off", "Xcompiler/Graphics/HDR",
                      "HDR output: off, scRGB, or Auto HDR")
    .allowed({"off", "scrgb", "auto_hdr"});
REXCVAR_DEFINE_BOOL(xnative_hdr_mode_unified, false,
                    "Xcompiler/Graphics/HDR/Compatibility",
                    "Canonical HDR mode has replaced the legacy Vulkan HDR toggle");
REXCVAR_DEFINE_DOUBLE(xnative_hdr_paper_white_nits, 203.0,
                      "Xcompiler/Graphics/HDR",
                      "SDR reference white brightness used by scRGB and Auto HDR")
    .range(80.0, 500.0);
REXCVAR_DEFINE_DOUBLE(xnative_hdr_peak_nits, 400.0, "Xcompiler/Graphics/HDR",
                      "Auto HDR highlight target brightness")
    .range(80.0, 2000.0);
REXCVAR_DEFINE_DOUBLE(xnative_auto_hdr_shoulder_start, 0.0,
                      "Xcompiler/Graphics/HDR/Advanced",
                      "Normalized luminance where Auto HDR highlight expansion begins")
    .range(0.0, 1.0);
REXCVAR_DEFINE_DOUBLE(xnative_auto_hdr_shoulder_power, 2.5,
                      "Xcompiler/Graphics/HDR/Advanced",
                      "Auto HDR highlight shoulder exponent")
    .range(1.0, 10.0);

REXCVAR_DEFINE_STRING(xnative_anti_aliasing, REX_PLATFORM_ANDROID ? "fxaa" : "smaa",
                      "Xcompiler/Graphics/Anti-Aliasing",
                      "Anti-aliasing: off, fxaa, smaa, deferred MSAA, or even-factor SSAA")
    // `spatial` remains loadable as a deprecated compatibility alias. It is
    // never exposed by the frontend or accepted by the controller setter.
    .allowed({"off", "fxaa", "smaa", "taa", "metalfx_taa", "msaa2x", "msaa4x", "msaa4x_smaa", "ssaa2x", "ssaa4x",
              "ssaa6x", "ssaa8x", "ssaa10x", "ssaa12x", "ssaa14x", "ssaa16x",
              "spatial"});
REXCVAR_DEFINE_BOOL(xnative_anti_aliasing_unified, false,
                    "Xcompiler/Graphics/Anti-Aliasing/Compatibility",
                    "Canonical unified anti-aliasing selection has replaced legacy split values");
REXCVAR_DEFINE_STRING(xnative_smaa_quality, "high", "Xcompiler/Graphics/Anti-Aliasing",
                      "SMAA 1x preset: low, medium, high, or ultra")
    .allowed({"low", "medium", "high", "ultra"});

namespace {

std::mutex g_anti_aliasing_controller_mutex;
std::atomic<bool> g_anti_aliasing_controller_initialized{false};
std::atomic<rex::graphics::xnative::AntiAliasingMode>
    g_configured_anti_aliasing_mode{rex::graphics::xnative::AntiAliasingMode::kSmaa};
std::atomic<rex::graphics::xnative::AntiAliasingMode>
    g_active_anti_aliasing_mode{rex::graphics::xnative::AntiAliasingMode::kSmaa};
std::mutex g_hdr_controller_mutex;
std::atomic<bool> g_hdr_controller_initialized{false};
std::atomic<rex::graphics::xnative::HdrMode> g_configured_hdr_mode{
    rex::graphics::xnative::HdrMode::kOff};

const char* AntiAliasingCompatibilityName(
    rex::graphics::xnative::AntiAliasingCompatibility compatibility) {
  using Compatibility = rex::graphics::xnative::AntiAliasingCompatibility;
  switch (compatibility) {
    case Compatibility::kCanonical:
      return "canonical";
    case Compatibility::kSpatialAlias:
      return "spatial-alias";
    case Compatibility::kLegacySceneMsaa:
      return "legacy-scene-msaa";
    case Compatibility::kLegacySpatialToggle:
      return "legacy-spatial-toggle";
    case Compatibility::kFallbackOff:
      return "fallback-off";
  }
  return "fallback-off";
}

void InitializeFrontendAntiAliasingControllerLocked() {
  using namespace rex::graphics::xnative;
  if (g_anti_aliasing_controller_initialized.load(std::memory_order_acquire)) {
    return;
  }
  const std::string configured = REXCVAR_GET(xnative_anti_aliasing);
  const std::string legacy_scene_msaa = rex::cvar::GetFlagByName("xnative_msaa");
  const auto resolved = ResolveAntiAliasingConfiguration(
      configured, legacy_scene_msaa,
      rex::cvar::Query<bool>("xnative_spatial_aa"),
      REXCVAR_GET(xnative_anti_aliasing_unified));
  g_configured_anti_aliasing_mode.store(resolved.mode, std::memory_order_release);
  g_active_anti_aliasing_mode.store(resolved.mode, std::memory_order_release);
  g_anti_aliasing_controller_initialized.store(true, std::memory_order_release);
  const auto route = GetAntiAliasingRoute(resolved.mode);
  REXLOG_INFO(
      "AAPolicy owner=frontend event=initialize configured-raw={} legacy-msaa={} "
      "legacy-spatial={} unified={} configured={} active={} compatibility={} "
      "ignored-legacy-msaa={} route-fxaa={} route-smaa={} scene-samples={} ssaa-factor={}",
      configured, legacy_scene_msaa, rex::cvar::Query<bool>("xnative_spatial_aa"),
      REXCVAR_GET(xnative_anti_aliasing_unified), AntiAliasingModeName(resolved.mode),
      AntiAliasingModeName(resolved.mode), AntiAliasingCompatibilityName(resolved.compatibility),
      resolved.ignored_legacy_scene_msaa, route.presentation_fxaa, route.presentation_smaa,
      route.scene_sample_count, route.supersampling_pixel_factor);
}

const char* HdrCompatibilityName(
    rex::graphics::xnative::HdrCompatibility compatibility) {
  using Compatibility = rex::graphics::xnative::HdrCompatibility;
  switch (compatibility) {
    case Compatibility::kCanonical:
      return "canonical";
    case Compatibility::kLegacyVulkanHdr:
      return "legacy-vulkan-hdr";
    case Compatibility::kFallbackOff:
      return "fallback-off";
  }
  return "fallback-off";
}

void InitializeFrontendHdrControllerLocked() {
  using namespace rex::graphics::xnative;
  if (g_hdr_controller_initialized.load(std::memory_order_acquire)) {
    return;
  }
  const std::string configured = REXCVAR_GET(xnative_hdr_mode);
  const bool legacy_vulkan_hdr = rex::cvar::Query<bool>("vulkan_hdr");
  const auto resolved = ResolveHdrConfiguration(
      configured, legacy_vulkan_hdr, REXCVAR_GET(xnative_hdr_mode_unified));
  const std::string_view resolved_name = HdrModeName(resolved.mode);
  const bool transport_enabled = HdrModeNeedsExtendedOutput(resolved.mode);
  const bool canonical_written =
      rex::cvar::SetFlagByName("xnative_hdr_mode", resolved_name);
  const bool marker_written =
      rex::cvar::SetFlagByName("xnative_hdr_mode_unified", "true");
  const bool transport_written = rex::cvar::SetFlagByName(
      "vulkan_hdr", transport_enabled ? "true" : "false");
  if (!canonical_written || !marker_written || !transport_written) {
    REXLOG_ERROR(
        "XPORTHDRPolicy event=initialize-failed configured-raw={} legacy-vulkan-hdr={} "
        "resolved={} canonical-written={} marker-written={} transport-written={}",
        configured, legacy_vulkan_hdr, resolved_name, canonical_written, marker_written,
        transport_written);
  }
  g_configured_hdr_mode.store(resolved.mode, std::memory_order_release);
  g_hdr_controller_initialized.store(true, std::memory_order_release);
  REXLOG_INFO(
      "XPORTHDRPolicy event=initialize configured-raw={} legacy-vulkan-hdr={} unified={} "
      "configured={} compatibility={} transport={}",
      configured, legacy_vulkan_hdr, REXCVAR_GET(xnative_hdr_mode_unified), resolved_name,
      HdrCompatibilityName(resolved.compatibility), transport_enabled);
}

bool ValidateSupersamplingSelection(
    rex::graphics::xnative::AntiAliasingMode mode) {
  using namespace rex::graphics::xnative;
  const uint32_t pixel_factor = GetAntiAliasingRoute(mode).supersampling_pixel_factor;
  if (pixel_factor == 1u) {
    return true;
  }

  // rexglue_glue's renderer and window instead of rex::Runtime's.
  auto* graphics = ::xnative::GraphicsSystem();
  auto* window = ::xnative::DisplayWindow();
  if (!graphics || graphics->GetTitleCommandAbi(kTitleId) != kTitleCommandAbi || !window) {
    REXLOG_ERROR(
        "XPORTSSAA event=reject-selection factor={} reason=device-capabilities-unavailable",
        pixel_factor);
    return false;
  }

  uint32_t output_width = window->GetActualPhysicalWidth();
  uint32_t output_height = window->GetActualPhysicalHeight();
  int32_t preset_width = 0;
  int32_t preset_height = 0;
  if (rex::graphics::video_mode_util::TryGetResolutionPresetFromCVar(preset_width,
                                                                     preset_height) &&
      preset_width > 0 && preset_height > 0) {
    output_width = uint32_t(preset_width);
    output_height = uint32_t(preset_height);
  }

  QueryDeviceCapabilitiesCommand command{};
  DeviceCapabilitiesResult capabilities{};
  if (!output_width || !output_height ||
      !graphics->ExecuteTitleCommand(kTitleId, kTitleCommandAbi, &command, sizeof(command),
                                     &capabilities, sizeof(capabilities))) {
    REXLOG_ERROR(
        "XPORTSSAA event=reject-selection factor={} output={}x{} "
        "reason=device-query-failed",
        pixel_factor, output_width, output_height);
    return false;
  }

  const auto physical = CalculateSupersampledExtent(
      output_width, output_height, pixel_factor, capabilities.max_image_dimension_2d);
  if (!physical) {
    REXLOG_ERROR(
        "XPORTSSAA event=reject-selection factor={} output={}x{} max-dimension={} "
        "reason=physical-extent-unsupported",
        pixel_factor, output_width, output_height, capabilities.max_image_dimension_2d);
    return false;
  }
  REXLOG_INFO(
      "XPORTSSAA event=validate-selection factor={} logical={}x{} physical={}x{} "
      "max-dimension={}",
      pixel_factor, output_width, output_height, physical->width, physical->height,
      capabilities.max_image_dimension_2d);
  return true;
}

}  // namespace

namespace rex::graphics::xnative {

void InitializeHdrController() {
  std::lock_guard lock(g_hdr_controller_mutex);
  InitializeFrontendHdrControllerLocked();
}

HdrMode GetConfiguredHdrMode() {
  InitializeHdrController();
  return g_configured_hdr_mode.load(std::memory_order_acquire);
}

std::string_view GetConfiguredHdrModeName() {
  return HdrModeName(GetConfiguredHdrMode());
}

bool SetConfiguredHdrMode(std::string_view value) {
  const auto requested = ParseHdrMode(value);
  if (!requested) {
    REXLOG_ERROR("XPORTHDRPolicy event=reject requested={} reason=invalid-mode", value);
    return false;
  }
  std::lock_guard lock(g_hdr_controller_mutex);
  InitializeFrontendHdrControllerLocked();
  const HdrMode previous =
      g_configured_hdr_mode.load(std::memory_order_acquire);
  const bool transport_enabled = HdrModeNeedsExtendedOutput(*requested);
  if (!rex::cvar::SetFlagByName("xnative_hdr_mode_unified", "true") ||
      !rex::cvar::SetFlagByName("xnative_hdr_mode", HdrModeName(*requested)) ||
      !rex::cvar::SetFlagByName("vulkan_hdr",
                                transport_enabled ? "true" : "false")) {
    REXLOG_ERROR("XPORTHDRPolicy event=reject requested={} reason=cvar-write", value);
    return false;
  }
  g_configured_hdr_mode.store(*requested, std::memory_order_release);
  REXLOG_INFO(
      "XPORTHDRPolicy event=set previous={} configured={} transport={} apply=live",
      HdrModeName(previous), HdrModeName(*requested), transport_enabled);
  return true;
}

void InitializeAntiAliasingController() {
  std::lock_guard lock(g_anti_aliasing_controller_mutex);
  InitializeFrontendAntiAliasingControllerLocked();
}

AntiAliasingMode GetConfiguredAntiAliasingMode() {
  InitializeAntiAliasingController();
  return g_configured_anti_aliasing_mode.load(std::memory_order_acquire);
}

AntiAliasingMode GetActiveAntiAliasingMode() {
  InitializeAntiAliasingController();
  return g_active_anti_aliasing_mode.load(std::memory_order_acquire);
}

std::string_view GetConfiguredAntiAliasingModeName() {
  return AntiAliasingModeName(GetConfiguredAntiAliasingMode());
}

std::string_view GetActiveAntiAliasingModeName() {
  return AntiAliasingModeName(GetActiveAntiAliasingMode());
}

AntiAliasingApplyResult SetConfiguredAntiAliasingMode(std::string_view value) {
  const auto requested = ParseAntiAliasingMode(value);
  if (!requested) {
    REXLOG_ERROR("AAPolicy owner=frontend event=reject requested={}", value);
    return AntiAliasingApplyResult::kRejected;
  }

  if (UsesTemporalAntiAliasing(*requested)) {
    REXLOG_WARN("AAPolicy: {} is not available", value);
    return AntiAliasingApplyResult::kRejected;
  }
  std::lock_guard lock(g_anti_aliasing_controller_mutex);
  InitializeFrontendAntiAliasingControllerLocked();
  const AntiAliasingMode active =
      g_active_anti_aliasing_mode.load(std::memory_order_acquire);
  const AntiAliasingMode previous_configured =
      g_configured_anti_aliasing_mode.load(std::memory_order_acquire);
  if (!ValidateSupersamplingSelection(*requested)) {
    return AntiAliasingApplyResult::kRejected;
  }
  const bool apply_live = CanApplyAntiAliasingLive(active, *requested);

  if (!rex::cvar::SetFlagByName("xnative_anti_aliasing_unified", "true")) {
    REXLOG_ERROR("AAPolicy owner=frontend event=reject requested={} reason=unified-marker",
                 value);
    return AntiAliasingApplyResult::kRejected;
  }
  if (!apply_live) {
    std::string_view legacy_scene_msaa = "original";
    if (*requested == AntiAliasingMode::kMsaa2x) {
      legacy_scene_msaa = "2x";
    } else if (GetAntiAliasingRoute(*requested).scene_sample_count == 4u) {
      legacy_scene_msaa = "4x";
    }
    // SetFlagByName intentionally records a pending restart even if the value
    // already matches. The active renderer route remains latched separately.
    if (!rex::cvar::SetFlagByName("xnative_msaa", legacy_scene_msaa)) {
      REXLOG_ERROR(
          "AAPolicy owner=frontend event=reject requested={} reason=restart-tracking",
          value);
      return AntiAliasingApplyResult::kRejected;
    }
  }
  if (!rex::cvar::SetFlagByName("xnative_anti_aliasing",
                                AntiAliasingModeName(*requested))) {
    REXLOG_ERROR("AAPolicy owner=frontend event=reject requested={} reason=canonical-cvar",
                 value);
    return AntiAliasingApplyResult::kRejected;
  }

  g_configured_anti_aliasing_mode.store(*requested, std::memory_order_release);
  if (apply_live) {
    g_active_anti_aliasing_mode.store(*requested, std::memory_order_release);
  }
  const AntiAliasingMode resulting_active =
      g_active_anti_aliasing_mode.load(std::memory_order_acquire);
  const auto route = GetAntiAliasingRoute(resulting_active);
  REXLOG_INFO(
      "AAPolicy owner=frontend event=set previous-configured={} requested={} active-before={} "
      "active-after={} apply={} route-fxaa={} route-smaa={} scene-samples={} ssaa-factor={}",
      AntiAliasingModeName(previous_configured), AntiAliasingModeName(*requested),
      AntiAliasingModeName(active), AntiAliasingModeName(resulting_active),
      apply_live ? "live" : "restart", route.presentation_fxaa, route.presentation_smaa,
      route.scene_sample_count, route.supersampling_pixel_factor);
  return apply_live ? AntiAliasingApplyResult::kAppliedLive
                    : AntiAliasingApplyResult::kRestartRequired;
}

}  // namespace rex::graphics::xnative
