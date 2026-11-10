#include <algorithm>
#include <array>
#include <atomic>
#include <bit>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstdint>
#include <cstring>
#include <filesystem>
#include <limits>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <type_traits>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include <fmt/format.h>
#include <xxhash.h>

#include <rex/cvar.h>
#include <rex/diagnostics/policy.h>
#include <rex/graphics/xnative/anti_aliasing_policy.h>
#include <rex/graphics/xnative/supersampling_policy.h>
#include <rex/graphics/video_mode_util.h>
#include <rex/graphics/xnative/shader_aliases.h>
#include <rex/graphics/xnative/shader_translator.h>
#include <rex/graphics/xnative/surface_view.h>
#include <rex/graphics/xnative/title_commands.h>
#include <rex/logging.h>
#include <rex/runtime.h>
#include <rex/ui/flags.h>
#include <rex/ui/window.h>

#include "xd3d_device.h"
#include "xd3d_init.h"

// runtime/os/thread_policy.h
void NameUnnamedCurrentThread(const char* name);

namespace {

using namespace rex::graphics::xnative;
using GuestFunction = void (*)(PPCContext&, uint8_t*);
// Device fields by where GTA IV's XDK keeps them (the numbers below), moved to this game's
// (tools/find_d3d.py writes xd3d_device.h).
using xd3d::DeviceOffset;
using xd3d::HasDeviceOffset;
// The hooks stand in for GTA IV's build of each function. With another XDK (xd3d::kReferenceXdk)
// they run the game's own instead and only tell the renderer what it did; the hooks that only
// stand in, without telling it anything, just run the game's.
constexpr bool kObserve = !xd3d::kReferenceXdk;
// Gears of War 3's XDK (2.0.20500) changed three of them: SetStreamSource has no offset (device,
// stream, buffer, stride, dirty mask), DrawIndexedVertices no base vertex (device, primitive,
// start index, index count), and LockTextureRect is inlined: XD3D_LockTextureRect is the worker
// that LockTextureRectArray and the engine call (texture, array index, level, flags, ...).
#if XPORT_TITLE_ID == 0x4D5308AB
#define XD3D_XDK_20500 1
#else
#define XD3D_XDK_20500 0
#endif
// What only the stand-ins call: another XDK's build may lack it (tools/find_d3d.py), and then
// nothing calls it (cmake/rexglue.cmake needs them all for GTA IV's).
void MissingGuestFunction(PPCContext& ctx, uint8_t*) { ctx.r3.u64 = 0; }
#ifndef XD3D_GuestFree
#define XD3D_GuestFree MissingGuestFunction
#endif
#ifndef XD3D_PhysicalAlloc
#define XD3D_PhysicalAlloc MissingGuestFunction
#endif
#ifndef XD3D_InitDeviceCpuState
#define XD3D_InitDeviceCpuState MissingGuestFunction
#endif
#ifndef XD3D_InitDisplay
#define XD3D_InitDisplay MissingGuestFunction
#endif
#ifndef XD3D_InitDisplayMode
#define XD3D_InitDisplayMode MissingGuestFunction
#endif
#ifndef XD3D_QueryPerformanceFrequency
#define XD3D_QueryPerformanceFrequency MissingGuestFunction
#endif
#ifndef XD3D_InitEngines
#define XD3D_InitEngines MissingGuestFunction
#endif
#ifndef XD3D_InitRing
#define XD3D_InitRing MissingGuestFunction
#endif
#ifndef XD3D_TilingRestoreState
#define XD3D_TilingRestoreState MissingGuestFunction
#endif
#ifndef XD3D_TilingRestoreConstants
#define XD3D_TilingRestoreConstants MissingGuestFunction
#endif
#ifndef XD3D_CommandListBindFences
#define XD3D_CommandListBindFences MissingGuestFunction
#endif
#ifndef XD3D_ADDR_TilingRestoreArgument
#define XD3D_ADDR_TilingRestoreArgument 0
#endif
#ifndef XD3D_ADDR_VdGlobalDevice
#define XD3D_ADDR_VdGlobalDevice 0
#endif
// The dirty words (device + 0 to 32), from this game's bits to GTA IV's and back (below).
using DirtyWords = std::array<uint64_t, 5>;
DirtyWords ToReferenceDirty(const DirtyWords& game);
DirtyWords ToGameDirty(const DirtyWords& reference);

constexpr uint32_t kOutOfMemory = 0x8007000E;
constexpr uint32_t kAlignedAllocationFreeFlags = 0x24800000;
constexpr uint32_t kFallbackAllocationFreeFlags = 0xB1800000;
constexpr uint32_t kVdGlobalDeviceImport = XD3D_ADDR_VdGlobalDevice;
constexpr uint32_t kStreamBufferBase = DeviceOffset(12452);
constexpr uint32_t kStreamSizeBase = DeviceOffset(1916);
constexpr uint32_t kStreamFetchBase = DeviceOffset(1912);
constexpr uint32_t kIndexBufferOffset = DeviceOffset(12428);
constexpr uint32_t kDepthStencilOffset = DeviceOffset(12448);
constexpr uint32_t kRenderTargetOffset = DeviceOffset(12432);
constexpr uint32_t kTextureHandleBase = DeviceOffset(12536);
constexpr uint32_t kTextureFetchBase = DeviceOffset(1152);
constexpr uint32_t kResourceFenceOffset = 8;
constexpr uint32_t kResourceSizeMask = 0x0FFFFFFF;
constexpr uint32_t kLiveResourceFenceOffset = DeviceOffset(10908);
constexpr uint32_t kPendingResourceMaskOffset = DeviceOffset(10912);
constexpr uint32_t kCommandScratchSize = 4800;
constexpr uint32_t kCommandScratchHeadOffset = DeviceOffset(13504);
constexpr uint32_t kCommandScratchWriteOffset = DeviceOffset(13508);
constexpr uint32_t kCommandScratchEndOffset = DeviceOffset(13512);
constexpr uint32_t kVertexDeclarationOffset = DeviceOffset(11812);
constexpr uint32_t kLegacyVertexDeclarationOffset = DeviceOffset(10456);
constexpr uint32_t kVertexDeclarationElementCountOffset = 0x18;
constexpr uint32_t kVertexDeclarationMaximumStreamOffset = 0x1C;
constexpr uint32_t kVertexDeclarationElementsOffset = 0x34;
constexpr uint32_t kVertexDeclarationMagic = 0x00100005;
constexpr size_t kVertexDeclarationDiagnosticLimit = 256;
constexpr uint32_t kUpCommandWriteOffset = DeviceOffset(13428);
constexpr uint32_t kUpVertexDataOffset = DeviceOffset(13432);
constexpr uint32_t kUpVertexWordCountOffset = DeviceOffset(13440);
constexpr uint32_t kSubmittedFrameOffset = DeviceOffset(16544);
// XD3D_Swap copies the incoming 52-byte display descriptor into the device
// slot; XD3D_PresentWorker later presents that persisted slot from its worker path.
constexpr uint32_t kPersistedPresentTextureOffset = DeviceOffset(14828);
constexpr uint32_t kPresentDescriptorSize = 52;
constexpr uint32_t kPresentFlag10941Offset = DeviceOffset(10941);
constexpr uint32_t kPresentFlag10942Offset = DeviceOffset(10942);
constexpr uint64_t kNativeDirtyMask16 = 0x000013A0001809E8;
constexpr uint64_t kNativeDirtyMask24 = 0x0040000880000000;
constexpr uint64_t kNativeDirtyMask32 = 0x0000000800080000;
constexpr uint32_t kReplayFetchStateOffset = DeviceOffset(0x480);
// All 26 Xenos texture-fetch slots are six dwords each. Cached draw replay
// must restore the same stage span consumed by the native renderer.
constexpr uint32_t kReplayFetchStateSize = 0x270;
constexpr uint32_t kReplayShaderConstantsOffset = DeviceOffset(0x780);
constexpr uint32_t kReplayShaderConstantsSize = 0x1E00;
// c8-c11, derived from kReplayShaderConstantsOffset + register * 16.
constexpr uint32_t kReplayVertexTransformConstantsOffset = kReplayShaderConstantsOffset + 0x80;
constexpr uint32_t kReplayVertexTransformConstantsSize = 0x40;
// Offsets within CapturedDrawSnapshot::shader_constants. Calculated by
// /tmp/calc_cached_snapshot_offsets.py rather than by hand.
constexpr uint32_t kReplayVertexTransformSnapshotOffset = 0x80;
constexpr uint32_t kReplayVertexBooleansOffset = DeviceOffset(0x2780);
constexpr uint32_t kReplayPixelBooleansOffset = DeviceOffset(0x2790);
constexpr uint32_t kReplayFixedStateOffset = DeviceOffset(0x28A0);
// Up to 0x28A0 + 0x8FC = 0x319C, the next field (it moves with the block's end).
constexpr uint32_t kReplayFixedStateSize = DeviceOffset(0x319C) - kReplayFixedStateOffset;
constexpr uint32_t kResourcePackedDimensionsOffset = 0x24;
constexpr uint32_t kResourceNonDimensionMask = 0x00000007;

struct PendingDrawPrimitiveUp {
  uint32_t device = 0;
  uint32_t primitive_type = 0;
  uint32_t vertex_count = 0;
  uint32_t stride = 0;
  uint32_t vertex_data = 0;
  uint32_t vertex_data_size = 0;
  NativeDirtyState dirty_state{};
};

thread_local PendingDrawPrimitiveUp g_pending_draw_primitive_up;

std::atomic<uint32_t> g_last_present_frontbuffer{0};
std::atomic<uint64_t> g_indexed_draw_invocation_id{0};

uint32_t LoadU32(uint8_t* base, uint32_t address);

struct CapturedDrawSnapshot {
  std::array<uint8_t, kReplayFetchStateSize> fetch_state{};
  std::array<uint8_t, kReplayShaderConstantsSize> shader_constants{};
  std::array<uint8_t, sizeof(uint32_t)> vertex_booleans{};
  std::array<uint8_t, sizeof(uint32_t)> pixel_booleans{};
  std::array<uint8_t, kReplayFixedStateSize> fixed_state{};
  bool valid = false;
};

struct CapturedNativeCommand {
  CommandType type = CommandType::kPresent;
  uint32_t selector_mask = 0;
  std::vector<uint8_t> bytes;
  std::vector<uint8_t> payload;
  CapturedDrawSnapshot draw_snapshot;
};

struct CachedNativeCommandList {
  std::vector<CapturedNativeCommand> commands;
};

struct ActiveNativeCommandCapture {
  bool active = false;
  uint8_t* base = nullptr;
  uint32_t build_object = 0;
  uint32_t selector_mask = 0;
  std::vector<CapturedNativeCommand> commands;
};

thread_local ActiveNativeCommandCapture g_active_native_capture;
std::mutex g_cached_native_command_lists_mutex;
std::unordered_map<uint32_t, std::shared_ptr<const CachedNativeCommandList>>
    g_cached_native_command_lists;
std::mutex g_vertex_declaration_diagnostic_mutex;
std::unordered_map<uint32_t, bool> g_logged_vertex_declarations;

struct NativeShaderBindingDiagnosticState {
  uint32_t vertex_shader = 0;
  uint32_t pixel_shader = 0;
};

std::mutex g_shader_binding_diagnostic_mutex;
std::unordered_map<uint32_t, NativeShaderBindingDiagnosticState> g_shader_bindings_by_device;

uint8_t* GuestPointer(uint8_t* base, uint32_t address) {
  return base + address + REX_PHYS_HOST_OFFSET(address);
}

void TrackNativeShaderBinding(uint32_t device, ShaderStage stage, uint32_t shader) {
  if (!rex::diagnostics::IsEnabled(rex::diagnostics::Category::kNativeTrace)) {
    return;
  }
  std::lock_guard lock(g_shader_binding_diagnostic_mutex);
  NativeShaderBindingDiagnosticState& state = g_shader_bindings_by_device[device];
  if (stage == ShaderStage::kVertex) {
    state.vertex_shader = shader;
  } else {
    state.pixel_shader = shader;
  }
}

NativeShaderBindingDiagnosticState CaptureNativeShaderBindings(uint32_t device) {
  std::lock_guard lock(g_shader_binding_diagnostic_mutex);
  const auto found = g_shader_bindings_by_device.find(device);
  return found != g_shader_bindings_by_device.end() ? found->second
                                                    : NativeShaderBindingDiagnosticState{};
}

void CaptureDrawState(uint8_t* base, uint32_t device, CapturedDrawSnapshot& snapshot) {
  if (!base || !device) {
    return;
  }

  std::memcpy(snapshot.fetch_state.data(), GuestPointer(base, device + kReplayFetchStateOffset),
              snapshot.fetch_state.size());
  std::memcpy(snapshot.shader_constants.data(),
              GuestPointer(base, device + kReplayShaderConstantsOffset),
              snapshot.shader_constants.size());
  std::memcpy(snapshot.vertex_booleans.data(),
              GuestPointer(base, device + kReplayVertexBooleansOffset),
              snapshot.vertex_booleans.size());
  std::memcpy(snapshot.pixel_booleans.data(),
              GuestPointer(base, device + kReplayPixelBooleansOffset),
              snapshot.pixel_booleans.size());
  std::memcpy(snapshot.fixed_state.data(), GuestPointer(base, device + kReplayFixedStateOffset),
              snapshot.fixed_state.size());
  snapshot.valid = true;
}

class ScopedReplayDrawState {
 public:
  ScopedReplayDrawState(uint8_t* base, uint32_t device, const CapturedDrawSnapshot& snapshot)
      : base_(base), device_(device), active_(base && device && snapshot.valid) {
    if (!active_) {
      return;
    }

    std::memcpy(backup_.fetch_state.data(), GuestPointer(base_, device_ + kReplayFetchStateOffset),
                backup_.fetch_state.size());
    std::memcpy(backup_.shader_constants.data(),
                GuestPointer(base_, device_ + kReplayShaderConstantsOffset),
                backup_.shader_constants.size());
    std::memcpy(backup_.vertex_booleans.data(),
                GuestPointer(base_, device_ + kReplayVertexBooleansOffset),
                backup_.vertex_booleans.size());
    std::memcpy(backup_.pixel_booleans.data(),
                GuestPointer(base_, device_ + kReplayPixelBooleansOffset),
                backup_.pixel_booleans.size());
    std::memcpy(backup_.fixed_state.data(), GuestPointer(base_, device_ + kReplayFixedStateOffset),
                backup_.fixed_state.size());

    std::memcpy(GuestPointer(base_, device_ + kReplayFetchStateOffset), snapshot.fetch_state.data(),
                snapshot.fetch_state.size());
    std::memcpy(GuestPointer(base_, device_ + kReplayShaderConstantsOffset),
                snapshot.shader_constants.data(), snapshot.shader_constants.size());
    std::memcpy(GuestPointer(base_, device_ + kReplayVertexBooleansOffset),
                snapshot.vertex_booleans.data(), snapshot.vertex_booleans.size());
    std::memcpy(GuestPointer(base_, device_ + kReplayPixelBooleansOffset),
                snapshot.pixel_booleans.data(), snapshot.pixel_booleans.size());
    std::memcpy(GuestPointer(base_, device_ + kReplayFixedStateOffset), snapshot.fixed_state.data(),
                snapshot.fixed_state.size());
  }

  ~ScopedReplayDrawState() {
    if (!active_) {
      return;
    }

    // Replay publishes the temporary constants to the renderer. Restoring the
    // CPU bytes alone leaves its delta state at the replay value. Mark exactly
    // the restored float groups and booleans for the next ordinary submission.
    // These are the same MSB-first, four-register groups as NativeConstantDirtyLayout.
    constexpr size_t kVertexBytes = 0x1000;
    constexpr size_t kPixelBytes = 0xE00;
    constexpr size_t kGroupBytes = 64;
    static_assert(kReplayShaderConstantsSize == kVertexBytes + kPixelBytes);
    std::array<uint64_t, 5> restored_dirty{};
    const uint8_t* applied = GuestPointer(base_, device_ + kReplayShaderConstantsOffset);
    const auto changed_groups = [&](size_t start, size_t size) {
      const uint8_t* before = applied + start;
      const uint8_t* after = backup_.shader_constants.data() + start;
      uint64_t mask = 0;
      if (std::memcmp(before, after, size) != 0) {
        for (size_t offset = 0; offset < size; offset += kGroupBytes) {
          if (std::memcmp(before + offset, after + offset, kGroupBytes) != 0) {
            mask |= (uint64_t{1} << 63) >> (offset / kGroupBytes);
          }
        }
      }
      return mask;
    };
    restored_dirty[0] = changed_groups(0, kVertexBytes);
    restored_dirty[1] = changed_groups(kVertexBytes, kPixelBytes);
    if (std::memcmp(GuestPointer(base_, device_ + kReplayVertexBooleansOffset),
                    backup_.vertex_booleans.data(), backup_.vertex_booleans.size()) != 0 ||
        std::memcmp(GuestPointer(base_, device_ + kReplayPixelBooleansOffset),
                    backup_.pixel_booleans.data(), backup_.pixel_booleans.size()) != 0) {
      restored_dirty[4] = uint64_t{1} << 56;
    }

    std::memcpy(GuestPointer(base_, device_ + kReplayFetchStateOffset), backup_.fetch_state.data(),
                backup_.fetch_state.size());
    std::memcpy(GuestPointer(base_, device_ + kReplayShaderConstantsOffset),
                backup_.shader_constants.data(), backup_.shader_constants.size());
    std::memcpy(GuestPointer(base_, device_ + kReplayVertexBooleansOffset),
                backup_.vertex_booleans.data(), backup_.vertex_booleans.size());
    std::memcpy(GuestPointer(base_, device_ + kReplayPixelBooleansOffset),
                backup_.pixel_booleans.data(), backup_.pixel_booleans.size());
    std::memcpy(GuestPointer(base_, device_ + kReplayFixedStateOffset), backup_.fixed_state.data(),
                backup_.fixed_state.size());

    // Keep pre-existing dirty work. The packed guest words are big-endian;
    // memcpy avoids adding alignment assumptions to the restoration scope.
    const DirtyWords game_dirty = ToGameDirty(restored_dirty);
    for (size_t word = 0; word < game_dirty.size(); ++word) {
      if (!game_dirty[word]) continue;
      uint8_t* destination = GuestPointer(base_, device_ + uint32_t(word * sizeof(uint64_t)));
      uint64_t guest_word;
      std::memcpy(&guest_word, destination, sizeof(guest_word));
      guest_word |= __builtin_bswap64(game_dirty[word]);
      std::memcpy(destination, &guest_word, sizeof(guest_word));
    }
  }

  ScopedReplayDrawState(const ScopedReplayDrawState&) = delete;
  ScopedReplayDrawState& operator=(const ScopedReplayDrawState&) = delete;

 private:
  uint8_t* base_ = nullptr;
  uint32_t device_ = 0;
  bool active_ = false;
  CapturedDrawSnapshot backup_;
};

uint8_t LoadU8(uint8_t* base, uint32_t address) {
  return *reinterpret_cast<volatile uint8_t*>(GuestPointer(base, address));
}

uint32_t LoadU32(uint8_t* base, uint32_t address) {
  return __builtin_bswap32(*reinterpret_cast<volatile uint32_t*>(GuestPointer(base, address)));
}

bool IsNativeMode();

constexpr uint32_t kRetailNullClearColorGlobal = XD3D_ADDR_NullClearColor;

uint64_t LoadU64(uint8_t* base, uint32_t address) {
  return __builtin_bswap64(*reinterpret_cast<volatile uint64_t*>(GuestPointer(base, address)));
}

void StoreU8(uint8_t* base, uint32_t address, uint8_t value) {
  *reinterpret_cast<volatile uint8_t*>(GuestPointer(base, address)) = value;
}

void StoreU16(uint8_t* base, uint32_t address, uint16_t value) {
  *reinterpret_cast<volatile uint16_t*>(GuestPointer(base, address)) = __builtin_bswap16(value);
}

void StoreU32(uint8_t* base, uint32_t address, uint32_t value) {
  *reinterpret_cast<volatile uint32_t*>(GuestPointer(base, address)) = __builtin_bswap32(value);
}

void StoreU64(uint8_t* base, uint32_t address, uint64_t value) {
  *reinterpret_cast<volatile uint64_t*>(GuestPointer(base, address)) = __builtin_bswap64(value);
}

// The dirty words hold this game's bits; the masks in this file and the renderer use GTA IV's
// (xd3d::kDirtyRuns says which is which).
constexpr uint64_t DirtyRunMask(const xd3d::DirtyRun& run) {
  return run.count >= 64 ? UINT64_MAX : (uint64_t{1} << run.count) - 1;
}

DirtyWords ToReferenceDirty(const DirtyWords& game) {
  DirtyWords reference{};
  for (const xd3d::DirtyRun& run : xd3d::kDirtyRuns) {
    reference[run.reference_word] |= ((game[run.word] >> run.bit) & DirtyRunMask(run)) << run.reference_bit;
  }
  return reference;
}

DirtyWords ToGameDirty(const DirtyWords& reference) {
  DirtyWords game{};
  for (const xd3d::DirtyRun& run : xd3d::kDirtyRuns) {
    game[run.word] |= ((reference[run.reference_word] >> run.reference_bit) & DirtyRunMask(run)) << run.bit;
  }
  return game;
}

// Sets GTA IV's mask of its word `word` in this game's words.
void MarkDirty(uint8_t* base, uint32_t device, uint32_t word, uint64_t mask) {
  DirtyWords reference{};
  reference[word] = mask;
  const DirtyWords game = ToGameDirty(reference);
  for (uint32_t index = 0; index < game.size(); ++index) {
    if (game[index]) {
      const uint32_t address = device + index * sizeof(uint64_t);
      StoreU64(base, address, LoadU64(base, address) | game[index]);
    }
  }
}

// The address of this game's word holding GTA IV's bits `mask` of word `word`: for a mask the
// title passes, already in this game's bits.
constexpr uint32_t GameDirtyWord(uint32_t device, uint32_t word, uint64_t mask) {
  for (const xd3d::DirtyRun& run : xd3d::kDirtyRuns) {
    if (run.reference_word == word && ((DirtyRunMask(run) << run.reference_bit) & mask)) {
      return device + run.word * uint32_t(sizeof(uint64_t));
    }
  }
  return device + word * uint32_t(sizeof(uint64_t));
}
// GTA IV keeps the fetch constants' bits (textures and vertex streams) in the low half of word 3.
constexpr uint64_t kReferenceFetchDirty = 0x00000000FFFFFFFF;

rex::system::IGraphicsSystem* GetNativeGraphicsSystem() {
  auto* graphics = ::xnative::GraphicsSystem();
  if (!graphics || graphics->GetTitleCommandAbi(kTitleId) != kTitleCommandAbi) {
    return nullptr;
  }
  return graphics;
}

bool IsNativeMode() {
  return GetNativeGraphicsSystem() != nullptr;
}

bool ShouldLogNativeHookCall(uint64_t call_count);

template <typename Command>
bool SubmitNativeCommand(const Command& command);

uint64_t NextNativeHookDiagnosticCall(std::atomic<uint64_t>& counter);

bool ShouldLogNativeHookCall(uint64_t call_count);

uint32_t EncodeSurfaceResourceDimensions(uint32_t original, uint32_t width, uint32_t height) {
  return (original & kResourceNonDimensionMask) | ((width - 1) << 18) | ((height - 1) << 3);
}

bool IsCachedListCommandType(CommandType type) {
  switch (type) {
    case CommandType::kSetRenderState:
    case CommandType::kSetPixelShader:
    case CommandType::kSetVertexShader:
    case CommandType::kSetVertexDeclaration:
    case CommandType::kSetTexture:
    case CommandType::kSetDepthStencil:
    case CommandType::kSetRenderTarget:
    case CommandType::kSetVertexStream:
    case CommandType::kSetIndexBuffer:
    case CommandType::kDrawPrimitive:
    case CommandType::kDrawPrimitiveUp:
    case CommandType::kDrawIndexedPrimitive:
    case CommandType::kResolve:
    case CommandType::kClear:
      return true;
    case CommandType::kDeviceCreated:
    case CommandType::kDeviceDestroyed:
    case CommandType::kRegisterShader:
    case CommandType::kRegisterVertexDeclaration:
    case CommandType::kResourceUnlock:
    case CommandType::kTextureLock:
    case CommandType::kPresent:
    case CommandType::kQueryDeviceCapabilities:
    case CommandType::kReleaseResource:
    case CommandType::kDepthSurfaceHandoff:
    case CommandType::kRegisterVirtualResource:
      return false;
  }
  return false;
}

template <typename Command>
bool CaptureNativeCommand(const Command& command) {
  if (!g_active_native_capture.active || !IsCachedListCommandType(command.header.type)) {
    return false;
  }

  CapturedNativeCommand captured{};
  captured.type = command.header.type;
  captured.selector_mask = g_active_native_capture.selector_mask;
  captured.bytes.resize(sizeof(command));
  std::memcpy(captured.bytes.data(), &command, sizeof(command));

  if constexpr (std::is_same_v<Command, DrawPrimitiveCommand> ||
                std::is_same_v<Command, DrawPrimitiveUpCommand> ||
                std::is_same_v<Command, DrawIndexedPrimitiveCommand>) {
    CaptureDrawState(g_active_native_capture.base, command.device, captured.draw_snapshot);
  }

  if constexpr (std::is_same_v<Command, DrawPrimitiveUpCommand>) {
    if (command.vertex_data && command.vertex_data_size) {
      captured.payload.resize(command.vertex_data_size);
      std::memcpy(captured.payload.data(),
                  GuestPointer(g_active_native_capture.base, command.vertex_data),
                  captured.payload.size());
    }
  }

  g_active_native_capture.commands.push_back(std::move(captured));
  return true;
}

template <typename Command>
bool SubmitNativeCommand(const Command& command) {
  if (CaptureNativeCommand(command)) {
    return true;
  }
  if (auto* graphics = GetNativeGraphicsSystem()) {
    const bool accepted =
        graphics->SubmitTitleCommand(kTitleId, kTitleCommandAbi, &command, sizeof(command));
    if (!accepted && rex::diagnostics::IsEnabled(rex::diagnostics::Category::kGuestHooks)) {
      static std::atomic<uint64_t> rejection_count{0};
      const uint64_t count = NextNativeHookDiagnosticCall(rejection_count);
      if (ShouldLogNativeHookCall(count)) {
        REXLOG_WARN("xnative-hook: rejected command #{} type={} size={}", count,
                    uint32_t(command.header.type), sizeof(command));
      }
    }
    return accepted;
  }
  return false;
}

bool ShouldLogNativeHookCall(uint64_t call_count);

bool ShouldLogNativeHookCall(uint64_t call_count) {
  return call_count != 0 && rex::diagnostics::IsEnabled(rex::diagnostics::Category::kGuestHooks) &&
         (call_count <= 32 || !(call_count % 4096));
}

uint64_t NextNativeHookDiagnosticCall(std::atomic<uint64_t>& counter) {
  if (!rex::diagnostics::IsEnabled(rex::diagnostics::Category::kGuestHooks)) {
    return 0;
  }
  return counter.fetch_add(1, std::memory_order_relaxed) + 1;
}

template <typename Command, typename Result>
bool ExecuteNativeCommand(const Command& command, Result& result) {
  if (auto* graphics = GetNativeGraphicsSystem()) {
    if constexpr (std::is_same_v<Command, TextureLockCommand>) {
    }
    return graphics->ExecuteTitleCommand(kTitleId, kTitleCommandAbi, &command, sizeof(command),
                                         &result, sizeof(result));
  }
  return false;
}

void DumpXenosShaderContainer(const uint8_t* container, uint32_t total_size,
                              uint64_t hash, ShaderStage stage,
                              const std::filesystem::path& dump_root) {
  static std::mutex dump_mutex;
  std::lock_guard lock(dump_mutex);

  const std::filesystem::path target_directory = dump_root / "xport_containers";
  std::error_code error;
  std::filesystem::create_directories(target_directory, error);
  if (error) {
    REXLOG_ERROR("the title shader dump: failed to create '{}': {}", target_directory.string(),
                 error.message());
    return;
  }

  const char* stage_extension = stage == ShaderStage::kVertex ? "vert" : "frag";
  // the registration order, which tells which .fxc a shader comes from.
  if (FILE* order = std::fopen((target_directory / "order.txt").string().c_str(), "a")) {
    std::fprintf(order, "%016llX %s\n", static_cast<unsigned long long>(hash), stage_extension);
    std::fclose(order);
  }
  const std::filesystem::path output_path =
      target_directory / fmt::format("xport_{:016X}.container.{}", hash, stage_extension);
  if (std::filesystem::exists(output_path, error) && !error) {
    return;
  }

  std::filesystem::path temporary_path = output_path;
  temporary_path += ".tmp";
  FILE* output = std::fopen(temporary_path.string().c_str(), "wb");
  if (!output) {
    REXLOG_ERROR("the title shader dump: failed to open '{}'", temporary_path.string());
    return;
  }

  const size_t bytes_written = std::fwrite(container, 1, total_size, output);
  const bool close_succeeded = std::fclose(output) == 0;
  if (bytes_written != total_size || !close_succeeded) {
    std::filesystem::remove(temporary_path, error);
    REXLOG_ERROR("the title shader dump: incomplete write for {:016X}", hash);
    return;
  }

  std::filesystem::rename(temporary_path, output_path, error);
  if (error) {
    std::filesystem::remove(temporary_path, error);
    REXLOG_ERROR("the title shader dump: failed to publish '{}'", output_path.string());
  }
}

uint32_t UpdateResourceLockCount(uint8_t* base, uint32_t resource, int32_t delta) {
  auto* raw_value = reinterpret_cast<uint32_t*>(GuestPointer(base, resource));
  uint32_t expected_raw = __atomic_load_n(raw_value, __ATOMIC_SEQ_CST);
  while (true) {
    const uint32_t previous = __builtin_bswap32(expected_raw);
    const uint32_t updated = previous + uint32_t(delta);
    const uint32_t desired_raw = __builtin_bswap32(updated);
    if (__atomic_compare_exchange_n(raw_value, &expected_raw, desired_raw, false, __ATOMIC_SEQ_CST,
                                    __ATOMIC_SEQ_CST)) {
      return previous;
    }
  }
}

struct NativeResourceLockState {
  uint32_t nesting = 0;
  uint32_t flags = 0;
  uint32_t minimum_offset = UINT32_MAX;
  uint32_t maximum_end = 0;
  uint32_t lock_caller = 0;
  bool guest_write = false;
};

std::mutex g_native_resource_lock_mutex;
std::unordered_map<uint32_t, NativeResourceLockState> g_native_resource_locks;

// Bink's video frames (Gears' intro, loading and cutscene movies) lock their Y/Cr/Cb textures
// once with this flag, then decode every frame straight into them, with no lock or unlock, as the
// Xbox GPU reads that memory. The renderer re-reads a texture only after an unlock, so the movie
// stayed on its first frame (white for the Microsoft logo). Each bind of such a texture
// (XD3D_SetTexture) now tells the renderer it was written; the renderer compares its content hash
// and uploads only what changed. UE3 also locks its own (DXT) textures with the flag when it
// streams them, and those are never written behind its back: only 8-bit textures (k_8, Bink's
// planes) are tracked, or every bind of them would be hashed again (~1500/s in gameplay).
// Guarded by g_native_resource_lock_mutex.
constexpr uint32_t kPersistentLockFlag = 0x10;
std::unordered_set<uint32_t> g_persistent_locks;

bool IsBinkPlaneTexture(uint8_t* base, uint32_t resource) {
  constexpr uint32_t kTextureType = 3;  // low 4 bits of the resource's first word
  constexpr uint32_t kFormat8 = 2;      // xenos::TextureFormat::k_8, fetch constant word 1
  return (LoadU32(base, resource) & 0xF) == kTextureType &&
         (LoadU32(base, resource + 32) & 0x3F) == kFormat8;
}

bool IsPersistentLock(uint32_t resource) {
  std::lock_guard lock(g_native_resource_lock_mutex);
  return g_persistent_locks.contains(resource);
}

// Halo 3's movie player decodes every frame straight into its Y/Cr/Cb/A planes (k_8 textures,
// two sets it alternates each frame) through pointers it took once at setup (sub_824512C8 locks
// each plane, writes, unlocks): when they are bound their lock count is 0 and no Unlock follows a
// frame. Only the renderer's sampled recheck (16 chunks every 4th frame) saw the new frames, and
// it misses small changes: planes stayed on old frames at random (a grey or green legal screen,
// the "BUNGIE" text green, frames of different moments mixed: flicker). The planes are bound by
// the engine's generic texture binder (SetTexture from 0x8220F7D4..0x8220F958), so they are told
// apart by their headers: the player's two sets are static, A at 829E7AEC/B20/B54/B88 and B at
// 829E7BC4/BF8/C2C/C60. Bound ones are remembered per stage (SetTexture comes only when a stage's
// texture changes), and every draw (TakeNativeDrawDirtyState) tells the renderer they were
// written, once a frame per stage; it compares the content hash and uploads only what changed.
// Render thread only, like the D3D calls.
bool IsMoviePlaneHeader(uint32_t texture) {
#if XPORT_TITLE_ID == 0x4D5307E6
  return texture >= 0x829E7AECu && texture <= 0x829E7C60u;
#else
  (void)texture;
  return false;
#endif
}

uint32_t g_bink_stage_mask = 0;
std::array<uint32_t, kTextureStageCount> g_bink_stage_texture{};
std::array<uint32_t, kTextureStageCount> g_bink_stage_notified{};  // frame + 1, 0 = not yet

void NoteBoundBinkPlane(uint8_t* base, uint32_t stage, uint32_t texture) {
  if (stage >= kTextureStageCount || stage >= 32) {
    return;
  }
  if (texture && IsMoviePlaneHeader(texture) && IsBinkPlaneTexture(base, texture)) {
    if (g_bink_stage_texture[stage] != texture) {
      g_bink_stage_texture[stage] = texture;
      g_bink_stage_notified[stage] = 0;
    }
    g_bink_stage_mask |= 1u << stage;
  } else {
    g_bink_stage_texture[stage] = 0;
    g_bink_stage_mask &= ~(1u << stage);
  }
}

void NotifyDrawnBinkPlanes(uint8_t* base, uint32_t device) {
  if (!g_bink_stage_mask) {
    return;
  }
  const uint32_t frame = LoadU32(base, device + kSubmittedFrameOffset) + 1;
  for (uint32_t mask = g_bink_stage_mask; mask; mask &= mask - 1) {
    const uint32_t stage = uint32_t(std::countr_zero(mask));
    if (g_bink_stage_notified[stage] == frame) {
      continue;
    }
    g_bink_stage_notified[stage] = frame;
    static uint32_t notes_logged = 0;
    if (notes_logged < 8) {
      ++notes_logged;
      REXLOG_INFO("xnative-bink: frame={} stage={} plane={:08X} rewritten every frame, re-read",
                  frame, stage, g_bink_stage_texture[stage]);
    }
    ResourceUnlockCommand written{};
    written.resource = g_bink_stage_texture[stage];
    written.access = ResourceUnlockAccess::kGuestWrite;
    written.flags = kPersistentLockFlag;
    SubmitNativeCommand(written);
  }
}

void UpdateGuestDirtyRange(uint8_t* base, uint32_t resource, uint32_t secondary_selector,
                           uint32_t primary_base, uint32_t secondary_base, uint32_t pointer,
                           uint32_t size, uint32_t flags) {
  // The retail lock routine uses bit zero as the no-dirty-update contract.
  // The DMA/cache-maintenance branches around this range bookkeeping target
  // the Xbox GPU and intentionally remain stubbed in the native renderer.
  if (flags & 1u) {
    return;
  }

  const bool secondary_range = secondary_selector && secondary_base;
  const uint32_t range_base = secondary_range ? secondary_base : primary_base;
  const uint32_t range_field = resource + (secondary_range ? 24u : 20u);
  const uint32_t offset = pointer - range_base;
  const uint32_t end = offset + size;
  const uint32_t first_block = offset >> 7;
  const uint32_t end_block = (end + 127u) >> 7;
  const uint32_t previous = LoadU32(base, range_field);
  const uint32_t previous_first = previous >> 16;
  const uint32_t previous_end = previous & 0xFFFFu;
  StoreU32(base, range_field,
           (std::min(first_block, previous_first) << 16) | std::max(end_block, previous_end));
}

void RecordNativeResourceLock(uint32_t resource, uint32_t base_pointer, uint32_t pointer,
                              uint32_t size, uint32_t flags, uint32_t caller) {
  std::lock_guard lock(g_native_resource_lock_mutex);
  NativeResourceLockState& state = g_native_resource_locks[resource];
  ++state.nesting;
  state.flags |= flags;
  if (!state.lock_caller) {
    state.lock_caller = caller;
  }
  if (!(flags & 1u)) {
    const uint32_t offset = pointer - base_pointer;
    state.minimum_offset = std::min(state.minimum_offset, offset);
    state.maximum_end = std::max(state.maximum_end, offset + size);
    state.guest_write = true;
  }
}

NativeResourceLockState ConsumeNativeResourceLock(uint32_t resource, bool outermost) {
  std::lock_guard lock(g_native_resource_lock_mutex);
  const auto existing = g_native_resource_locks.find(resource);
  if (existing == g_native_resource_locks.end()) {
    NativeResourceLockState conservative;
    conservative.guest_write = true;
    return conservative;
  }

  if (existing->second.nesting) {
    --existing->second.nesting;
  }
  NativeResourceLockState state = existing->second;
  if (outermost || !existing->second.nesting) {
    g_native_resource_locks.erase(existing);
  }
  return state;
}

// A D3D shader object keeps a copy of its container's first part and points at its microcode
// (XDK 2.0.6274 and 2.0.3529 alike): a vertex shader at +872 and +32, a pixel shader at +40 and
// +24. Word 0's low bits are the type.
constexpr uint32_t kVertexShaderHeaderOffset = 872;
constexpr uint32_t kVertexShaderMicrocodeOffset = 32;
constexpr uint32_t kPixelShaderHeaderOffset = 40;
constexpr uint32_t kPixelShaderMicrocodeOffset = 24;
constexpr uint32_t kPixelShaderType = 7;
constexpr uint32_t kMaximumShaderPart = 1u << 20;

// What a shader object held when it was registered: a handle that holds something else now was
// freed by the engine and reused for another shader.
struct NativeShaderObjectKey {
  uint32_t microcode = 0;
  uint32_t virtual_size = 0;
  uint32_t physical_size = 0;
  uint64_t header_hash = 0;
  bool operator==(const NativeShaderObjectKey&) const = default;
};

std::mutex g_native_shader_objects_mutex;
std::unordered_map<uint32_t, NativeShaderObjectKey> g_native_shader_objects;
// Set while the CreateVertexShader hook runs: it registers the shader, not its InitVertexShader call.
thread_local bool g_creating_vertex_shader = false;

std::optional<NativeShaderObjectKey> ReadNativeShaderObject(uint8_t* base, uint32_t shader,
                                                            uint32_t microcode, ShaderStage stage) {
  const uint32_t header = shader + (stage == ShaderStage::kVertex ? kVertexShaderHeaderOffset
                                                                  : kPixelShaderHeaderOffset);
  const uint32_t virtual_size = LoadU32(base, header + 4);
  const uint32_t physical_size = LoadU32(base, header + 8);
  if (!microcode || !virtual_size || virtual_size > kMaximumShaderPart ||
      physical_size > kMaximumShaderPart) {
    return std::nullopt;
  }
  return NativeShaderObjectKey{microcode, virtual_size, physical_size,
                               XXH3_64bits(GuestPointer(base, header), virtual_size)};
}

// False if the handle held that already.
bool RememberNativeShaderObject(uint32_t shader, const NativeShaderObjectKey& key) {
  std::lock_guard lock(g_native_shader_objects_mutex);
  const auto [found, inserted] = g_native_shader_objects.try_emplace(shader, key);
  if (!inserted && found->second == key) {
    return false;
  }
  found->second = key;
  return true;
}

#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
// Shaders translated in the background (shader_translator.h, AsyncRuntimeShaders): their
// registration waits here, and SubmitReadyShaderRegistrations (each SetPixelShader,
// SetVertexShader and Swap) sends it once the translation is done. Until then the renderer skips
// their draws (active-shader-texture-metadata), as it skips draws whose pipeline is compiling.
struct DeferredShaderRegistration {
  ShaderStage stage;
  uint64_t hash;
};
std::mutex g_deferred_shader_mutex;
std::unordered_map<uint32_t, DeferredShaderRegistration> g_deferred_shaders;
std::atomic<uint64_t> g_deferred_shader_serial{0};

void DeferShaderRegistration(uint32_t shader, ShaderStage stage, uint64_t hash) {
  std::lock_guard lock(g_deferred_shader_mutex);
  g_deferred_shaders[shader] = {stage, hash};
}

void ForgetDeferredShaderRegistration(uint32_t shader) {
  std::lock_guard lock(g_deferred_shader_mutex);
  g_deferred_shaders.erase(shader);
}

void SubmitReadyShaderRegistrations() {
  const uint64_t serial = rex::graphics::xnative::RuntimeShaderSerial();
  if (serial == g_deferred_shader_serial.load(std::memory_order_relaxed)) {
    return;
  }
  std::vector<RegisterShaderCommand> ready;
  {
    std::lock_guard lock(g_deferred_shader_mutex);
    g_deferred_shader_serial.store(serial, std::memory_order_relaxed);
    for (auto it = g_deferred_shaders.begin(); it != g_deferred_shaders.end();) {
      if (rex::graphics::xnative::RuntimeShaderPending(it->second.hash)) {
        ++it;
        continue;
      }
      // Translated, or failed: then the registration misses the cache as it did when the hook
      // translated it.
      RegisterShaderCommand command;
      command.shader = it->first;
      command.stage = it->second.stage;
      command.hash = it->second.hash;
      ready.push_back(command);
      it = g_deferred_shaders.erase(it);
    }
  }
  for (const RegisterShaderCommand& command : ready) {
    SubmitNativeCommand(command);
  }
}
#endif

void ForgetNativeShaderObject(uint32_t shader) {
  std::lock_guard lock(g_native_shader_objects_mutex);
  g_native_shader_objects.erase(shader);
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
  ForgetDeferredShaderRegistration(shader);
#endif
}

#ifdef XD3D_BindVertexShader
// The declaration each vertex shader was bound to and the vertex shader each device has set: the
// declaration of a draw made with none set.
std::mutex g_bound_vertex_declarations_mutex;
std::unordered_map<uint32_t, uint32_t> g_bound_vertex_declarations;
std::unordered_map<uint32_t, uint32_t> g_device_vertex_shaders;

void RememberBoundVertexDeclaration(uint32_t shader, uint32_t declaration) {
  std::lock_guard lock(g_bound_vertex_declarations_mutex);
  g_bound_vertex_declarations[shader] = declaration;
}

void ForgetBoundVertexDeclaration(uint32_t shader) {
  std::lock_guard lock(g_bound_vertex_declarations_mutex);
  g_bound_vertex_declarations.erase(shader);
}

void RememberDeviceVertexShader(uint32_t device, uint32_t shader) {
  std::lock_guard lock(g_bound_vertex_declarations_mutex);
  g_device_vertex_shaders[device] = shader;
}

uint32_t BoundVertexDeclaration(uint32_t device) {
  std::lock_guard lock(g_bound_vertex_declarations_mutex);
  const auto shader = g_device_vertex_shaders.find(device);
  if (shader == g_device_vertex_shaders.end()) {
    return 0;
  }
  const auto declaration = g_bound_vertex_declarations.find(shader->second);
  return declaration != g_bound_vertex_declarations.end() ? declaration->second : 0;
}
#endif

#if XPORT_TITLE_ID == 0x4D5308AB && defined(XD3D_BindVertexShader)
// Unreal Engine 3 moves textures inside its texture pool (defragmentation) with the GPU: vertex
// shader 0FF548D34E454B3D fetches 8 x 16 bytes per vertex from vertex fetch constant 95 and
// memory-exports them to the stream c6 describes (xe_gpu_memexport_stream_t; the shader adds the
// element index to eA.y). Elements: (vertex - c7.x) * 8, eM0..eM3 = r8..r5, then four elements
// later eM0..eM3 = r4..r1 (c255 = 0, 1, 4, 8). The native renderer runs no memory exports, so the
// engine pointed each moved texture at its new place while the bytes stayed at the old one (3D
// objects without their texture, HUD icons cut in bands, 2026-11-03). The copy is done here, on
// guest memory, when the game issues the draw: the renderer captures the textures of the draws
// that follow after it. XNATIVE_GPU_COPY=0 leaves it out.
constexpr uint64_t kGpuCopyShaderHash = 0x0FF548D34E454B3Dull;
// The copy shader's objects and where their microcode is (the pointer the shader was filled
// over: Bind patches the fetches' formats, offsets and strides there).
struct GpuCopyShader {
  uint32_t microcode = 0;
  uint32_t microcode_size = 0;
};
std::mutex g_gpu_copy_shaders_mutex;
std::unordered_map<uint32_t, GpuCopyShader> g_gpu_copy_shaders;

void NoteGpuCopyShader(uint32_t shader, ShaderStage stage, uint64_t hash, uint64_t canonical,
                       uint32_t microcode, uint32_t microcode_size) {
  if (stage != ShaderStage::kVertex || !shader) {
    return;
  }
  const bool copy = hash == kGpuCopyShaderHash || canonical == kGpuCopyShaderHash;
  std::lock_guard lock(g_gpu_copy_shaders_mutex);
  if (!copy) {
    g_gpu_copy_shaders.erase(shader);
    return;
  }
  g_gpu_copy_shaders[shader] = {microcode, microcode_size};
  REXLOG_INFO(
      "xnative-gpu-copy: vertex shader object {:08X} is the engine's GPU memory copy (microcode "
      "{:08X}, {} bytes)",
      shader, microcode, microcode_size);
}

// One 32-bit word through a GPU endian swap (xenos::Endian: none, 8in16, 8in32, 16in32).
uint32_t SwapGpuWord(uint32_t value, uint32_t endian) {
  switch (endian) {
    case 1:
      return ((value & 0x00FF00FFu) << 8) | ((value >> 8) & 0x00FF00FFu);
    case 2:
      return __builtin_bswap32(value);
    case 3:
      return (value << 16) | (value >> 16);
    default:
      return value;
  }
}

// The copy shader the device has set, if it has: its draws only copy memory.
bool FindBoundGpuCopyShader(uint32_t device, uint32_t& shader, GpuCopyShader& copy_shader) {
  static const bool enabled = [] {
    const char* value = std::getenv("XNATIVE_GPU_COPY");
    return !value || value[0] != '0';
  }();
  if (!enabled) {
    return false;
  }
  {
    std::lock_guard lock(g_bound_vertex_declarations_mutex);
    const auto found = g_device_vertex_shaders.find(device);
    if (found == g_device_vertex_shaders.end()) {
      return false;
    }
    shader = found->second;
  }
  std::lock_guard lock(g_gpu_copy_shaders_mutex);
  const auto found = g_gpu_copy_shaders.find(shader);
  if (found == g_gpu_copy_shaders.end()) {
    return false;
  }
  copy_shader = found->second;
  return true;
}

// The copy of one such draw. Called after the game's own DrawVertices, which patches the fetches
// for the declaration (the first draw finds them still zero before it).
void RunGpuMemoryCopy(uint8_t* base, uint32_t device, uint32_t shader,
                      const GpuCopyShader& copy_shader, uint32_t start_vertex,
                      uint32_t vertex_count) {
  static std::atomic<uint64_t> copy_count{0};
  const uint64_t count = copy_count.fetch_add(1, std::memory_order_relaxed) + 1;
  const bool log = count <= 48 || !(count % 1024);
  auto skip = [&](const char* reason, uint64_t a = 0, uint64_t b = 0) {
    REXLOG_WARN("xnative-gpu-copy: #{} not copied ({} {:X} {:X}): start={} vertices={}", count,
                reason, a, b, start_vertex, vertex_count);
  };
  if (!vertex_count) {
    return;
  }

  // The 8 vertex fetches as Bind patched them for the declaration: register, offset and stride
  // (dwords), format. The control flow ends where the first exec's instructions start.
  // The object's copy of the container's first part locates the instructions in the physical
  // part (after the literal constants): the Shader record at shaderOffset (+24) gives their offset
  // and size.
  const uint32_t header = shader + kVertexShaderHeaderOffset;
  const uint32_t shader_record = header + LoadU32(base, header + 24);
  const uint32_t instructions_offset = LoadU32(base, shader_record);
  const uint32_t instructions_size = LoadU32(base, shader_record + 4);
  const uint32_t microcode = copy_shader.microcode + instructions_offset;
  const uint32_t slots = instructions_size / 12;
  if (!copy_shader.microcode || !slots || slots > 4096 ||
      uint64_t(instructions_offset) + instructions_size > copy_shader.microcode_size) {
    skip("microcode", uint64_t(copy_shader.microcode) << 32 | instructions_offset,
                uint64_t(instructions_size) << 32 | copy_shader.microcode_size);
    return;
  }
  struct Fetch {
    uint32_t reg, offset, stride, format;
  };
  std::array<Fetch, 8> fetches{};
  uint32_t fetch_count = 0;
  uint32_t last_stride = 0;
  bool layout_ok = true;
  uint32_t control_flow_end = slots;
  for (uint32_t pair = 0; pair < control_flow_end; ++pair) {
    const uint32_t d0 = LoadU32(base, microcode + pair * 12);
    const uint32_t d1 = LoadU32(base, microcode + pair * 12 + 4);
    const uint32_t d2 = LoadU32(base, microcode + pair * 12 + 8);
    const uint64_t instructions[2] = {d0 | uint64_t(d1 & 0xFFFF) << 32,
                                      (d1 >> 16) | uint64_t(d2) << 16};
    for (const uint64_t cf : instructions) {
      const uint32_t opcode = uint32_t(cf >> 44) & 0xF;
      if (!((opcode >= 1 && opcode <= 6) || opcode == 13 || opcode == 14)) {
        continue;  // not an exec
      }
      const uint32_t address = uint32_t(cf) & 0xFFF;
      const uint32_t count_in_exec = uint32_t(cf >> 12) & 7;
      const uint32_t sequence = uint32_t(cf >> 16) & 0xFFF;
      if (address) {
        control_flow_end = std::min(control_flow_end, address);
      }
      for (uint32_t k = 0; k < count_in_exec; ++k) {
        if (!((sequence >> (2 * k)) & 1)) {
          continue;  // ALU
        }
        if (address + k >= slots) {
          layout_ok = false;
          continue;
        }
        const uint32_t slot = microcode + (address + k) * 12;
        const uint32_t f0 = LoadU32(base, slot), f1 = LoadU32(base, slot + 4),
                       f2 = LoadU32(base, slot + 8);
        if (f0 & 31) {
          continue;  // a texture fetch
        }
        const uint32_t constant = ((f0 >> 20) & 31) * 3 + ((f0 >> 25) & 3);
        const bool mini = (f1 >> 30) & 1;
        const uint32_t stride = mini ? last_stride : (f2 & 0xFF);
        last_stride = stride;
        if (constant != 95 || fetch_count >= fetches.size()) {
          layout_ok = false;
          continue;
        }
        fetches[fetch_count++] = {(f0 >> 12) & 63, (f2 >> 8) & 0x7FFFFF, stride, (f1 >> 16) & 63};
      }
    }
  }
  // Bytes of one element of a vertex fetch format (xenos::VertexFormat) or of a memory export
  // format (xenos::ColorFormat, same numbers): 0 for one this copy does not take.
  const auto element_bytes = [](uint32_t format, bool exported) -> uint32_t {
    switch (format) {
      case 6: case 7: case 16: case 17: case 25: case 31: case 36:
        return 4;
      case 33:  // k_32: no export format
        return exported ? 0 : 4;
      case 26: case 32: case 37:
        return 8;
      case 34:  // k_32_32
        return exported ? 0 : 8;
      case 35:  // k_32_32_32_32
        return exported ? 0 : 16;
      case 38:
        return 16;
      default:
        return 0;
    }
  };
  const uint32_t element_size = fetch_count ? element_bytes(fetches[0].format, false) : 0;
  uint32_t registers = 0;
  for (uint32_t i = 0; i < fetch_count; ++i) {
    const Fetch& fetch = fetches[i];
    // One format for all, each element within the vertex's stride.
    if (fetch.reg < 1 || fetch.reg > 8 || (registers & (1u << fetch.reg)) || !element_size ||
        fetch.format != fetches[0].format || fetch.stride != fetches[0].stride ||
        uint64_t(fetch.offset) * 4 + element_size > uint64_t(fetch.stride) * 4) {
      layout_ok = false;
    }
    registers |= 1u << fetch.reg;
  }
  if (!layout_ok || fetch_count != 8) {
    if (count <= 8) {
      std::string list;
      for (uint32_t i = 0; i < fetch_count; ++i) {
        list += fmt::format(" r{}+{}/{}:f{}", fetches[i].reg, fetches[i].offset, fetches[i].stride,
                            fetches[i].format);
      }
      REXLOG_WARN("xnative-gpu-copy: #{} fetches:{} stream0={:08X} {:08X}", count, list,
                  LoadU32(base, device + kStreamFetchBase), LoadU32(base, device + kStreamSizeBase));
    }
    skip("fetch layout", uint64_t(microcode) << 32 | fetch_count,
                uint64_t(fetches[0].stride) << 32 | fetches[0].format);
    return;
  }

  // Source: vertex fetch constant 95 (stream 0). Destination: the export stream in c6, c7.x the
  // vertex whose elements start the stream.
  const uint32_t fetch_word0 = LoadU32(base, device + kStreamFetchBase);
  const uint32_t fetch_word1 = LoadU32(base, device + kStreamSizeBase);
  const uint64_t source = fetch_word0 & 0x1FFFFFFCu;
  const uint64_t source_size = uint64_t((fetch_word1 >> 2) & 0xFFFFFF) * 4;
  const uint32_t source_endian = fetch_word1 & 3;
  const uint32_t constants = device + kReplayShaderConstantsOffset;
  const uint32_t stream[4] = {LoadU32(base, constants + 6 * 16),
                              LoadU32(base, constants + 6 * 16 + 4),
                              LoadU32(base, constants + 6 * 16 + 8),
                              LoadU32(base, constants + 6 * 16 + 12)};
  const float first_vertex = std::bit_cast<float>(LoadU32(base, constants + 7 * 16));
  if ((stream[0] >> 30) != 1 || stream[1] != 0x4B000000u || (stream[2] >> 20) != 0x4B0 ||
      (stream[3] >> 23) != 0x96) {
    skip("export stream", uint64_t(stream[0]) << 32 | stream[1],
                uint64_t(stream[2]) << 32 | stream[3]);
    return;
  }
  const uint64_t destination = uint64_t(stream[0] & 0x3FFFFFFF) << 2;
  const uint32_t export_endian = stream[2] & 7;
  const uint32_t export_format = (stream[2] >> 8) & 63;
  const uint64_t element_limit = stream[3] & 0x7FFFFF;
  if (element_bytes(export_format, true) != element_size || export_endian > 3) {
    skip("export format", uint64_t(export_format) << 32 | fetches[0].format,
                export_endian);
    return;
  }
  if (!std::isfinite(first_vertex) || first_vertex < 0.0f ||
      first_vertex != std::floor(first_vertex) || double(first_vertex) > double(start_vertex)) {
    skip("first vertex", std::bit_cast<uint32_t>(first_vertex), start_vertex);
    return;
  }
  const uint64_t stride_bytes = uint64_t(fetches[0].stride) * 4;
  const uint64_t first_element = (uint64_t(start_vertex) - uint64_t(first_vertex)) * 8;
  const uint64_t source_begin = source + uint64_t(start_vertex) * stride_bytes;
  const uint64_t source_end = source_begin + uint64_t(vertex_count) * stride_bytes;
  const uint64_t vertex_bytes = uint64_t(element_size) * 8;
  const uint64_t destination_begin = destination + first_element * element_size;
  const uint64_t destination_end = destination_begin + uint64_t(vertex_count) * vertex_bytes;
  constexpr uint64_t kPhysicalSize = 0x20000000;
  if (source_end > source + source_size || source_end > kPhysicalSize ||
      destination_end > kPhysicalSize ||
      (element_limit && first_element + uint64_t(vertex_count) * 8 > element_limit)) {
    skip("range", source_begin << 32 | source_end,
                destination_begin << 32 | destination_end);
    return;
  }

  // 16-bit components: k_16_16, k_16_16_16_16 and their float forms.
  const uint32_t format = fetches[0].format;
  const bool halves = format == 25 || format == 26 || format == 31 || format == 32;
  // The console's 512 MB as the game sees it at 0xA0000000 (kernel/memory.cpp's physical view).
  uint8_t* const physical = GuestPointer(base, 0xA0000000u);
  const bool backward = destination_begin > source_begin;
  for (uint32_t n = 0; n < vertex_count; ++n) {
    const uint32_t i = backward ? vertex_count - 1 - n : n;
    const uint64_t vertex = uint64_t(start_vertex) + i;
    // Elements in export order (r8 first), each element_size bytes.
    uint32_t words[8 * 4];
    auto* elements = reinterpret_cast<uint8_t*>(words);
    for (const Fetch& fetch : fetches) {
      const uint8_t* from = physical + source + vertex * stride_bytes + uint64_t(fetch.offset) * 4;
      std::memcpy(elements + (8 - fetch.reg) * element_size, from, element_size);
    }
    // Each word as the GPU moves it: fetched with the fetch constant's swap, its components taken
    // apart (16-bit formats put x in the high half when fetched, in the low half when exported),
    // packed again and stored with the export's swap. 8in32 in and 8in16 out with 16-bit
    // components is a plain copy.
    if (source_endian != export_endian || halves) {
      for (uint32_t w = 0; w < 2 * element_size; ++w) {
        uint32_t word = SwapGpuWord(words[w], source_endian);
        if (halves) {
          word = (word << 16) | (word >> 16);
        }
        words[w] = SwapGpuWord(word, export_endian);
      }
    }
    std::memcpy(physical + destination_begin + uint64_t(i) * vertex_bytes, elements, vertex_bytes);
  }
  if (log) {
    REXLOG_INFO(
        "xnative-gpu-copy: #{} {:X} bytes {:08X} -> {:08X} (vertices {}+{}, first {}, stride {}, "
        "fetch endian {} format {}, export endian {} format {}, limit {})",
        count, uint64_t(vertex_count) * vertex_bytes, source_begin, destination_begin, start_vertex,
        vertex_count, first_vertex, stride_bytes, source_endian, fetches[0].format, export_endian,
        export_format, element_limit);
  }
}
#endif

// A shader the game filled without CreateVertexShader / CreatePixelShader (an engine that
// allocates the object and the microcode itself, as Unreal Engine 3 does). Its container, the
// bytes the caches hash, is the object's copy of the first part followed by the microcode.
void RegisterNativeShaderObject(uint8_t* base, uint32_t shader, uint32_t microcode,
                                ShaderStage stage) {
  static const char* const dump_path = std::getenv("XPORT_SHADER_DUMP");
  const bool dump = dump_path && *dump_path;
  const bool native_mode = IsNativeMode();
  if (!native_mode && !dump) {
    return;
  }
  const auto key = ReadNativeShaderObject(base, shader, microcode, stage);
  if (!key || !RememberNativeShaderObject(shader, *key)) {
    return;
  }

  const uint32_t header = shader + (stage == ShaderStage::kVertex ? kVertexShaderHeaderOffset
                                                                  : kPixelShaderHeaderOffset);
  std::vector<uint8_t> container(size_t(key->virtual_size) + key->physical_size);
  std::memcpy(container.data(), GuestPointer(base, header), key->virtual_size);
  std::memcpy(container.data() + key->virtual_size, GuestPointer(base, microcode),
              key->physical_size);
  const uint64_t hash = XXH3_64bits(container.data(), container.size());
  if (dump) {
    DumpXenosShaderContainer(container.data(), uint32_t(container.size()), hash, stage, dump_path);
  }
  if (!native_mode) {
    return;
  }
  const uint64_t canonical = rex::graphics::xnative::CanonicalShaderHash(hash);
#if XPORT_TITLE_ID == 0x4D5308AB && defined(XD3D_BindVertexShader)
  NoteGpuCopyShader(shader, stage, hash, canonical, microcode, key->physical_size);
#endif
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
  if (rex::graphics::xnative::AsyncRuntimeShaders()) {
    if (rex::graphics::xnative::QueueRuntimeShader(canonical, container.data(),
                                                   uint32_t(container.size()),
                                                   stage == ShaderStage::kPixel)) {
      DeferShaderRegistration(shader, stage, canonical);
      return;
    }
    ForgetDeferredShaderRegistration(shader);
  } else
#endif
  {
    rex::graphics::xnative::PrepareRuntimeShader(canonical, container.data(), container.size(),
                                                 stage == ShaderStage::kPixel);
  }
  RegisterShaderCommand command;
  command.shader = shader;
  command.stage = stage;
  command.hash = canonical;
  SubmitNativeCommand(command);
}

// A pixel shader is filled in line (D3D's CreatePixelShader inlines it too), so one the game made
// itself is first seen when it is set.
void RegisterNativePixelShaderOnSet(uint8_t* base, uint32_t shader) {
  if (shader > UINT16_MAX && shader != UINT32_MAX &&
      (LoadU32(base, shader) & 0xF) == kPixelShaderType) {
    RegisterNativeShaderObject(base, shader, LoadU32(base, shader + kPixelShaderMicrocodeOffset),
                               ShaderStage::kPixel);
  }
}

void RegisterNativeShader(PPCContext& ctx, uint8_t* base, GuestFunction implementation,
                          ShaderStage stage) {
  const bool native_mode = IsNativeMode();
  // the dump also works with the native renderer, for the containers missing from its
  // SPIR-V cache (tools/iv_shaders, 2026-10-30): XPORT_SHADER_DUMP=<dir>, as the cvar is not compiled
  // in. Taken before the D3D call can touch the container.
  const char* dump_env = std::getenv("XPORT_SHADER_DUMP");
  const std::string dump_path = dump_env ? dump_env : "";
  if (!native_mode && dump_path.empty()) {
    implementation(ctx, base);
    return;
  }

  const uint32_t shader_container = ctx.r3.u32;
  const uint32_t virtual_size = shader_container ? LoadU32(base, shader_container + 4) : 0;
  const uint32_t physical_size = shader_container ? LoadU32(base, shader_container + 8) : 0;
  const bool size_valid = virtual_size <= UINT32_MAX - physical_size;
  const uint32_t total_size = size_valid ? virtual_size + physical_size : 0;
  const uint64_t hash =
      total_size ? XXH3_64bits(GuestPointer(base, shader_container), total_size) : 0;

  if (hash && !dump_path.empty()) {
    DumpXenosShaderContainer(static_cast<const uint8_t*>(GuestPointer(base, shader_container)),
                             total_size, hash, stage, dump_path);
  }
  // A shader the SPIR-V cache lacks is translated now, while the container is whole, so the
  // renderer has it by the time the registration below reaches it.
  [[maybe_unused]] bool deferred = false;
  if (hash && native_mode) {
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
    // Gears of War 3: queued (a copy of the container), registered once translated.
    if (rex::graphics::xnative::AsyncRuntimeShaders()) {
      deferred = rex::graphics::xnative::QueueRuntimeShader(
          rex::graphics::xnative::CanonicalShaderHash(hash),
          static_cast<const uint8_t*>(GuestPointer(base, shader_container)), total_size,
          stage == ShaderStage::kPixel);
    } else
#endif
    {
      rex::graphics::xnative::PrepareRuntimeShader(
          rex::graphics::xnative::CanonicalShaderHash(hash),
          static_cast<const uint8_t*>(GuestPointer(base, shader_container)), total_size,
          stage == ShaderStage::kPixel);
    }
  }

  g_creating_vertex_shader = stage == ShaderStage::kVertex;
  implementation(ctx, base);
  g_creating_vertex_shader = false;

  if (ctx.r3.u32 && hash && native_mode) {
    RegisterShaderCommand command;
    command.shader = ctx.r3.u32;
    command.stage = stage;
    command.hash = rex::graphics::xnative::CanonicalShaderHash(hash);
#if XPORT_TITLE_ID == 0x4D5308AB && defined(XD3D_BindVertexShader)
    // D3D's own copy of the microcode, as the object points at it.
    NoteGpuCopyShader(command.shader, stage, hash, command.hash,
                      LoadU32(base, command.shader + kVertexShaderMicrocodeOffset), physical_size);
#endif
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
    if (deferred) {
      DeferShaderRegistration(command.shader, stage, command.hash);
    } else {
      ForgetDeferredShaderRegistration(command.shader);
      SubmitNativeCommand(command);
    }
#else
    SubmitNativeCommand(command);
#endif
    // Known now: setting it registers it no second time.
    if (stage == ShaderStage::kPixel) {
      const uint32_t shader = ctx.r3.u32;
      if (const auto key = ReadNativeShaderObject(
              base, shader, LoadU32(base, shader + kPixelShaderMicrocodeOffset), stage)) {
        RememberNativeShaderObject(shader, *key);
      }
    }
  }
}

bool SubmitNativeVertexDeclaration(uint8_t* base, uint32_t device, uint32_t declaration) {
  // A default-state table can bind 0xFFFF before any declaration object exists.
  // The D3D setter stores this value but never dereferences it.
  if (declaration <= UINT16_MAX || declaration == UINT32_MAX ||
      LoadU32(base, declaration) != kVertexDeclarationMagic) {
    return false;
  }

  const uint32_t element_count = LoadU32(base, declaration + kVertexDeclarationElementCountOffset);
  const uint32_t maximum_stream =
      LoadU32(base, declaration + kVertexDeclarationMaximumStreamOffset);
  if (!element_count || element_count > kMaximumVertexElementCount ||
      maximum_stream >= kVertexStreamCount) {
    return false;
  }

  RegisterVertexDeclarationCommand command{};
  command.device = device;
  command.declaration = declaration;
  command.element_count = element_count;
  command.maximum_stream = maximum_stream;
  const uint32_t elements = declaration + kVertexDeclarationElementsOffset;
  for (uint32_t index = 0; index < element_count; ++index) {
    const uint32_t element = elements + index * sizeof(VertexElement);
    VertexElement& output = command.elements[index];
    output.stream =
        __builtin_bswap16(*reinterpret_cast<volatile uint16_t*>(GuestPointer(base, element)));
    output.offset =
        __builtin_bswap16(*reinterpret_cast<volatile uint16_t*>(GuestPointer(base, element + 2)));
    output.type = LoadU32(base, element + 4);
    output.method = LoadU8(base, element + 8);
    output.usage = LoadU8(base, element + 9);
    output.usage_index = LoadU8(base, element + 10);
    output.padding = 0;
  }

  bool log_declaration = false;
  {
    std::lock_guard lock(g_vertex_declaration_diagnostic_mutex);
    if (g_logged_vertex_declarations.size() < kVertexDeclarationDiagnosticLimit) {
      log_declaration = g_logged_vertex_declarations.emplace(declaration, true).second;
    }
  }
  if (log_declaration) {
    std::string elements_summary;
    for (uint32_t index = 0; index < element_count; ++index) {
      const VertexElement& element = command.elements[index];
      elements_summary += fmt::format(
          "{}s{}+{} type={:08X} method={} usage={}/{}", index ? ", " : "", element.stream,
          element.offset, element.type, element.method, element.usage, element.usage_index);
    }
    REXLOG_INFO("xnative-vertex-declaration: handle={:08X} elements={} maximum-stream={} [{}]",
                declaration, element_count, maximum_stream, elements_summary);
  }
  SubmitNativeCommand(command);
  return true;
}

PPCContext InvokeGuest(PPCContext& parent, uint8_t* base, GuestFunction function, uint64_t r3 = 0,
                       uint64_t r4 = 0, uint64_t r5 = 0, uint64_t r6 = 0, uint64_t r7 = 0) {
  PPCContext nested = parent;
  nested.r3.u64 = r3;
  nested.r4.u64 = r4;
  nested.r5.u64 = r5;
  nested.r6.u64 = r6;
  nested.r7.u64 = r7;
  function(nested, base);
  return nested;
}

void FreeAlignedGuestAllocation(PPCContext& ctx, uint8_t* base, uint32_t address) {
  if (!address) {
    return;
  }
  const uint32_t allocation = LoadU32(base, address - 4);
  if (allocation) {
    InvokeGuest(ctx, base, XD3D_GuestFree, allocation, kAlignedAllocationFreeFlags);
  }
}

bool CachedSelectorMatches(uint32_t command_mask, uint32_t replay_mask) {
  return !command_mask || !replay_mask || (command_mask & replay_mask) != 0;
}

void PatchCommandDevice(std::vector<uint8_t>& bytes, uint32_t device) {
  if (bytes.size() < sizeof(CommandHeader) + sizeof(device)) {
    return;
  }
  std::memcpy(bytes.data() + sizeof(CommandHeader), &device, sizeof(device));
}

void PatchCachedDrawExecutionState(std::vector<uint8_t>& bytes, CommandType type) {
  const auto patch = [&bytes]<typename DrawCommand>() {
    if (bytes.size() != sizeof(DrawCommand)) {
      return;
    }
    DrawCommand draw{};
    std::memcpy(&draw, bytes.data(), sizeof(draw));
    draw.dirty_state.words.fill(UINT64_MAX);
    std::memcpy(bytes.data(), &draw, sizeof(draw));
  };
  switch (type) {
    case CommandType::kDrawPrimitive:
      patch.template operator()<DrawPrimitiveCommand>();
      break;
    case CommandType::kDrawPrimitiveUp:
      patch.template operator()<DrawPrimitiveUpCommand>();
      break;
    case CommandType::kDrawIndexedPrimitive:
      patch.template operator()<DrawIndexedPrimitiveCommand>();
      break;
    default:
      break;
  }
}

bool SubmitCapturedNativeCommand(PPCContext& ctx, uint8_t* base, uint32_t device,
                                 uint32_t command_list, uint32_t replay_mask,
                                 uint32_t replay_command_ordinal,
                                 const CapturedNativeCommand& captured) {
  auto* graphics = GetNativeGraphicsSystem();
  if (!graphics || captured.bytes.empty()) {
    return false;
  }

  std::vector<uint8_t> bytes = captured.bytes;
  PatchCommandDevice(bytes, device);
  // Selector-filtered cached lists do not preserve a trustworthy predecessor
  // dirty-mask chain. The scoped replay snapshot is authoritative, so force a
  // conservative semantic refresh for every replayed draw.
  PatchCachedDrawExecutionState(bytes, captured.type);

  // A cached command may execute many times. Give each execution unique
  // provenance at submission rather than reusing the ID and LR from the list
  // build, otherwise transition events from distinct frames collide.
  DrawIndexedPrimitiveCommand indexed_draw{};
  if (captured.type == CommandType::kDrawIndexedPrimitive) {
    if (bytes.size() != sizeof(DrawIndexedPrimitiveCommand)) {
      return false;
    }
    DrawIndexedPrimitiveCommand draw;
    std::memcpy(&draw, bytes.data(), sizeof(draw));
    draw.caller = static_cast<uint32_t>(ctx.lr);
    draw.origin_flags = kDrawCommandOriginCachedReplay;
    draw.command_list = command_list;
    draw.draw_id = g_indexed_draw_invocation_id.fetch_add(1, std::memory_order_relaxed) + 1;
    indexed_draw = draw;
    std::memcpy(bytes.data(), &draw, sizeof(draw));
  }

  uint32_t replay_vertex_data = 0;
  if (captured.type == CommandType::kDrawPrimitiveUp) {
    if (bytes.size() != sizeof(DrawPrimitiveUpCommand)) {
      return false;
    }
    DrawPrimitiveUpCommand draw;
    std::memcpy(&draw, bytes.data(), sizeof(draw));
    if (!captured.payload.empty()) {
      replay_vertex_data =
          InvokeGuest(ctx, base, XD3D_PhysicalAlloc, captured.payload.size(), 16).r3.u32;
      if (!replay_vertex_data) {
        return false;
      }
      std::memcpy(GuestPointer(base, replay_vertex_data), captured.payload.data(),
                  captured.payload.size());
      draw.vertex_data = replay_vertex_data;
      draw.vertex_data_size = uint32_t(captured.payload.size());
      std::memcpy(bytes.data(), &draw, sizeof(draw));
    }
  }

  bool accepted = false;
  {
    ScopedReplayDrawState replay_state(base, device, captured.draw_snapshot);
    accepted = graphics->SubmitTitleCommand(kTitleId, kTitleCommandAbi, bytes.data(), bytes.size());
  }
  FreeAlignedGuestAllocation(ctx, base, replay_vertex_data);
  return accepted;
}

std::shared_ptr<const CachedNativeCommandList> FindCachedNativeCommandList(uint32_t command_list) {
  std::lock_guard lock(g_cached_native_command_lists_mutex);
  const auto found = g_cached_native_command_lists.find(command_list);
  return found != g_cached_native_command_lists.end() ? found->second : nullptr;
}

void EraseCachedNativeCommandList(uint32_t command_list) {
  if (!command_list) {
    return;
  }
  std::lock_guard lock(g_cached_native_command_lists_mutex);
  g_cached_native_command_lists.erase(command_list);
}

bool ReplayCachedNativeCommandList(PPCContext& ctx, uint8_t* base, uint32_t device,
                                   uint32_t command_list, uint32_t replay_mask,
                                   uint64_t& selected_count, uint64_t& submitted_count) {
  const auto cached = FindCachedNativeCommandList(command_list);
  if (!cached) {
    return false;
  }

  selected_count = 0;
  submitted_count = 0;
  if (g_active_native_capture.active) {
    for (const auto& command : cached->commands) {
      if (!CachedSelectorMatches(command.selector_mask, replay_mask)) {
        continue;
      }
      CapturedNativeCommand flattened = command;
      flattened.selector_mask = g_active_native_capture.selector_mask;
      g_active_native_capture.commands.push_back(std::move(flattened));
      ++selected_count;
      ++submitted_count;
    }
    return true;
  }

  for (size_t command_index = 0; command_index < cached->commands.size(); ++command_index) {
    const auto& command = cached->commands[command_index];
    if (!CachedSelectorMatches(command.selector_mask, replay_mask)) {
      continue;
    }
    ++selected_count;
    if (command.type == CommandType::kResolve && command.bytes.size() == sizeof(ResolveCommand)) {
      ResolveCommand resolve{};
      std::memcpy(&resolve, command.bytes.data(), sizeof(resolve));
      const uint32_t known_frontbuffer = g_last_present_frontbuffer.load(std::memory_order_relaxed);
      if (rex::diagnostics::IsEnabled(rex::diagnostics::Category::kGuestHooks) &&
          ((known_frontbuffer && resolve.destination_texture == known_frontbuffer) ||
           (resolve.flags & 0x04000000u))) {
        std::fprintf(stderr,
                     "[ResolveOriginTrace] origin=replay caller=%08X command-list=%08X "
                     "replay-mask=%08X selector=%08X "
                     "source=%08X/%08X destination=%08X flags=%08X\n",
                     uint32_t(ctx.lr), command_list, replay_mask, command.selector_mask,
                     resolve.source.handle, resolve.source.address, resolve.destination_texture,
                     resolve.flags);
        std::fflush(stderr);
      }
    }
    if (SubmitCapturedNativeCommand(ctx, base, device, command_list, replay_mask,
                                    uint32_t(command_index + 1), command)) {
      ++submitted_count;
    }
  }
  return true;
}

// XPORT_DRAW_CHAINS=1: which guest call paths issue draws. Each new path (the draw's caller
// and up to 9 frames of the r1 back chain) is logged once with the time; every 5 s, how many
// draws each path issued in that window (so a pass that stops being issued shows up).
void NoteDrawChain(PPCContext& ctx, uint8_t* base, const char* kind) {
  static const bool enabled = std::getenv("XPORT_DRAW_CHAINS") != nullptr;
  if (!enabled) {
    return;
  }
  uint32_t chain[10];
  uint32_t count = 0;
  chain[count++] = uint32_t(ctx.lr);
  uint32_t sp = ctx.r1.u32;
  while (count < 10 && sp && sp < 0xFFFF0000u) {
    const uint32_t next = LoadU32(base, sp);
    if (next <= sp || next - sp > 0x100000) {
      break;
    }
    chain[count++] = LoadU32(base, next - 8);
    sp = next;
  }
  const uint64_t key = XXH3_64bits(chain, count * sizeof(uint32_t)) ^ uint64_t(kind[4]);
  struct Entry {
    uint64_t window = 0;
    uint64_t total = 0;
  };
  static std::mutex mutex;
  static std::unordered_map<uint64_t, Entry> entries;
  static const auto start = std::chrono::steady_clock::now();
  static auto last_dump = start;
  std::lock_guard lock(mutex);
  auto [it, inserted] = entries.try_emplace(key);
  ++it->second.window;
  ++it->second.total;
  const auto now = std::chrono::steady_clock::now();
  const auto ms = std::chrono::duration_cast<std::chrono::milliseconds>(now - start).count();
  if (inserted) {
    std::string line = fmt::format("xport-draw-chain: new {:016X} {} ms={}:", key, kind, ms);
    for (uint32_t index = 0; index < count; ++index) {
      line += fmt::format(" {:08X}", chain[index]);
    }
    REXLOG_INFO("{}", line);
  }
  if (now - last_dump >= std::chrono::seconds(5)) {
    last_dump = now;
    std::string line = fmt::format("xport-draw-chain: window ms={}", ms);
    for (auto& [chain_key, entry] : entries) {
      if (entry.window) {
        line += fmt::format(" {:016X}={}", chain_key, entry.window);
        entry.window = 0;
      }
    }
    REXLOG_INFO("{}", line);
  }
}

void StoreAllDirty(uint8_t* base, uint32_t device) {
  StoreU64(base, device, UINT64_MAX);
  StoreU64(base, device + 8, UINT64_MAX);
  StoreU64(base, device + 16, UINT64_MAX);
  StoreU64(base, device + 24, UINT64_MAX);
  StoreU64(base, device + 32, UINT64_MAX);
}

void ApplyNativeDirtyDefaults(uint8_t* base, uint32_t device) {
  MarkDirty(base, device, 2, kNativeDirtyMask16);
  MarkDirty(base, device, 3, kNativeDirtyMask24);
  MarkDirty(base, device, 4, kNativeDirtyMask32);
}

void ApplyDeviceCpuPrefix(uint8_t* base, uint32_t device) {
  StoreU64(base, device + DeviceOffset(11816), UINT32_MAX);
  for (uint32_t index = 0; index < 26; ++index) {
    const uint32_t address = device + DeviceOffset(1152) + index * 24;
    StoreU32(base, address, LoadU32(base, address) & ~uint32_t(3));
  }
  for (uint32_t index = 0; index < 18; ++index) {
    const uint32_t address = device + DeviceOffset(1776) + index * 8;
    StoreU32(base, address, (LoadU32(base, address) & ~uint32_t(3)) | 1);
  }

  StoreU32(base, device + DeviceOffset(10428), 0x20002000);
  StoreU32(base, device + DeviceOffset(10604), 8);
  StoreU32(base, device + DeviceOffset(10708), 0x000FF000);
  StoreU32(base, device + DeviceOffset(10712), 0x000FF100);
  StoreU32(base, device + DeviceOffset(10564), LoadU32(base, device + DeviceOffset(10564)) | 0x00080000);
  StoreU32(base, device + DeviceOffset(10628), 14);
  StoreU32(base, device + DeviceOffset(10580), 4);
  StoreU32(base, device + DeviceOffset(10688), 4);
  StoreU32(base, device + DeviceOffset(10768), 14);
  StoreU32(base, device + DeviceOffset(10772), 16);
  StoreU32(base, device + DeviceOffset(10568), LoadU32(base, device + DeviceOffset(10568)) | 0x00010000);
  StoreU32(base, device + DeviceOffset(10444), 0x00FFFFFF);
  StoreU32(base, device + DeviceOffset(10824), 2);
  StoreU32(base, device + DeviceOffset(10916), 14);
  StoreAllDirty(base, device);
}

void PublishGlobalDevice(uint8_t* base, uint32_t device) {
  const uint32_t global_device = LoadU32(base, kVdGlobalDeviceImport);
  if (global_device) {
    StoreU32(base, global_device, device);
  }
}

void FreeNativeDeviceAllocation(PPCContext& ctx, uint8_t* base, uint32_t device) {
  const uint32_t fallback = LoadU32(base, device + DeviceOffset(16712));
  if (fallback) {
    InvokeGuest(ctx, base, XD3D_GuestFree, fallback, kFallbackAllocationFreeFlags);
  }
  FreeAlignedGuestAllocation(ctx, base, device);
}

void InitializeNativeCommandScratch(uint8_t* base, uint32_t device) {
  const uint32_t scratch = LoadU32(base, device + DeviceOffset(16712));
  StoreU32(base, device + kCommandScratchHeadOffset, 0);
  StoreU32(base, device + kCommandScratchWriteOffset, scratch);
  StoreU32(base, device + kCommandScratchEndOffset, scratch ? scratch + kCommandScratchSize : 0);
}

#if (XPORT_TITLE_ID == 0x4D5307E6)
// Halo 3 writes some shader constants straight into its command buffer: sub_821710C8 puts a
// SET_CONSTANT packet there and returns where its registers go, and the caller copies them in.
// That is how it sends the node and world matrices (VS c16-c18: sub_82175810, sub_82171020 and
// sub_8236C1B0 while the byte at 0x828C46E7 is set, which sub_8216E730 does for the frame's
// world pass), the view block (VS c16-c95, sub_8216E730) and PS c108 (sub_8216CEC0). The
// device's copy of the constants (0x780, what the renderer reads) never sees them, so every
// structure and object was drawn with a stale matrix: the map around the camera, stretched and
// spiked. Each write goes into that copy once its caller has filled it (at the next such write
// or at the next draw) and its groups are reported changed to the renderer. A group the D3D
// functions changed since the last draw is left alone: the draw sends theirs after the packet,
// so the GPU ends up with those.
struct DirectConstantWrite {
  uint32_t device = 0;
  uint32_t data = 0;
  uint32_t first = 0;  // ALU register: VS 0-255, PS 256-511
  uint32_t count = 0;
};
std::mutex g_direct_constant_mutex;
std::vector<DirectConstantWrite> g_direct_constant_writes;
uint32_t g_direct_constant_device = 0;
std::array<uint64_t, 2> g_direct_constant_groups{};  // VS, PS words: bit 63 is group 0
// The device's dirty words that Halo's tessellated draw (sub_825A6DE0, the water's
// DrawIndexedTessellatedPrimitive) sent and cleared: the renderer never sees that draw, so the
// next one it sees takes them as changed too. Lost, later draws kept older constants and
// booleans (the transparents' b0 took the lighting pass branch: dark bands under a hard line).
uint32_t g_carried_dirty_device = 0;
std::array<uint64_t, 5> g_carried_dirty{};

void FlushDirectConstantWritesLocked(uint8_t* base) {
  for (const DirectConstantWrite& write : g_direct_constant_writes) {
    if (write.device != g_direct_constant_device) {
      g_direct_constant_groups = {};
      g_direct_constant_device = write.device;
    }
    const uint64_t d3d_groups[2] = {LoadU64(base, write.device),
                                    LoadU64(base, write.device + uint32_t(sizeof(uint64_t)))};
    for (uint32_t index = 0; index < write.count; ++index) {
      const uint32_t reg = (write.first + index) & 0x1FFu;
      const uint32_t word = reg >> 8;
      const uint64_t group = uint64_t{1} << (63u - ((reg & 0xFFu) >> 2));
      if (d3d_groups[word] & group) {
        continue;
      }
      std::memcpy(GuestPointer(base, write.device + DeviceOffset(1920) + reg * 16u),
                  GuestPointer(base, write.data + index * 16u), 16);
      g_direct_constant_groups[word] |= group;
    }
  }
  g_direct_constant_writes.clear();
}

// A shader's literal constants: desc + 20 is the offset of its load table (+16 size in bytes, then
// 8-byte entries: u16 first register, u16 dwords, u32 offset from `data`), as sub_8214FA18 reads.
void ApplyShaderLiteralsLocked(uint8_t* base, uint32_t device, uint32_t desc, uint32_t data) {
  const uint32_t table_offset = LoadU32(base, desc + 20);
  if (!table_offset || !data) {
    return;
  }
  const uint32_t table = desc + table_offset;
  uint32_t entry = table + 20;
  const uint32_t end = entry + LoadU32(base, table + 16);
  for (uint32_t guard = 0; entry < end && guard < 512; ++guard, entry += 8) {
    const uint32_t header = LoadU32(base, entry);
    const uint32_t first = header >> 16;
    const uint32_t bytes = (header & 0xFFFFu) * 4u;
    if (!bytes) {
      break;
    }
    const uint32_t source = data + LoadU32(base, entry + 4);
    for (uint32_t offset = 0; offset < bytes && offset < 512u * 16u; offset += 16u) {
      const uint32_t reg = (first + offset / 16u) & 0x1FFu;
      std::memcpy(GuestPointer(base, device + DeviceOffset(1920) + reg * 16u),
                  GuestPointer(base, source + offset), std::min(16u, bytes - offset));
      g_direct_constant_groups[reg >> 8] |= uint64_t{1} << (63u - ((reg & 0xFFu) >> 2));
    }
  }
}

std::array<uint64_t, 2> TakeDirectConstantGroups(uint8_t* base, uint32_t device) {
  std::lock_guard lock(g_direct_constant_mutex);
  FlushDirectConstantWritesLocked(base);
  if (device != g_direct_constant_device) {
    g_direct_constant_groups = {};
    g_direct_constant_device = device;
  }
  // Halo's draw (sub_82174DD8) sends the changed constants, then, when device word 2 says a
  // shader changed (SetVertexShader 0x00080000, SetPixelShader 0x00100000), sub_8214F000 loads
  // the pixel and then the vertex shader's literal constants from the shader's own memory
  // (sub_8214FA18, LOAD_ALU_CONSTANT): after the changed ones, so the literals win. The renderer
  // takes the draw's state before the game's draw runs, and never saw them: they go in here.
  const uint32_t shader_dirty = uint32_t(LoadU64(base, device + 16));
  const uint32_t vertex_shader = LoadU32(base, device + 12688);
  const uint32_t pixel_shader = LoadU32(base, device + 12684);
  if (vertex_shader && (shader_dirty & 0x001E0000u)) {
    if (pixel_shader && (shader_dirty & 0x00100000u)) {
      ApplyShaderLiteralsLocked(base, device, pixel_shader + 40, LoadU32(base, pixel_shader + 24));
    }
    if (shader_dirty & 0x00180000u) {
      ApplyShaderLiteralsLocked(base, device, vertex_shader + 872,
                                LoadU32(base, vertex_shader + 32));
    }
  }
  const std::array<uint64_t, 2> groups = g_direct_constant_groups;
  g_direct_constant_groups = {};
  return groups;
}
#endif

// What changed since the last draw; observing, the game's draw clears it after.
NativeDirtyState PeekNativeDrawDirtyState(uint8_t* base, uint32_t device) {
  DirtyWords game{};
  for (uint32_t index = 0; index < game.size(); ++index) {
    game[index] = LoadU64(base, device + index * sizeof(uint64_t));
  }
#if (XPORT_TITLE_ID == 0x4D5307E6)
  const std::array<uint64_t, 2> direct_groups = TakeDirectConstantGroups(base, device);
  game[0] |= direct_groups[0];
  game[1] |= direct_groups[1];
  {
    std::lock_guard lock(g_direct_constant_mutex);
    if (g_carried_dirty_device == device) {
      for (uint32_t index = 0; index < game.size() && index < g_carried_dirty.size(); ++index) {
        game[index] |= g_carried_dirty[index];
      }
    }
    g_carried_dirty = {};
  }
#endif
  NativeDirtyState dirty_state{};
  dirty_state.words = ToReferenceDirty(game);
  return dirty_state;
}

NativeDirtyState ConsumeNativeDrawDirtyState(uint8_t* base, uint32_t device) {
  const NativeDirtyState dirty_state = PeekNativeDrawDirtyState(base, device);
  for (uint32_t index = 0; index < dirty_state.words.size(); ++index) {
    StoreU64(base, device + index * sizeof(uint64_t), 0);
  }
  return dirty_state;
}

NativeDirtyState TakeNativeDrawDirtyState(uint8_t* base, uint32_t device) {
  // Every draw site takes its dirty state here, before submitting the draw.
  NotifyDrawnBinkPlanes(base, device);
  return kObserve ? PeekNativeDrawDirtyState(base, device) : ConsumeNativeDrawDirtyState(base, device);
}

bool InitializePrimaryCpuState(PPCContext& ctx, uint8_t* base, uint32_t device,
                               uint32_t presentation) {
  InvokeGuest(ctx, base, __imp__RtlInitializeCriticalSection, device + DeviceOffset(14928));
  InvokeGuest(ctx, base, __imp__RtlInitializeCriticalSection, device + DeviceOffset(14956));
  StoreU8(base, device + DeviceOffset(10942), LoadU8(base, device + DeviceOffset(10942)) | 4);
  PublishGlobalDevice(base, device);

  const uint32_t scratch = LoadU32(base, device + DeviceOffset(16712));
  if (!scratch) {
    return false;
  }
  StoreU16(base, scratch, 0);
  PPCContext config_result =
      InvokeGuest(ctx, base, __imp__ExGetXConfigSetting, 3, 10, device + DeviceOffset(16700), 4, scratch);
  if (config_result.r3.s32 < 0) {
    return false;
  }

  StoreU32(base, device + DeviceOffset(21540), UINT32_MAX);
  StoreU32(base, device + DeviceOffset(21544), UINT32_MAX);
  ApplyDeviceCpuPrefix(base, device);

  PPCContext display_result = InvokeGuest(ctx, base, XD3D_InitDisplay, device, presentation);
  if (!display_result.r3.u32) {
    return false;
  }
  InvokeGuest(ctx, base, XD3D_InitDisplayMode, device);

  PPCContext frequency_result = InvokeGuest(ctx, base, XD3D_QueryPerformanceFrequency, scratch);
  (void)frequency_result;
  const int64_t frequency = int64_t(LoadU64(base, scratch));
  if (frequency > 0) {
    const float frequency_float = float(frequency);
    StoreU32(base, device + DeviceOffset(21568), std::bit_cast<uint32_t>(frequency_float));
    StoreU32(base, device + DeviceOffset(21572), std::bit_cast<uint32_t>(1.0f / frequency_float));
  }

  InvokeGuest(ctx, base, XD3D_InitEngines, device);
  StoreAllDirty(base, device);  // what the XD3D_DirtyAllState hook does here
  return InvokeGuest(ctx, base, XD3D_InitRing, device).r3.u32 != 0;
}

bool InitializeSecondaryCpuState(PPCContext& ctx, uint8_t* base, uint32_t device) {
  StoreU8(base, device + DeviceOffset(10940), LoadU8(base, device + DeviceOffset(10940)) | 0x80);
  StoreU32(base, device + DeviceOffset(14920), UINT32_MAX);
  StoreU32(base, device + DeviceOffset(21540), UINT32_MAX);
  StoreU32(base, device + DeviceOffset(21544), UINT32_MAX);
  ApplyDeviceCpuPrefix(base, device);
  InvokeGuest(ctx, base, XD3D_InitDisplayMode, device);
  return true;
}

void RetireNativeBoundResource(uint8_t* base, uint32_t device, uint32_t resource) {
  if (!resource) {
    return;
  }
  const uint32_t live_fence = LoadU32(base, device + kLiveResourceFenceOffset);
  if (live_fence) {
    StoreU32(base, resource + kResourceFenceOffset, live_fence);
  }
}

void ApplyNativeShaderState(PPCContext& ctx, uint8_t* base, GuestFunction implementation) {
  const uint32_t device = ctx.r3.u32;
  const uint32_t pending_resource_mask = LoadU32(base, device + kPendingResourceMaskOffset);

  // The generated shader setters combine CPU-visible shader state expansion with an
  // eight-byte PM4 resource-retirement packet. Native mode retains the former, while
  // making the packet predicate false for this synchronous call.
  StoreU32(base, device + kPendingResourceMaskOffset, 0);
  implementation(ctx, base);
  StoreU32(base, device + kPendingResourceMaskOffset, pending_resource_mask);
}

SurfaceDescriptor CaptureSurfaceDescriptor(uint8_t* base, uint32_t surface) {
  SurfaceDescriptor descriptor;
  descriptor.handle = surface;
  if (!surface) {
    return descriptor;
  }

  descriptor.flags = LoadU32(base, surface);
  descriptor.base = LoadU32(base, surface + 24);
  descriptor.address = LoadU32(base, surface + 28);
  descriptor.packed_dimensions = LoadU32(base, surface + 36);
  descriptor.format = LoadU32(base, surface + 40);
  descriptor.width = (std::rotl(descriptor.packed_dimensions, 14) & 0x3FFF) + 1;
  descriptor.height = (std::rotl(descriptor.packed_dimensions, 29) & 0x7FFF) + 1;
  descriptor.sample_type = DecodeSurfaceSampleType(descriptor.base);
  return descriptor;
}

uint32_t EncodeNativeFetchAddress(uint32_t address) {
  const uint32_t page = std::rotl(address, 12) & 0xFFF;
  const uint32_t bank = (page + 512) & 0x1000;
  return bank + (address & 0x1FFFFFFF);
}

int32_t TruncateViewportCoordinate(float value) {
  if (std::isnan(value) || value <= float(std::numeric_limits<int32_t>::min())) {
    return std::numeric_limits<int32_t>::min();
  }
  if (value >= float(std::numeric_limits<int32_t>::max())) {
    return std::numeric_limits<int32_t>::max();
  }
  return int32_t(value);
}

}  // namespace

#if (XPORT_TITLE_ID == 0x4D5307E6)
// Halo 3's direct constant packet (see DirectConstantWrite): r4 0 VS / 1 PS, r5 first register,
// r6 count; returns where the caller writes the registers (0 when out of space).
PPC_FUNC_IMPL(__imp__sub_821710C8);
PPC_FUNC(sub_821710C8) {
  if (!IsNativeMode()) {
    __imp__sub_821710C8(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t stage = ctx.r4.u32;
  const uint32_t first = ctx.r5.u32;
  const uint32_t count = ctx.r6.u32;
  {
    // The previous write's caller has filled it by now.
    std::lock_guard lock(g_direct_constant_mutex);
    FlushDirectConstantWritesLocked(base);
  }
  __imp__sub_821710C8(ctx, base);
  const uint32_t data = ctx.r3.u32;
  if (data && count && count <= 512u) {
    std::lock_guard lock(g_direct_constant_mutex);
    g_direct_constant_writes.push_back({device, data, (first - stage * 256u) & 0x1FFu, count});
  }
}

// The other way around the device's copy: sub_825A2968(device, r4 0 VS / 1 PS, r5 first register,
// r6 address, r7 registers) puts a LOAD_ALU_CONSTANT packet in the command buffer, so the GPU
// reads the registers from memory (sub_8214D8E0 does it for a parameter whose descriptor asks).
PPC_FUNC_IMPL(__imp__sub_825A2968);
PPC_FUNC(sub_825A2968) {
  if (!IsNativeMode()) {
    __imp__sub_825A2968(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t first = (ctx.r4.u32 * 256u + ctx.r5.u32) & 0x1FFu;
  const uint32_t address = ctx.r6.u32;
  const uint32_t count = ctx.r7.u32;
  __imp__sub_825A2968(ctx, base);
  if (address && count && count <= 512u) {
    std::lock_guard lock(g_direct_constant_mutex);
    g_direct_constant_writes.push_back({device, address, first, count});
  }
}

void SyncNativeVertexDeclaration(uint8_t* base, uint32_t device);

// The most sub-triangle edges per patch edge for the native tessellated draws: every patch is cut
// N x N whatever its own edge factors. The cvar (live: "cvar xnative_tess_segments=4" in
// xnative_skip.txt) wins when not 0, else XNATIVE_TESS_SEGMENTS, else 6; 1-16. 6 is the value
// checked on the phone at the waterfall (2026-11-07: looks like 8, 14.5 vs 11.2 fps); 4 was never
// seen there (its only capture was taken while the water pipeline was still compiling).
REXCVAR_DEFINE_UINT32(xnative_tess_segments, 0, "Xcompiler/Graphics/Native Renderer",
                      "Most sub-triangle edges per patch edge of tessellated draws (Halo 3 water), "
                      "1-16; 0 = XNATIVE_TESS_SEGMENTS or 6");
uint32_t NativeTessellationSegmentCap() {
  static const uint32_t environment_cap = [] {
    const char* value = std::getenv("XNATIVE_TESS_SEGMENTS");
    const long parsed = value && *value ? std::strtol(value, nullptr, 10) : 6;
    return uint32_t(std::clamp(parsed, 1L, 16L));
  }();
  const uint32_t live = REXCVAR_GET(xnative_tess_segments);
  return live ? std::clamp(live, 1u, 16u) : environment_cap;
}

// Halo's DrawIndexedTessellatedPrimitive (sub_8244C3A8: SetIndices, this, SetIndices 0; it takes
// primitive types 0x10-0x12 when device+10616 & 3 is 2). Xenia: "4D5307E6 - water - adaptive",
// triangle patches (0x11) with three edge factors per patch in the index buffer. There is no
// host tessellation stage, so the renderer draws each patch as N x N sub-triangles, N from the
// maximum level (VGT_HOS_MAX_TESS_LEVEL at device+10620, factor = level + 1) capped by
// NativeTessellationSegmentCap; the vertex shader rebuilds the domain location and patch index
// (XcTessellatedPatchInputs). Like sub_82174DD8 it sends what changed and clears all five dirty
// words: a draw it cannot take keeps those words for the next draw.
PPC_FUNC_IMPL(__imp__sub_825A6DE0);
PPC_FUNC(sub_825A6DE0) {
  if (IsNativeMode()) {
    const uint32_t device = ctx.r3.u32;
    const uint32_t primitive = ctx.r4.u32;
    const uint32_t index_count = ctx.r7.u32;
    const uint32_t mode = LoadU32(base, device + DeviceOffset(10616)) & 3u;
    const float max_level = std::bit_cast<float>(LoadU32(base, device + DeviceOffset(10620)));
    const float min_level = std::bit_cast<float>(LoadU32(base, device + DeviceOffset(10624)));
    // r7 counts patches: in adaptive mode the function sends r7 * 3 edge factors (mullw r27, r17
    // with r17 = 3 for triangle patches) as the draw's index count.
    const uint32_t patches = index_count;
    uint32_t segments = 1;
    if (std::isfinite(max_level)) {
      segments = uint32_t(std::ceil(std::clamp(max_level + 1.0f, 1.0f, 16.0f)));
    }
    segments = std::min(segments, NativeTessellationSegmentCap());
    const uint64_t vertex_count = uint64_t(patches) * 3u * segments * segments;
    const bool supported = mode == 2u && primitive == 0x11u && patches && patches <= 65535u &&
                           ctx.r5.u32 == 0 && ctx.r6.u32 == 0 &&
                           vertex_count <= (uint64_t{1} << 26);
    // The live water watch (graphics_system.cpp) reads and clears these once per frame.
    {
      auto& watch = rex::graphics::xnative::GetTessellationHookWatch();
      if (supported) {
        watch.sent.fetch_add(1, std::memory_order_relaxed);
        watch.last_sent_patches.store(patches, std::memory_order_relaxed);
        watch.last_sent_segments.store(segments, std::memory_order_relaxed);
      } else {
        watch.rejected.fetch_add(1, std::memory_order_relaxed);
        watch.rejected_primitive.store(primitive, std::memory_order_relaxed);
        watch.rejected_mode.store(mode, std::memory_order_relaxed);
        watch.rejected_base.store(ctx.r5.s32, std::memory_order_relaxed);
        watch.rejected_start.store(ctx.r6.u32, std::memory_order_relaxed);
        watch.rejected_patches.store(patches, std::memory_order_relaxed);
        watch.rejected_max_level_bits.store(std::bit_cast<uint32_t>(max_level),
                                            std::memory_order_relaxed);
      }
    }
    static std::atomic<uint32_t> reports{0};
    // Unsupported ones (the water would vanish: the guest path draws nothing natively) are
    // reported too, up to 32, whenever they happen (stepping into a puddle, 2026-11-06).
    static std::atomic<uint32_t> unsupported_reports{0};
    if (reports.fetch_add(1, std::memory_order_relaxed) < 8 ||
        (!supported && unsupported_reports.fetch_add(1, std::memory_order_relaxed) < 32)) {
      REXLOG_INFO("xnative-hook: tessellated draw type={} mode={} base={} start={} indices={} "
                  "levels={}-{} segments={} vertices={} supported={}",
                  primitive, mode, ctx.r5.s32, ctx.r6.u32, index_count, min_level, max_level,
                  segments, vertex_count, supported);
    }
    if (supported) {
      SyncNativeVertexDeclaration(base, device);
      DrawPrimitiveCommand command{};
      command.device = device;
      // kNativeTessellatedPrimitiveBase + N (native_pipeline_policy.h).
      command.primitive_type = 0x40u + segments;
      command.start_vertex = 0;
      command.vertex_count = uint32_t(vertex_count);
      command.dirty_state = TakeNativeDrawDirtyState(base, device);
      SubmitNativeCommand(command);
      if (kObserve) {
        __imp__sub_825A6DE0(ctx, base);
      }
      return;
    }
    std::lock_guard lock(g_direct_constant_mutex);
    if (g_carried_dirty_device != device) {
      g_carried_dirty = {};
      g_carried_dirty_device = device;
    }
    for (uint32_t index = 0; index < g_carried_dirty.size(); ++index) {
      g_carried_dirty[index] |= LoadU64(base, device + index * uint32_t(sizeof(uint64_t)));
    }
  }
  __imp__sub_825A6DE0(ctx, base);
}
#endif

// Each optional hook only when tools/find_d3d.py found its function (its name is then a macro).
#ifdef XD3D_RunCommandList
PPC_FUNC(XD3D_RunCommandList) {
  if (kObserve || !IsNativeMode()) {
    // XPORT_DRAW_CHAINS=1: how often the game runs a command list (draws the native renderer
    // never sees in observe mode), and what the first ones look like.
    static const bool chains = std::getenv("XPORT_DRAW_CHAINS") != nullptr;
    if (chains) {
      static std::atomic<uint32_t> runs{0};
      static std::mutex mutex;
      static std::unordered_set<uint32_t> seen;
      const uint32_t run = runs.fetch_add(1);
      const uint32_t list = ctx.r4.u32;
      bool first = false;
      {
        std::lock_guard lock(mutex);
        first = seen.insert(list).second && seen.size() <= 24;
      }
      if (first || (run % 2000) == 0) {
        std::string words;
        for (uint32_t index = 0; list && index < 32; ++index) {
          words += fmt::format(" {:08X}", LoadU32(base, list + index * 4));
        }
        REXLOG_INFO("xport-cmdlist: run #{} list={:08X} mask={:08X} lr={:08X} unique={} words:{}", run,
                    list, ctx.r5.u32, uint32_t(ctx.lr), seen.size(), words);
      }
    }
    XD3D_IMP(RunCommandList)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t command_list = ctx.r4.u32;
  const uint32_t replay_mask = ctx.r5.u32;
  if (!device || !command_list) {
    return;
  }

  const uint32_t live_fence = LoadU32(base, device + kLiveResourceFenceOffset);
  StoreU32(base, command_list + kResourceFenceOffset, live_fence);
  const uint32_t selector_tree = LoadU32(base, command_list + 112);
  if (selector_tree) {
    InvokeGuest(ctx, base, XD3D_CommandListBindFences, live_fence, selector_tree, replay_mask);
  }

  // Unlike the original GPU-list prefix, native replay has not consumed the
  // inherited CPU changes here. Keep them pending, including restoration from
  // a previous replay. An empty or selector-filtered list must not erase them.
  for (uint32_t word = 0; word < NativeDirtyState{}.words.size(); ++word) {
    const uint32_t dirty_address = device + word * sizeof(uint64_t);
    StoreU64(base, dirty_address,
             LoadU64(base, dirty_address) |
                 ~LoadU64(base, command_list + 64 + word * sizeof(uint64_t)));
  }

  uint64_t selected_count = 0;
  uint64_t submitted_count = 0;
  const bool found = ReplayCachedNativeCommandList(ctx, base, device, command_list, replay_mask,
                                                   selected_count, submitted_count);
  static std::atomic<uint64_t> replay_count{0};
  const uint64_t replay = NextNativeHookDiagnosticCall(replay_count);
  if (ShouldLogNativeHookCall(replay) || !found || selected_count != submitted_count) {
    REXLOG_INFO(
        "xnative-cache: replay #{} command-list={:08X} device={:08X} mask={:08X} "
        "found={} selected={} submitted={} capture={}",
        replay, command_list, device, replay_mask, found, selected_count, submitted_count,
        g_active_native_capture.active);
  }
}
#endif

#ifdef XD3D_DeleteCommandList
PPC_FUNC(XD3D_DeleteCommandList) {
  if (IsNativeMode()) {
    EraseCachedNativeCommandList(ctx.r3.u32);
  }
  XD3D_IMP(DeleteCommandList)(ctx, base);
}
#endif

#ifdef XD3D_DeviceClearFlagBit2
PPC_FUNC(XD3D_DeviceClearFlagBit2) {
  if (kObserve || !IsNativeMode()) {
    XD3D_IMP(DeviceClearFlagBit2)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  StoreU8(base, device + DeviceOffset(10941), LoadU8(base, device + DeviceOffset(10941)) & uint8_t(0xFB));
}
#endif

#ifdef XD3D_CreateDevice
PPC_FUNC(XD3D_CreateDevice) {
  if (!IsNativeMode()) {
    XD3D_IMP(CreateDevice)(ctx, base);
    return;
  }

  const uint32_t mode = ctx.r4.u32;
  if (kObserve) {
    const uint32_t created = ctx.r8.u32;
    XD3D_IMP(CreateDevice)(ctx, base);
    if (!ctx.r3.u32 && created && LoadU32(base, created)) {
      SubmitNativeCommand(
          DeviceCommand{{sizeof(DeviceCommand), CommandType::kDeviceCreated}, LoadU32(base, created), mode});
    }
    return;
  }
  uint32_t behavior = ctx.r6.u32;
  const uint32_t presentation = ctx.r7.u32;
  const uint32_t output = ctx.r8.u32;
  if (output) {
    StoreU32(base, output, 0);
  }

  const uint32_t device = InvokeGuest(ctx, base, XD3D_PhysicalAlloc, xd3d::kDeviceSize, 128).r3.u32;
  if (!device) {
    ctx.r3.u32 = kOutOfMemory;
    return;
  }

  if (!InvokeGuest(ctx, base, XD3D_InitDeviceCpuState, device).r3.u32) {
    FreeNativeDeviceAllocation(ctx, base, device);
    ctx.r3.u32 = kOutOfMemory;
    return;
  }
  InitializeNativeCommandScratch(base, device);

  bool initialized = false;
  if (mode == 2) {
    initialized = InitializeSecondaryCpuState(ctx, base, device);
  } else {
    if (!(behavior & 0x100) && !(behavior & 0x3F000000)) {
      behavior |= 0x0C000000;
    }
    StoreU32(base, device + DeviceOffset(22280), behavior);
    initialized = presentation && InitializePrimaryCpuState(ctx, base, device, presentation);
  }

  if (!initialized) {
    FreeNativeDeviceAllocation(ctx, base, device);
    ctx.r3.u32 = kOutOfMemory;
    return;
  }

  if (output) {
    StoreU32(base, output, device);
  }
  DeviceCommand command{{sizeof(DeviceCommand), CommandType::kDeviceCreated}, device, mode};
  SubmitNativeCommand(command);
  ctx.r3.u32 = 0;
}
#endif

#ifdef XD3D_DeviceCpuPrefix
PPC_FUNC(XD3D_DeviceCpuPrefix) {
  if (kObserve || !IsNativeMode()) {
    XD3D_IMP(DeviceCpuPrefix)(ctx, base);
    return;
  }
  ApplyDeviceCpuPrefix(base, ctx.r3.u32);
}
#endif

#ifdef XD3D_DirtyAllState
PPC_FUNC(XD3D_DirtyAllState) {
  if (kObserve || !IsNativeMode()) {
    XD3D_IMP(DirtyAllState)(ctx, base);
    return;
  }
  StoreAllDirty(base, ctx.r3.u32);
}
#endif

#ifdef XD3D_SetDeviceBits10916
PPC_FUNC(XD3D_SetDeviceBits10916) {
  if (kObserve || !IsNativeMode()) {
    XD3D_IMP(SetDeviceBits10916)(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  uint32_t value = LoadU32(base, device + DeviceOffset(10916));
  value = (value & ~uint32_t(0x3000)) | (std::rotl(ctx.r4.u32, 12) & 0x3000);
  StoreU32(base, device + DeviceOffset(10916), value);
}
#endif

#ifdef XD3D_SetShaderGprAllocation
PPC_FUNC(XD3D_SetShaderGprAllocation) {
  if (kObserve || !IsNativeMode()) {
    XD3D_IMP(SetShaderGprAllocation)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  uint32_t first = ctx.r5.u32;
  uint32_t second = ctx.r6.u32;
  if (!first && !second) {
    first = 64;
    second = 64;
  }

  uint32_t value = 0;
  if (!(ctx.r4.u32 & 1)) {
    second = (second & ~uint32_t(0x7F00)) | (std::rotl(first, 8) & 0x7F00);
    value = LoadU32(base, device + DeviceOffset(10920));
    value = (value & ~uint32_t(0x7F0)) | (std::rotl(second, 4) & 0x7F0);
    value = (value & ~uint32_t(0x7F000)) | (std::rotl(second, 4) & 0x7F000);
  }
  StoreU32(base, device + DeviceOffset(10920), value);
  ApplyNativeDirtyDefaults(base, device);
}
#endif

#ifdef XD3D_SetScissorRect
PPC_FUNC(XD3D_SetScissorRect) {
  if (kObserve || !IsNativeMode()) {
    XD3D_IMP(SetScissorRect)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t rectangle = ctx.r4.u32;
  const int32_t viewport_x =
      TruncateViewportCoordinate(std::bit_cast<float>(LoadU32(base, device + DeviceOffset(12640))));
  const int32_t viewport_y =
      TruncateViewportCoordinate(std::bit_cast<float>(LoadU32(base, device + DeviceOffset(12644))));
  const int32_t viewport_width =
      TruncateViewportCoordinate(std::bit_cast<float>(LoadU32(base, device + DeviceOffset(12648))));
  const int32_t viewport_height =
      TruncateViewportCoordinate(std::bit_cast<float>(LoadU32(base, device + DeviceOffset(12652))));

  const int32_t left = int32_t(LoadU32(base, rectangle));
  const int32_t top = int32_t(LoadU32(base, rectangle + 4));
  const int32_t right = int32_t(LoadU32(base, rectangle + 8));
  const int32_t bottom = int32_t(LoadU32(base, rectangle + 12));
  StoreU32(base, device + DeviceOffset(12668), uint32_t(left));
  StoreU32(base, device + DeviceOffset(12672), uint32_t(top));
  StoreU32(base, device + DeviceOffset(12676), uint32_t(right));
  StoreU32(base, device + DeviceOffset(12680), uint32_t(bottom));

  int32_t clipped_left = viewport_x;
  int32_t clipped_top = viewport_y;
  int32_t clipped_right = std::bit_cast<int32_t>(uint32_t(viewport_x) + uint32_t(viewport_width));
  int32_t clipped_bottom = std::bit_cast<int32_t>(uint32_t(viewport_y) + uint32_t(viewport_height));
  if (int32_t(LoadU32(base, device + DeviceOffset(11848)))) {
    clipped_left = std::max(clipped_left, left);
    clipped_top = std::max(clipped_top, top);
    clipped_right = std::min(clipped_right, right);
    clipped_bottom = std::min(clipped_bottom, bottom);
  }

  uint32_t packed_top_left = LoadU32(base, device + DeviceOffset(10436));
  packed_top_left = (packed_top_left & 0x8000FFFF) | ((uint32_t(clipped_top) << 16) & 0x7FFF0000);
  packed_top_left = (packed_top_left & 0xFFFF8000) | (uint32_t(clipped_left) & 0x00007FFF);
  uint32_t packed_bottom_right = LoadU32(base, device + DeviceOffset(10440));
  packed_bottom_right =
      (packed_bottom_right & 0x8000FFFF) | ((uint32_t(clipped_bottom) << 16) & 0x7FFF0000);
  packed_bottom_right = (packed_bottom_right & 0xFFFF8000) | (uint32_t(clipped_right) & 0x00007FFF);
  StoreU32(base, device + DeviceOffset(10436), packed_top_left);
  StoreU32(base, device + DeviceOffset(10440), packed_bottom_right);
}
#endif

// D3D predicated tiling. A title draws into EDRAM surfaces shorter than the screen and the GPU
// replays the scene once per tile, predicating each tile's resolves with SetPredication(3 << 2 * n).
// The host has no EDRAM limit: the bound surfaces grow to the area the tiles cover, BeginTiling gets
// one tile, and of the predicated resolves the first one takes the whole area and the rest are dropped.
struct CollapsedTiling {
  bool active = false;
  uint32_t width = 0;
  uint32_t height = 0;
  uint32_t predication = 0;
  uint32_t tile_width = 0;
  uint32_t tile_height = 0;
};
CollapsedTiling g_collapsed_tiling;

bool GrowSurfaceToTilingArea(uint8_t* base, uint32_t surface, uint32_t width, uint32_t height) {
  if (!surface) {
    return false;
  }
  const uint32_t address = surface + kResourcePackedDimensionsOffset;
  const uint32_t packed = LoadU32(base, address);
  const uint32_t grown = EncodeSurfaceResourceDimensions(packed, width, height);
  if (grown == packed) {
    return false;
  }
  StoreU32(base, address, grown);
  return true;
}

// A title may bind its tile-sized surfaces after BeginTiling (Halo 3 opens the bracket with the
// previous frame's full-screen targets bound and then sets its 1152x160 G-buffer and depth). Those
// grow too when bound: else they stay one tile high on the host, the scene past the first tile is
// lost, and their depth never matches the full-screen depth surface it is handed to.
void GrowTileSurfaceBoundInTiling(uint8_t* base, uint32_t surface) {
  const CollapsedTiling& tiling = g_collapsed_tiling;
  if (!tiling.active || !surface || !tiling.tile_width || !tiling.tile_height) {
    return;
  }
  const uint32_t packed = LoadU32(base, surface + kResourcePackedDimensionsOffset);
  const uint32_t width = (std::rotl(packed, 14) & 0x3FFF) + 1;
  const uint32_t height = (std::rotl(packed, 29) & 0x7FFF) + 1;
  if (width != tiling.tile_width || height != tiling.tile_height) {
    return;
  }
  if (GrowSurfaceToTilingArea(base, surface, tiling.width, tiling.height)) {
    static uint32_t logged = 0;
    if (logged < 8) {
      ++logged;
      REXLOG_INFO("xnative-tiling: surface {:08X} bound inside tiling grows {}x{} -> {}x{}", surface,
                  width, height, tiling.width, tiling.height);
    }
  }
}

PPC_FUNC(XD3D_SetRenderTarget) {
  if (!IsNativeMode()) {
    XD3D_IMP(SetRenderTarget)(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t index = ctx.r4.u32;
  const uint32_t surface = ctx.r5.u32;
  GrowTileSurfaceBoundInTiling(base, surface);
  XD3D_IMP(SetRenderTarget)(ctx, base);
  SetRenderTargetCommand command{};
  command.device = device;
  command.index = index;
  command.surface = CaptureSurfaceDescriptor(base, surface);
  SubmitNativeCommand(command);
}

// A depth surface drawn with tiling hands its contents to the next depth surface of its size at the
// same EDRAM address: on the console they are the same memory, here they are two images.
struct PendingDepthHandoff {
  uint32_t source = 0;
  uint32_t texture = 0;
  bool armed = false;
};
PendingDepthHandoff g_pending_depth_handoff;

void SubmitPendingDepthHandoff(uint8_t* base, uint32_t device, uint32_t surface, uint32_t caller) {
  PendingDepthHandoff& pending = g_pending_depth_handoff;
  if (!pending.armed || !surface || surface == pending.source) {
    return;
  }
  const SurfaceDescriptor source = CaptureSurfaceDescriptor(base, pending.source);
  const SurfaceDescriptor destination = CaptureSurfaceDescriptor(base, surface);
  if (source.address != destination.address || source.width != destination.width ||
      source.height != destination.height) {
    return;
  }
  DepthSurfaceHandoffCommand handoff{};
  handoff.device = device;
  handoff.source = source;
  handoff.destination = destination;
  handoff.source_wrapper = pending.source;
  handoff.destination_wrapper = surface;
  handoff.source_texture = pending.texture;
  handoff.trace_caller = caller;
  handoff.stencil_policy = ForwardStencilHandoffPolicy::kRebuildSceneCoverage;
  pending = {};
  SubmitNativeCommand(handoff);
}

PPC_FUNC(XD3D_SetDepthStencilSurface) {
  if (!IsNativeMode()) {
    XD3D_IMP(SetDepthStencilSurface)(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  const uint32_t surface = ctx.r4.u32;
  GrowTileSurfaceBoundInTiling(base, surface);
  XD3D_IMP(SetDepthStencilSurface)(ctx, base);
  SubmitPendingDepthHandoff(base, device, surface, uint32_t(ctx.lr));
  SetDepthStencilCommand command{};
  command.device = device;
  command.surface = CaptureSurfaceDescriptor(base, surface);
  command.trace_wrapper = surface;
  command.trace_caller = uint32_t(ctx.lr);
  SubmitNativeCommand(command);
}

PPC_FUNC(XD3D_SetStreamSource) {
  if (!IsNativeMode()) {
    XD3D_IMP(SetStreamSource)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t stream = ctx.r4.u32;
  const uint32_t buffer = ctx.r5.u32;
#if XD3D_XDK_20500
  const uint32_t offset = 0;
  const uint32_t stride = ctx.r6.u32;
  const uint64_t dirty_mask = ctx.r7.u64;
#else
  const uint32_t offset = ctx.r6.u32;
  const uint32_t stride = ctx.r7.u32;
  const uint64_t dirty_mask = ctx.r8.u64;
#endif
  if (kObserve) {
    XD3D_IMP(SetStreamSource)(ctx, base);
  }
  if (stream >= kVertexStreamCount) {
    return;
  }

  const uint32_t stride_words = (stride >> 2) & 0xFF;
  if (!kObserve) {
    if (buffer) {
      const uint32_t resource_address = LoadU32(base, buffer + 24) + offset;
      const uint32_t full_resource_size = LoadU32(base, buffer + 28) & kResourceSizeMask;
      const uint32_t resource_size = offset < full_resource_size ? full_resource_size - offset : 0;
      StoreU32(base, device + kStreamSizeBase - stream * 8, resource_size);
      StoreU32(base, device + kStreamFetchBase - stream * 8,
               EncodeNativeFetchAddress(resource_address));
      const uint32_t dirty_word = GameDirtyWord(device, 3, kReferenceFetchDirty);
      StoreU64(base, dirty_word, LoadU64(base, dirty_word) | dirty_mask);
    }

    const uint32_t slot = device + kStreamBufferBase + stream * 4;
    RetireNativeBoundResource(base, device, LoadU32(base, slot));
    StoreU32(base, slot, buffer);
    StoreU8(base, device + DeviceOffset(12520) + stream, uint8_t(stride_words));
    if (stride_words && stride_words != LoadU8(base, device + DeviceOffset(11824) + stream)) {
      MarkDirty(base, device, 2, 0x00080000);
    }
  }

  SetVertexStreamCommand command{};
  command.device = device;
  command.stream = stream;
  command.buffer = buffer;
  command.offset = offset;
  command.stride = stride;
  command.stride_words = stride_words;
  SubmitNativeCommand(command);
}

PPC_FUNC(XD3D_SetIndices) {
  if (!IsNativeMode()) {
    XD3D_IMP(SetIndices)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t buffer = ctx.r4.u32;
  if (kObserve) {
    XD3D_IMP(SetIndices)(ctx, base);
  } else {
    const uint32_t slot = device + kIndexBufferOffset;
    RetireNativeBoundResource(base, device, LoadU32(base, slot));
    StoreU32(base, slot, buffer);
  }

  SetIndexBufferCommand command{};
  command.device = device;
  command.buffer = buffer;
  SubmitNativeCommand(command);
}

PPC_FUNC(XD3D_LockTextureRectArray) {
  const uint32_t texture = ctx.r3.u32;
  // With XDK 2.0.20500 it calls XD3D_LockTextureRect (the worker), whose hook reads it back.
  if (!XD3D_XDK_20500 && IsNativeMode() && texture && ctx.r8.u32 == 0) {
    TextureLockCommand command;
    command.texture = texture;
    command.array_index = ctx.r4.u32;
    command.level = ctx.r5.u32;
    command.flags = ctx.r8.u32;
    command.dimension = TextureLockDimension::k2D;
    TextureLockResult result{};
    ExecuteNativeCommand(command, result);
  }
  XD3D_IMP(LockTextureRectArray)(ctx, base);
}

PPC_FUNC(XD3D_LockTextureRect) {
  const uint32_t texture = ctx.r3.u32;
#if XD3D_XDK_20500
  // The worker: (texture, array index, level, flags, ...).
  if (IsNativeMode() && texture && ctx.r6.u32 == 0) {
    TextureLockCommand command;
    command.texture = texture;
    command.array_index = ctx.r4.u32;
    command.level = ctx.r5.u32;
    command.flags = ctx.r6.u32;
    command.dimension = TextureLockDimension::k2D;
    TextureLockResult result{};
    ExecuteNativeCommand(command, result);
  }
  XD3D_IMP(LockTextureRect)(ctx, base);
  return;
#endif
  if (IsNativeMode() && texture && ctx.r7.u32 == 0) {
    TextureLockCommand command;
    command.texture = texture;
    command.array_index = 0;
    command.level = ctx.r4.u32;
    command.flags = ctx.r7.u32;
    command.dimension = TextureLockDimension::k3D;
    TextureLockResult result{};
    ExecuteNativeCommand(command, result);
  }
  XD3D_IMP(LockTextureRect)(ctx, base);
}

PPC_FUNC(XD3D_ReleaseResource) {
  const bool native_mode = IsNativeMode();
  const uint32_t resource = ctx.r3.u32;
  XD3D_IMP(ReleaseResource)(ctx, base);

  // The generated implementation returns zero only after the guest reference
  // count reaches zero and the resource destructor has run. Notify the native
  // renderer at that exact ownership boundary. Previously its handle caches
  // retained the final shared_ptr forever, which also kept the corresponding
  // MoltenVK image and device memory alive.
  if (native_mode && resource && ctx.r3.u32 == 0) {
    {
      std::lock_guard lock(g_native_resource_lock_mutex);
      g_native_resource_locks.erase(resource);
      g_persistent_locks.erase(resource);
    }
    ForgetNativeShaderObject(resource);
#ifdef XD3D_BindVertexShader
    ForgetBoundVertexDeclaration(resource);
#endif
    ReleaseResourceCommand command;
    command.resource = resource;
    SubmitNativeCommand(command);
  }
}

PPC_FUNC(XD3D_LockResource) {
  if (!IsNativeMode()) {
    XD3D_IMP(LockResource)(ctx, base);
    return;
  }

  const uint32_t resource = ctx.r3.u32;
  if (!resource) {
    ctx.r3.u32 = 0;
    return;
  }
  const uint32_t secondary_selector = ctx.r5.u32;
  const uint32_t primary_base = ctx.r6.u32;
  const uint32_t secondary_base = ctx.r7.u32;
  const uint32_t pointer = ctx.r8.u32;
  const uint32_t size = ctx.r9.u32;
  const uint32_t flags = ctx.r10.u32;
  if ((flags & kPersistentLockFlag) && IsBinkPlaneTexture(base, resource)) {
    std::lock_guard lock(g_native_resource_lock_mutex);
    g_persistent_locks.insert(resource);
  }
  const uint32_t range_base = secondary_selector && secondary_base ? secondary_base : primary_base;
  if (kObserve) {
    // The renderer learns of the range at the unlock.
    RecordNativeResourceLock(resource, range_base, pointer, size, flags, ctx.lr);
    XD3D_IMP(LockResource)(ctx, base);
    return;
  }

  UpdateGuestDirtyRange(base, resource, secondary_selector, primary_base, secondary_base, pointer,
                        size, flags);
  RecordNativeResourceLock(resource, range_base, pointer, size, flags, ctx.lr);
  UpdateResourceLockCount(base, resource, 256);

  if (flags & 0x10u) {
    const uint32_t page = std::rotl(pointer, 12) & 0xFFFu;
    const uint32_t bank = (page + 512u) & 0x1000u;
    ctx.r3.u32 = bank + (pointer & 0x1FFFFFFFu) - 0x40000000u;
  } else {
    ctx.r3.u32 = pointer;
  }
}

PPC_FUNC(XD3D_UnlockResource) {
  if (!IsNativeMode()) {
    XD3D_IMP(UnlockResource)(ctx, base);
    return;
  }

  const uint32_t resource = ctx.r3.u32;
  const uint32_t caller = uint32_t(ctx.lr);
  if (!resource) {
    if (kObserve) {
      XD3D_IMP(UnlockResource)(ctx, base);
    }
    return;
  }
  // The lock count (bits 8-11 of the first word): the game's function takes one off.
  const uint32_t previous =
      kObserve ? LoadU32(base, resource) : UpdateResourceLockCount(base, resource, -256);
  const bool outermost = (previous & 0xF00) == 0x100;
  const NativeResourceLockState lock_state = ConsumeNativeResourceLock(resource, outermost);
  if (kObserve) {
    XD3D_IMP(UnlockResource)(ctx, base);
  } else if (outermost) {
    if (LoadU32(base, resource + 20) != 0xFFFF0000) {
      StoreU32(base, resource + 20, 0xFFFF0000);
    }
    if (ctx.r5.u32 && LoadU32(base, resource + 24) != 0xFFFF0000) {
      StoreU32(base, resource + 24, 0xFFFF0000);
    }
  }

  if (outermost) {
    ResourceUnlockCommand command;
    command.resource = resource;
    command.access = lock_state.guest_write ? ResourceUnlockAccess::kGuestWrite
                                            : ResourceUnlockAccess::kNoDirtyUpdate;
    command.flags = lock_state.flags;
    command.range_offset = lock_state.minimum_offset == UINT32_MAX ? 0 : lock_state.minimum_offset;
    command.range_length = lock_state.maximum_end >= command.range_offset
                               ? lock_state.maximum_end - command.range_offset
                               : 0;
    command.lock_caller = lock_state.lock_caller;
    command.unlock_caller = caller;
    SubmitNativeCommand(command);
  }
}

namespace {
// A resolved depth texture read through another texture header over the same memory (a 32-bit color
// view of the depth), which on the console is the same pixels. The host keeps the resolved depth in
// an image, so the renderer has to be told the second header is a view of the first.
std::unordered_map<uint32_t, uint32_t> g_resolved_depth_by_memory;
std::unordered_map<uint32_t, uint32_t> g_packed_depth_views;

uint32_t TextureMemory(uint8_t* base, uint32_t texture) {
  return LoadU32(base, texture + 32) & 0xFFFFF000;
}

#if XD3D_XDK_20500
// Gears of War 3's color grading table: drawn into a 16x512 k_8_8_8_8 target, resolved to a 2D texture
// and read through another header over the same memory, stacked 16x16x16, each slice 32 rows apart.
// The host keeps the resolve in an image, so the second header has to be told it is a view of the first
// (the renderer copies the slices). Without it the table reads empty and the whole picture is black.
std::unordered_map<uint32_t, uint32_t> g_resolved_color_by_memory;
std::unordered_map<uint32_t, uint32_t> g_stacked_views;

uint32_t PhysicalTextureMemory(uint8_t* base, uint32_t texture) {
  return LoadU32(base, texture + 32) & 0x1FFFF000;
}

void RegisterStackedView(uint8_t* base, uint32_t texture) {
  // Fetch word 1, bit 10: stacked.
  const uint32_t word1 = LoadU32(base, texture + 32);
  if (!(word1 & 0x400) || (word1 & 0x3F) != 6) {
    return;
  }
  const auto source = g_resolved_color_by_memory.find(PhysicalTextureMemory(base, texture));
  if (source == g_resolved_color_by_memory.end() || source->second == texture) {
    return;
  }
  auto& registered = g_stacked_views[texture];
  if (registered == source->second) {
    return;
  }
  registered = source->second;
  const uint32_t dimensions = LoadU32(base, texture + kResourcePackedDimensionsOffset);
  const uint32_t width = (dimensions & 0x1FFF) + 1;
  const uint32_t height = ((dimensions >> 13) & 0x1FFF) + 1;
  RegisterVirtualResourceCommand registration{};
  registration.resource = texture;
  registration.kind = VirtualResourceKind::kTexture;
  registration.wrapper = texture;
  registration.companion = source->second;
  registration.guest_backing_width = width;
  registration.guest_backing_height = height;
  registration.logical_width = width;
  registration.logical_height = height;
  registration.physical_width = width;
  registration.physical_height = height;
  registration.scale_domain = VirtualResourceScaleDomain::kLogical;
  registration.packed_depth_source = source->second;
  SubmitNativeCommand(registration);
}
#endif

void RegisterPackedDepthView(uint8_t* base, uint32_t texture) {
  if (!texture) {
    return;
  }
#if XD3D_XDK_20500
  RegisterStackedView(base, texture);
#endif
  // Fetch word 1: format in the low 6 bits, 6 = k_8_8_8_8.
  if ((LoadU32(base, texture + 32) & 0x3F) != 6) {
    return;
  }
  const auto source = g_resolved_depth_by_memory.find(TextureMemory(base, texture));
  if (source == g_resolved_depth_by_memory.end() || source->second == texture) {
    return;
  }
#if XD3D_XDK_20500
  // Gears 3 hands the same resolve memory to textures of other sizes, one after the other (a 1120x688
  // shadow depth, then a 1280x720 color target). Only a texture of the depth's own size is a view of it.
  if (LoadU32(base, texture + kResourcePackedDimensionsOffset) !=
      LoadU32(base, source->second + kResourcePackedDimensionsOffset)) {
    return;
  }
#endif
  auto& registered = g_packed_depth_views[texture];
  if (registered == source->second) {
    return;
  }
  registered = source->second;
  const uint32_t dimensions = LoadU32(base, texture + kResourcePackedDimensionsOffset);
  const uint32_t width = (dimensions & 0x1FFF) + 1;
  const uint32_t height = ((dimensions >> 13) & 0x1FFF) + 1;
  RegisterVirtualResourceCommand registration{};
  registration.resource = texture;
  registration.kind = VirtualResourceKind::kTexture;
  registration.wrapper = texture;
  registration.companion = source->second;
  registration.guest_backing_width = width;
  registration.guest_backing_height = height;
  registration.logical_width = width;
  registration.logical_height = height;
  registration.physical_width = width;
  registration.physical_height = height;
  registration.scale_domain = VirtualResourceScaleDomain::kLogical;
  registration.packed_depth_source = source->second;
  SubmitNativeCommand(registration);
}

}  // namespace

PPC_FUNC(XD3D_SetTexture) {
  if (!IsNativeMode()) {
    XD3D_IMP(SetTexture)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t stage = ctx.r4.u32;
  const uint32_t texture = ctx.r5.u32;
  const uint64_t dirty_mask = ctx.r6.u64;
  // The movie's planes: the draws tell the renderer (NotifyDrawnBinkPlanes).
  NoteBoundBinkPlane(base, stage, texture);
  if (texture && stage < kTextureStageCount && IsPersistentLock(texture)) {
    // Written by the CPU since the last bind, maybe (g_persistent_locks).
    ResourceUnlockCommand written{};
    written.resource = texture;
    written.access = ResourceUnlockAccess::kGuestWrite;
    written.flags = kPersistentLockFlag;
    written.unlock_caller = uint32_t(ctx.lr);
    SubmitNativeCommand(written);
  }
  if (kObserve) {
    XD3D_IMP(SetTexture)(ctx, base);
    if (stage < kTextureStageCount) {
      RegisterPackedDepthView(base, texture);
      SetTextureCommand command{};
      command.device = device;
      command.stage = stage;
      command.texture = texture;
      SubmitNativeCommand(command);
    }
    return;
  }
  if (stage >= kTextureStageCount) {
    return;
  }

  const uint32_t slot = device + kTextureHandleBase + stage * 4;
  const uint32_t previous = LoadU32(base, slot);
  std::array<uint32_t, 6> source_descriptor{};
  std::array<uint32_t, 6> final_fetch{};
  if (texture) {
    const uint32_t fetch = device + kTextureFetchBase + stage * 24;
    for (size_t index = 0; index < source_descriptor.size(); ++index) {
      source_descriptor[index] = LoadU32(base, texture + 28 + uint32_t(index) * 4);
    }
    uint32_t word0 = source_descriptor[0];
    uint32_t word4 = EncodeNativeFetchAddress(source_descriptor[1]);
    const uint32_t word8 = source_descriptor[2];
    uint32_t word12 = source_descriptor[3];
    uint32_t word16 = source_descriptor[4];
    uint32_t word20 = EncodeNativeFetchAddress(source_descriptor[5]);
    word0 = (word0 & 0xFFC003FF) | (LoadU32(base, fetch) & 0x003FFC00);
    word4 = (word4 & 0xFFFFF7FF) | (LoadU32(base, fetch + 4) & 0x00000800);
    word12 = (word12 & 0x8007FFFF) | (LoadU32(base, fetch + 12) & 0x7FF80000);
    word16 = (word16 & 0x000003FC) | (LoadU32(base, fetch + 16) & 0xFFFFFC03);

    uint32_t lower_mip = std::rotl(LoadU32(base, texture + 44), 30) & 0xF;
    lower_mip = std::max<uint32_t>(lower_mip, LoadU8(base, device + DeviceOffset(11942) + stage));
    word16 = (word16 & 0xFFFFFFC3) | ((lower_mip << 2) & 0x3C);
    uint32_t upper_mip = std::rotl(LoadU32(base, texture + 44), 26) & 0xF;
    upper_mip = std::min<uint32_t>(upper_mip, LoadU8(base, device + DeviceOffset(11968) + stage));
    word16 = (word16 & 0xFFFFFC3F) | ((upper_mip << 6) & 0x3C0);
    word20 = (word20 & 0xFFFFFE00) | (LoadU32(base, fetch + 20) & 0x000001FF);

    StoreU32(base, fetch, word0);
    StoreU32(base, fetch + 4, word4);
    StoreU32(base, fetch + 8, word8);
    StoreU32(base, fetch + 12, word12);
    StoreU32(base, fetch + 16, word16);
    StoreU32(base, fetch + 20, word20);
    final_fetch = {word0, word4, word8, word12, word16, word20};
    const uint32_t dirty_word = GameDirtyWord(device, 3, kReferenceFetchDirty);
    StoreU64(base, dirty_word, LoadU64(base, dirty_word) | dirty_mask);
  }

  StoreU32(base, slot, texture);
  RetireNativeBoundResource(base, device, previous);
  RegisterPackedDepthView(base, texture);
  SetTextureCommand command{};
  command.device = device;
  command.stage = stage;
  command.texture = texture;
  SubmitNativeCommand(command);
}

#ifdef XD3D_AcquireCommandScratch
PPC_FUNC(XD3D_AcquireCommandScratch) {
  if (kObserve || !IsNativeMode()) {
    XD3D_IMP(AcquireCommandScratch)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t scratch = LoadU32(base, device + DeviceOffset(16712));
  if (!scratch) {
    ctx.r3.u32 = 0;
    return;
  }
  StoreU32(base, device + kCommandScratchWriteOffset, scratch);
  StoreU32(base, device + kCommandScratchEndOffset, scratch + kCommandScratchSize);
  ctx.r3.u32 = scratch;
}
#endif

PPC_FUNC(XD3D_SetPixelShader) {
  if (!IsNativeMode()) {
    XD3D_IMP(SetPixelShader)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t shader = ctx.r4.u32;
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
  SubmitReadyShaderRegistrations();
#endif
  RegisterNativePixelShaderOnSet(base, shader);
  if (kObserve) {
    XD3D_IMP(SetPixelShader)(ctx, base);
  } else {
    ApplyNativeShaderState(ctx, base, XD3D_IMP(SetPixelShader));
  }
  TrackNativeShaderBinding(device, ShaderStage::kPixel, shader);
  SetShaderCommand command{
      {sizeof(SetShaderCommand), CommandType::kSetPixelShader}, device, shader};
  SubmitNativeCommand(command);
}

PPC_FUNC(XD3D_CreatePixelShader) {
  RegisterNativeShader(ctx, base, XD3D_IMP(CreatePixelShader), ShaderStage::kPixel);
}

PPC_FUNC(XD3D_CreateVertexShader) {
  RegisterNativeShader(ctx, base, XD3D_IMP(CreateVertexShader), ShaderStage::kVertex);
}

#ifdef XD3D_InitVertexShader
// D3D's fill of a vertex shader object (r3) over its microcode (r4): CreateVertexShader calls it,
// and so does an engine that allocates the shader itself. The microcode is still as built here.
PPC_FUNC(XD3D_InitVertexShader) {
  const uint32_t shader = ctx.r3.u32;
  const uint32_t microcode = ctx.r4.u32;
  XD3D_IMP(InitVertexShader)(ctx, base);
  if (!g_creating_vertex_shader) {
    RegisterNativeShaderObject(base, shader, microcode, ShaderStage::kVertex);
  }
}
#endif

PPC_FUNC(XD3D_SetVertexShader) {
  if (!IsNativeMode()) {
    XD3D_IMP(SetVertexShader)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t shader = ctx.r4.u32;
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
  SubmitReadyShaderRegistrations();
#endif
  if (kObserve) {
    XD3D_IMP(SetVertexShader)(ctx, base);
  } else {
    ApplyNativeShaderState(ctx, base, XD3D_IMP(SetVertexShader));
  }
  TrackNativeShaderBinding(device, ShaderStage::kVertex, shader);
#ifdef XD3D_BindVertexShader
  RememberDeviceVertexShader(device, shader);
#endif
  SetShaderCommand command{
      {sizeof(SetShaderCommand), CommandType::kSetVertexShader}, device, shader};
  SubmitNativeCommand(command);
}

#ifdef XD3D_SetVertexDeclaration
PPC_FUNC(XD3D_SetVertexDeclaration) {
  if (!IsNativeMode()) {
    XD3D_IMP(SetVertexDeclaration)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t declaration = ctx.r4.u32;
  if (kObserve) {
    XD3D_IMP(SetVertexDeclaration)(ctx, base);
  } else {
    StoreU32(base, device + kVertexDeclarationOffset, declaration);
    MarkDirty(base, device, 2, 0x00080000);
  }

  SubmitNativeVertexDeclaration(base, device, declaration);

  SetVertexDeclarationCommand command{};
  command.device = device;
  command.declaration = declaration;
  SubmitNativeCommand(command);
}
#endif

#ifdef XD3D_BindVertexShader
// IDirect3DVertexShader9::Bind (r3 the shader, r5 a declaration): patches the microcode for that
// declaration ahead, so the game can then draw with none set (Unreal Engine 3 does).
PPC_FUNC(XD3D_BindVertexShader) {
  const uint32_t shader = ctx.r3.u32;
  // XDK 2.0.20500's has no flags: (shader, declaration, strides, pixel shader).
  const uint32_t declaration = XD3D_XDK_20500 ? ctx.r4.u32 : ctx.r5.u32;
  XD3D_IMP(BindVertexShader)(ctx, base);
  RememberBoundVertexDeclaration(shader, declaration);
}
#endif

#if defined(XD3D_SetVertexDeclaration) && !defined(XD3D_BindVertexShader)
void SyncNativeVertexDeclaration(uint8_t*, uint32_t) {}
#else
// The game's SetVertexDeclaration is inlined into its callers (no XD3D_SetVertexDeclaration), or
// it draws with none set and its vertex shader bound to one (XD3D_BindVertexShader): what the
// device holds reaches the renderer at the next draw.
void SyncNativeVertexDeclaration(uint8_t* base, uint32_t device) {
  thread_local uint32_t synced_device = 0;
  thread_local uint32_t synced_declaration = 0;
  uint32_t declaration = LoadU32(base, device + kVertexDeclarationOffset);
#ifdef XD3D_BindVertexShader
  if (!declaration) {
    declaration = BoundVertexDeclaration(device);
  }
#endif
  if (device == synced_device && declaration == synced_declaration) {
    return;
  }
  synced_device = device;
  synced_declaration = declaration;
  SubmitNativeVertexDeclaration(base, device, declaration);
  SetVertexDeclarationCommand command{};
  command.device = device;
  command.declaration = declaration;
  SubmitNativeCommand(command);
}
#endif

#ifdef XD3D_SetVertexDeclarationDirect
PPC_FUNC(XD3D_SetVertexDeclarationDirect) {
  if (!IsNativeMode()) {
    XD3D_IMP(SetVertexDeclarationDirect)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t declaration = ctx.r4.u32;
  if (kObserve) {
    XD3D_IMP(SetVertexDeclarationDirect)(ctx, base);
  } else {
    StoreU32(base, device + kLegacyVertexDeclarationOffset, declaration);
    MarkDirty(base, device, 2, 0x4000000000);
  }

  SubmitNativeVertexDeclaration(base, device, declaration);
  SetVertexDeclarationCommand command{};
  command.device = device;
  command.declaration = declaration;
  SubmitNativeCommand(command);
}
#endif

// Draws whose vertices the title writes between the two (DrawPrimitiveUP). Observing, the game's
// functions put them in the ring buffer, which the renderer does not read: they are not drawn.
#ifdef XD3D_BeginVertices
PPC_FUNC(XD3D_BeginVertices) {
  if (!IsNativeMode()) {
    XD3D_IMP(BeginVertices)(ctx, base);
    return;
  }
  if (kObserve) {
    // The game's function returns where the title writes the vertices. Its EndVertices may be
    // inlined (UE3), so the draw goes now, with this state, and the renderer copies the
    // vertices at the next command the hooks send.
    const uint32_t device = ctx.r3.u32;
    DrawPrimitiveUpCommand command{};
    command.device = device;
    command.primitive_type = ctx.r4.u32;
    command.vertex_count = ctx.r5.u32;
    command.stride = ctx.r6.u32;
    const uint64_t vertex_data_size = uint64_t(command.vertex_count) * command.stride;
    SyncNativeVertexDeclaration(base, device);
    command.dirty_state = TakeNativeDrawDirtyState(base, device);
    XD3D_IMP(BeginVertices)(ctx, base);
    if (ctx.r3.u32 && vertex_data_size && vertex_data_size <= UINT32_MAX) {
      command.vertex_data = ctx.r3.u32;
      command.vertex_data_size = uint32_t(vertex_data_size);
      command.payload_deferred = 1;
      SubmitNativeCommand(command);
    }
    return;
  }

  const uint64_t call = g_indexed_draw_invocation_id.fetch_add(1, std::memory_order_relaxed) + 1;
  if (ShouldLogNativeHookCall(call)) {
    REXLOG_INFO(
        "xnative-hook: DrawPrimitiveUPBegin #{} lr={:08X} device={:08X} primitive={} "
        "vertices={} stride={}",
        call, ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }

  if (g_pending_draw_primitive_up.vertex_data) {
    FreeAlignedGuestAllocation(ctx, base, g_pending_draw_primitive_up.vertex_data);
    g_pending_draw_primitive_up = {};
  }

  const uint32_t device = ctx.r3.u32;
  const uint32_t primitive_type = ctx.r4.u32;
  const uint32_t vertex_count = ctx.r5.u32;
  const uint32_t stride = ctx.r6.u32;
  const uint64_t vertex_data_size_64 = uint64_t(vertex_count) * uint64_t(stride);
  if (!vertex_data_size_64 || vertex_data_size_64 > UINT32_MAX) {
    ctx.r3.u32 = 0;
    return;
  }
  const uint32_t vertex_data_size = uint32_t(vertex_data_size_64);
  const uint32_t vertex_data =
      InvokeGuest(ctx, base, XD3D_PhysicalAlloc, vertex_data_size, 16).r3.u32;
  if (!vertex_data) {
    ctx.r3.u32 = 0;
    return;
  }

  const NativeDirtyState dirty_state = ConsumeNativeDrawDirtyState(base, device);
  StoreU32(base, device + kUpCommandWriteOffset, LoadU32(base, device + DeviceOffset(48)));
  StoreU32(base, device + kUpVertexDataOffset, vertex_data);
  StoreU32(base, device + kUpVertexWordCountOffset, vertex_data_size >> 2);
  g_pending_draw_primitive_up.device = device;
  g_pending_draw_primitive_up.primitive_type = primitive_type;
  g_pending_draw_primitive_up.vertex_count = vertex_count;
  g_pending_draw_primitive_up.stride = stride;
  g_pending_draw_primitive_up.vertex_data = vertex_data;
  g_pending_draw_primitive_up.vertex_data_size = vertex_data_size;
  g_pending_draw_primitive_up.dirty_state = dirty_state;
  ctx.r3.u32 = vertex_data;
}
#endif

// DrawIndexedPrimitiveUP, observing (not in the reference XDK's list): as BeginVertices, the
// title writes the indices and vertices after the call; the renderer expands the indices then.
#ifdef XD3D_BeginIndexedVertices
PPC_FUNC(XD3D_BeginIndexedVertices) {
  if (!kObserve || !IsNativeMode()) {
    XD3D_IMP(BeginIndexedVertices)(ctx, base);
    return;
  }

  // (device, primitive, base vertex, vertices, indices, index format, stride, &indices,
  // &vertices): the ninth argument is on the stack.
  const uint32_t device = ctx.r3.u32;
  DrawPrimitiveUpCommand command{};
  command.device = device;
  command.primitive_type = ctx.r4.u32;
  command.base_vertex = ctx.r5.s32;
  command.source_vertex_count = ctx.r6.u32;
  command.vertex_count = ctx.r7.u32;
  command.index_32bit = (ctx.r8.u32 & 4) ? 1 : 0;  // D3DFMT_INDEX32
  command.stride = ctx.r9.u32;
  const uint32_t index_pointer = ctx.r10.u32;
  const uint32_t vertex_pointer = LoadU32(base, ctx.r1.u32 + 84);
  const uint64_t vertex_data_size = uint64_t(command.vertex_count) * command.stride;
  SyncNativeVertexDeclaration(base, device);
  command.dirty_state = TakeNativeDrawDirtyState(base, device);
  XD3D_IMP(BeginIndexedVertices)(ctx, base);
  if (ctx.r3.u32 || !index_pointer || !vertex_pointer || !vertex_data_size ||
      vertex_data_size > UINT32_MAX) {
    return;
  }
  command.index_data = LoadU32(base, index_pointer);
  command.vertex_data = LoadU32(base, vertex_pointer);
  if (command.index_data && command.vertex_data) {
    command.vertex_data_size = uint32_t(vertex_data_size);
    command.payload_deferred = 1;
    SubmitNativeCommand(command);
  }
}
#endif

#ifdef XD3D_EndVertices
PPC_FUNC(XD3D_EndVertices) {
  if (kObserve || !IsNativeMode()) {
    XD3D_IMP(EndVertices)(ctx, base);
    return;
  }

  static std::atomic<uint64_t> call_count{0};
  const uint64_t call = NextNativeHookDiagnosticCall(call_count);
  if (ShouldLogNativeHookCall(call)) {
    REXLOG_INFO(
        "xnative-hook: DrawPrimitiveUPCommit #{} lr={:08X} device={:08X} "
        "pending-device={:08X} pending-data={:08X}",
        call, ctx.lr, ctx.r3.u32, g_pending_draw_primitive_up.device,
        g_pending_draw_primitive_up.vertex_data);
  }

  const uint32_t device = ctx.r3.u32;
  StoreU32(base, device + DeviceOffset(48), LoadU32(base, device + kUpCommandWriteOffset));
  if (g_pending_draw_primitive_up.device != device || !g_pending_draw_primitive_up.vertex_data) {
    return;
  }

  DrawPrimitiveUpCommand command{};
  command.device = device;
  command.primitive_type = g_pending_draw_primitive_up.primitive_type;
  command.vertex_count = g_pending_draw_primitive_up.vertex_count;
  command.stride = g_pending_draw_primitive_up.stride;
  command.vertex_data = g_pending_draw_primitive_up.vertex_data;
  command.vertex_data_size = g_pending_draw_primitive_up.vertex_data_size;
  command.dirty_state = g_pending_draw_primitive_up.dirty_state;
  const NativeDirtyState commit_dirty_state = ConsumeNativeDrawDirtyState(base, device);
  for (size_t index = 0; index < command.dirty_state.words.size(); ++index) {
    command.dirty_state.words[index] |= commit_dirty_state.words[index];
  }
  SubmitNativeCommand(command);
  FreeAlignedGuestAllocation(ctx, base, g_pending_draw_primitive_up.vertex_data);
  g_pending_draw_primitive_up = {};
}
#endif

PPC_FUNC(XD3D_DrawVertices) {
  if (!IsNativeMode()) {
    XD3D_IMP(DrawVertices)(ctx, base);
    return;
  }

  static std::atomic<uint64_t> call_count{0};
  const uint64_t call = NextNativeHookDiagnosticCall(call_count);
  if (ShouldLogNativeHookCall(call)) {
    REXLOG_INFO(
        "xnative-hook: DrawPrimitive #{} lr={:08X} device={:08X} primitive={} "
        "start={} vertices={}",
        call, ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }

  const uint32_t device = ctx.r3.u32;
#if XPORT_TITLE_ID == 0x4D5308AB && defined(XD3D_BindVertexShader)
  // The engine's GPU memory copy (texture pool defragmentation): done on guest memory, no draw.
  {
    uint32_t copy_shader_object = 0;
    GpuCopyShader copy_shader;
    if (FindBoundGpuCopyShader(device, copy_shader_object, copy_shader)) {
      const uint32_t start_vertex = ctx.r5.u32;
      const uint32_t vertex_count = ctx.r6.u32;
      if (kObserve) {
        XD3D_IMP(DrawVertices)(ctx, base);
      }
      RunGpuMemoryCopy(base, device, copy_shader_object, copy_shader, start_vertex, vertex_count);
      return;
    }
  }
#endif
  SyncNativeVertexDeclaration(base, device);
  NoteDrawChain(ctx, base, "vertices");
  DrawPrimitiveCommand command{};
  command.device = device;
  command.primitive_type = ctx.r4.u32;
  command.start_vertex = ctx.r5.u32;
  command.vertex_count = ctx.r6.u32;
  command.dirty_state = TakeNativeDrawDirtyState(base, device);
  SubmitNativeCommand(command);
  if (kObserve) {
    XD3D_IMP(DrawVertices)(ctx, base);
  }
}

void SubmitNativeResolve(uint8_t* base, uint32_t device, uint32_t flags,
                         const ResolveRectangle* source_rectangle, uint32_t destination_texture,
                         const ResolvePoint* destination_point, uint32_t destination_level,
                         uint32_t destination_slice_or_face, const uint32_t* clear_color_bits,
                         double clear_depth, uint32_t clear_stencil, uint32_t parameters,
                         uint32_t trace_origin, uint32_t trace_caller, uint32_t trace_owner,
                         uint32_t trace_source_wrapper = 0) {
  // With no destination a resolve only clears: Gears clears its scene depth like this.
  if (!device || (!destination_texture && !(flags & 0x300u))) {
    return;
  }

  ResolveCommand command{};
  command.device = device;
  command.flags = flags;
  const uint32_t source_index = flags & 7;
  uint32_t source_surface = 0;
  if (source_index == 4) {
    source_surface = LoadU32(base, device + kDepthStencilOffset);
  } else if (source_index < kRenderTargetCount) {
    source_surface = LoadU32(base, device + kRenderTargetOffset + source_index * sizeof(uint32_t));
  }
  command.source = CaptureSurfaceDescriptor(base, source_surface);
  command.flags = NormalizeResolveSampleFlags(command.flags, command.source.sample_type);

  command.source_rectangle_valid = source_rectangle != nullptr;
  if (source_rectangle) {
    command.source_rectangle = *source_rectangle;
  }

  command.destination_texture = destination_texture;
  command.trace_origin = trace_origin;
  command.trace_caller = trace_caller;
  command.trace_owner = trace_owner;
  command.trace_source_wrapper = trace_source_wrapper;
  for (uint32_t index = 0; destination_texture && index < 6; ++index) {
    command.destination_fetch[index] =
        LoadU32(base, destination_texture + 28 + index * sizeof(uint32_t));
  }
  if (source_index == 4 && destination_texture) {
    g_resolved_depth_by_memory[TextureMemory(base, destination_texture)] = destination_texture;
  }
#if XD3D_XDK_20500
  // A color resolve over the same memory: what is there is no longer that depth.
  if (source_index != 4 && destination_texture) {
    g_resolved_depth_by_memory.erase(TextureMemory(base, destination_texture));
    g_resolved_color_by_memory[PhysicalTextureMemory(base, destination_texture)] = destination_texture;
  }
#endif
  if (rex::diagnostics::IsEnabled(rex::diagnostics::Category::kGuestHooks) &&
      (flags & 0x04000000u)) {
    std::fprintf(
        stderr,
        "[ResolveOriginTrace] origin=%s caller=%08X device=%08X flags=%08X "
        "source=%08X/%08X source-size=%ux%u destination=%08X "
        "fetch=%08X,%08X,%08X,%08X,%08X,%08X rect=%u:%d,%d,%d,%d "
        "point=%u:%d,%d level=%u slice=%u\n",
        trace_origin == 1   ? "direct"
        : trace_origin == 2 ? "batch"
        : trace_origin == 3 ? "deferred-full-frame"
                            : "unknown",
        trace_caller, device, flags, command.source.handle, command.source.address,
        command.source.width, command.source.height, destination_texture,
        command.destination_fetch[0], command.destination_fetch[1], command.destination_fetch[2],
        command.destination_fetch[3], command.destination_fetch[4], command.destination_fetch[5],
        unsigned(source_rectangle != nullptr), source_rectangle ? source_rectangle->left : 0,
        source_rectangle ? source_rectangle->top : 0,
        source_rectangle ? source_rectangle->right : 0,
        source_rectangle ? source_rectangle->bottom : 0, unsigned(destination_point != nullptr),
        destination_point ? destination_point->x : 0, destination_point ? destination_point->y : 0,
        destination_level, destination_slice_or_face);
    std::fflush(stderr);
  }
  command.destination_point_valid = destination_point != nullptr;
  if (destination_point) {
    command.destination_point = *destination_point;
  }
  command.destination_level = destination_level;
  command.destination_slice_or_face = destination_slice_or_face;
  if (clear_color_bits) {
    for (uint32_t index = 0; index < 4; ++index) {
      command.clear_color_bits[index] = clear_color_bits[index];
    }
  } else {
    // XD3D_Resolve loads the same float into all four lanes.
    std::fill(std::begin(command.clear_color_bits), std::end(command.clear_color_bits),
              LoadU32(base, kRetailNullClearColorGlobal));
  }
  command.clear_depth_bits = std::bit_cast<uint64_t>(clear_depth);
  command.clear_stencil = clear_stencil;
  command.parameters_valid = parameters != 0;
  if (parameters) {
    command.color_format = LoadU32(base, parameters);
    command.color_exp_bias = int32_t(LoadU32(base, parameters + 4));
    command.depth_format = LoadU32(base, parameters + 8);
  }
  SubmitNativeCommand(command);
  RetireNativeBoundResource(base, device, destination_texture);
}

ResolveRectangle LoadResolveRectangle(uint8_t* base, uint32_t address) {
  return {int32_t(LoadU32(base, address)), int32_t(LoadU32(base, address + 4)),
          int32_t(LoadU32(base, address + 8)), int32_t(LoadU32(base, address + 12))};
}

ResolvePoint LoadResolvePoint(uint8_t* base, uint32_t address) {
  return {int32_t(LoadU32(base, address)), int32_t(LoadU32(base, address + 4))};
}

void FinalizeNativeResolveBatchState(uint8_t* base, uint32_t device) {
  uint8_t control_flags = LoadU8(base, device + DeviceOffset(10940)) & uint8_t(0xDF);
  StoreU8(base, device + DeviceOffset(10943), LoadU8(base, device + DeviceOffset(10943)) & uint8_t(0xCF));

  bool render_targets_compatible = false;
  if (!(control_flags & uint8_t(0x0C)) && !LoadU8(base, device + DeviceOffset(12179))) {
    if (control_flags & uint8_t(0x10)) {
      render_targets_compatible = true;
    } else if (control_flags & uint8_t(0x20)) {
      render_targets_compatible = true;
      for (uint32_t index = 0; index < 5; ++index) {
        const uint32_t current = LoadU32(base, device + DeviceOffset(12432) + index * sizeof(uint32_t));
        const uint32_t cached = LoadU32(base, device + DeviceOffset(12720) + index * sizeof(uint32_t));
        if (current && current != cached) {
          render_targets_compatible = false;
          break;
        }
      }
    }
  }

  control_flags =
      uint8_t((control_flags & uint8_t(0xFE)) | uint8_t(render_targets_compatible ? 1 : 0));
  StoreU8(base, device + DeviceOffset(10940), control_flags);
  StoreU32(base, device + DeviceOffset(12716), 0);
  StoreU32(base, device + DeviceOffset(12708), 0);
  StoreU32(base, device + DeviceOffset(12704), 0);
  StoreU32(base, device + DeviceOffset(10932), 0);
  StoreU32(base, device + DeviceOffset(10936), 0);
  StoreU32(base, device + DeviceOffset(12700), UINT32_MAX);
}

#ifdef XD3D_BeginTiling
PPC_FUNC(XD3D_BeginTiling) {
  const uint32_t rectangles = ctx.r6.u32;
  const uint32_t count = ctx.r5.u32;
  if (!IsNativeMode() || count < 2 || !rectangles) {
    XD3D_IMP(BeginTiling)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  uint32_t width = 0;
  uint32_t height = 0;
  for (uint32_t index = 0; index < count; ++index) {
    width = std::max(width, LoadU32(base, rectangles + index * 16 + 8));
    height = std::max(height, LoadU32(base, rectangles + index * 16 + 12));
  }

  // Bound before BeginTiling, so the renderer saw them at their EDRAM size: bind them again.
  for (uint32_t index = 0; index < kRenderTargetCount; ++index) {
    const uint32_t surface =
        LoadU32(base, device + kRenderTargetOffset + index * sizeof(uint32_t));
    if (GrowSurfaceToTilingArea(base, surface, width, height)) {
      InvokeGuest(ctx, base, XD3D_SetRenderTarget, device, index, surface);
    }
  }
  const uint32_t depth = LoadU32(base, device + kDepthStencilOffset);
  if (GrowSurfaceToTilingArea(base, depth, width, height)) {
    InvokeGuest(ctx, base, XD3D_SetDepthStencilSurface, device, depth);
  }

  const std::array<uint32_t, 4> saved_rectangle = {
      LoadU32(base, rectangles), LoadU32(base, rectangles + 4), LoadU32(base, rectangles + 8),
      LoadU32(base, rectangles + 12)};
  StoreU32(base, rectangles, 0);
  StoreU32(base, rectangles + 4, 0);
  StoreU32(base, rectangles + 8, width);
  StoreU32(base, rectangles + 12, height);
  ctx.r5.u32 = 1;
  g_collapsed_tiling = {true,
                        width,
                        height,
                        0,
                        saved_rectangle[2] - saved_rectangle[0],
                        saved_rectangle[3] - saved_rectangle[1]};

  XD3D_IMP(BeginTiling)(ctx, base);
  StoreU32(base, rectangles, saved_rectangle[0]);
  StoreU32(base, rectangles + 4, saved_rectangle[1]);
  StoreU32(base, rectangles + 8, saved_rectangle[2]);
  StoreU32(base, rectangles + 12, saved_rectangle[3]);
}
#endif

// D3D SetPredication(device, mask).
#ifdef XD3D_SetPredication
PPC_FUNC(XD3D_SetPredication) {
  g_collapsed_tiling.predication = ctx.r4.u32;
  XD3D_IMP(SetPredication)(ctx, base);
}
#endif

void SubmitNativeResolveCall(PPCContext& ctx, uint8_t* base);

PPC_FUNC(XD3D_Resolve) {
  if (!IsNativeMode()) {
    XD3D_IMP(Resolve)(ctx, base);
    return;
  }
  SubmitNativeResolveCall(ctx, base);
  if (kObserve) {
    XD3D_IMP(Resolve)(ctx, base);
  }
}

// XD3D_Resolve(device, flags, source rectangle, destination, destination point, level,
// slice, clear color, f1 = clear depth; clear stencil and parameters on the stack).
void SubmitNativeResolveCall(PPCContext& ctx, uint8_t* base) {
  const bool collapsed_tile =
      g_collapsed_tiling.active && g_collapsed_tiling.predication != 0;
  if (collapsed_tile && std::countr_zero(g_collapsed_tiling.predication) >= 2) {
    return;
  }

  static std::atomic<uint64_t> call_count{0};
  const uint64_t call = NextNativeHookDiagnosticCall(call_count);
  if (ShouldLogNativeHookCall(call)) {
    REXLOG_INFO(
        "xnative-hook: Resolve #{} lr={:08X} device={:08X} flags={:08X} "
        "source-rect={:08X} destination={:08X}",
        call, ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32);
  }

  ResolveRectangle source_rectangle{};
  ResolvePoint destination_point{};
  uint32_t clear_color_bits[4]{};
  const ResolveRectangle* source_rectangle_pointer = nullptr;
  const ResolvePoint* destination_point_pointer = nullptr;
  const uint32_t* clear_color_pointer = nullptr;
  if (ctx.r5.u32) {
    source_rectangle = LoadResolveRectangle(base, ctx.r5.u32);
    source_rectangle_pointer = &source_rectangle;
  }
  if (ctx.r7.u32) {
    destination_point = LoadResolvePoint(base, ctx.r7.u32);
    destination_point_pointer = &destination_point;
  }
  // A depth resolve that also clears leaves the clear in the EDRAM, not what it copied out.
  if (collapsed_tile && (ctx.r4.u32 & 7) == 4 && ctx.r6.u32 && !(ctx.r4.u32 & 0x200u)) {
    g_pending_depth_handoff = {LoadU32(base, ctx.r3.u32 + kDepthStencilOffset), ctx.r6.u32, false};
  }
  if (collapsed_tile) {
    source_rectangle = {0, 0, int32_t(g_collapsed_tiling.width), int32_t(g_collapsed_tiling.height)};
    source_rectangle_pointer = &source_rectangle;
    destination_point = {0, 0};
    destination_point_pointer = &destination_point;
  }
  // XDK 2.0.20500: (device, flags, rect, texture, point, slice, clear color, f1 depth, stencil on
  // the stack +84): no level and no parameters.
#if XD3D_XDK_20500
  const uint32_t clear_color = ctx.r9.u32;
  const uint32_t destination_level = 0;
  const uint32_t destination_slice = ctx.r8.u32;
  const uint32_t clear_stencil = LoadU32(base, ctx.r1.u32 + 84);
  const uint32_t parameters = 0;
#else
  const uint32_t clear_color = ctx.r10.u32;
  const uint32_t destination_level = ctx.r8.u32;
  const uint32_t destination_slice = ctx.r9.u32;
  const uint32_t clear_stencil = LoadU32(base, ctx.r1.u32 + 92);
  const uint32_t parameters = LoadU32(base, ctx.r1.u32 + 100);
#endif
  if (clear_color) {
    for (uint32_t index = 0; index < 4; ++index) {
      clear_color_bits[index] = LoadU32(base, clear_color + index * sizeof(uint32_t));
    }
    clear_color_pointer = clear_color_bits;
  }

  const uint32_t known_frontbuffer = g_last_present_frontbuffer.load(std::memory_order_relaxed);
  if (rex::diagnostics::IsEnabled(rex::diagnostics::Category::kGuestHooks) && known_frontbuffer &&
      ctx.r6.u32 == known_frontbuffer) {
    std::array<uint32_t, kRenderTargetCount> render_targets{};
    for (uint32_t index = 0; index < kRenderTargetCount; ++index) {
      render_targets[index] =
          LoadU32(base, ctx.r3.u32 + kRenderTargetOffset + index * sizeof(uint32_t));
    }
    std::fprintf(stderr,
                 "[GuestResolveTrace] origin=direct caller=%08X device=%08X flags=%08X "
                 "destination=%08X rt=%08X,%08X,%08X,%08X depth=%08X\n",
                 uint32_t(ctx.lr), ctx.r3.u32, ctx.r4.u32, ctx.r6.u32, render_targets[0],
                 render_targets[1], render_targets[2], render_targets[3],
                 LoadU32(base, ctx.r3.u32 + kDepthStencilOffset));
    std::fflush(stderr);
  }

  SubmitNativeResolve(base, ctx.r3.u32, ctx.r4.u32, source_rectangle_pointer, ctx.r6.u32,
                      destination_point_pointer, destination_level, destination_slice,
                      clear_color_pointer, ctx.f1.f64, clear_stencil, parameters, 1,
                      uint32_t(ctx.lr), 0);
}

#ifdef XD3D_EndTiling
PPC_FUNC(XD3D_EndTiling) {
  if (!IsNativeMode()) {
    XD3D_IMP(EndTiling)(ctx, base);
    return;
  }
  g_collapsed_tiling = {};
  g_pending_depth_handoff.armed = g_pending_depth_handoff.source != 0;

  const uint32_t device = ctx.r3.u32;
  if (kObserve && !device) {
    XD3D_IMP(EndTiling)(ctx, base);
    return;
  }
  const uint32_t base_flags = ctx.r4.u32;
  uint32_t resolve_records = ctx.r5.u32;
  const uint32_t destination_texture = ctx.r6.u32;
  const uint32_t final_clear_color = ctx.r7.u32;
  const double final_clear_depth = ctx.f1.f64;
  const uint32_t final_clear_stencil = ctx.r9.u32;
  const uint32_t final_parameters = ctx.r10.u32;
  const uint32_t resolve_count = device ? LoadU32(base, device + DeviceOffset(12740)) : 0;

  if (!device) {
    ctx.r3.u32 = 0;
    return;
  }

  static std::atomic<uint64_t> call_count{0};
  const uint64_t call = NextNativeHookDiagnosticCall(call_count);
  if (ShouldLogNativeHookCall(call)) {
    REXLOG_INFO(
        "xnative-hook: ResolveBatch #{} lr={:08X} device={:08X} flags={:08X} "
        "records={:08X} count={} destination={:08X}",
        call, ctx.lr, device, base_flags, resolve_records, resolve_count, destination_texture);
  }

  if (!resolve_records && destination_texture) {
    resolve_records = device + DeviceOffset(12744);
  }

  const uint32_t source_surface = device ? LoadU32(base, device + DeviceOffset(12716)) : 0;
  const uint32_t known_frontbuffer = g_last_present_frontbuffer.load(std::memory_order_relaxed);
  if (rex::diagnostics::IsEnabled(rex::diagnostics::Category::kGuestHooks) && known_frontbuffer &&
      destination_texture == known_frontbuffer) {
    std::fprintf(stderr,
                 "[GuestResolveTrace] origin=batch caller=%08X device=%08X flags=%08X "
                 "destination=%08X source=%08X records=%08X count=%u\n",
                 uint32_t(ctx.lr), device, base_flags, destination_texture, source_surface,
                 resolve_records, resolve_count);
    std::fflush(stderr);
  }
  if (destination_texture && resolve_count && source_surface) {
    const uint32_t packed_dimensions = LoadU32(base, source_surface + 36);
    const uint32_t surface_width = (std::rotl(packed_dimensions, 14) & 0x3FFF) + 1;
    const uint32_t surface_height = (std::rotl(packed_dimensions, 29) & 0x7FFF) + 1;

    for (uint32_t index = 0; index < resolve_count; ++index) {
      const uint32_t record = resolve_records + index * 16;
      ResolveRectangle source_rectangle = LoadResolveRectangle(base, record);
      const ResolvePoint destination_point = LoadResolvePoint(base, record);
      if (uint32_t(source_rectangle.right) == surface_width) {
        source_rectangle.right = int32_t((uint32_t(source_rectangle.right) + 7) & ~uint32_t(7));
      }
      if (uint32_t(source_rectangle.bottom) == surface_height) {
        source_rectangle.bottom = int32_t((uint32_t(source_rectangle.bottom) + 7) & ~uint32_t(7));
      }

      uint32_t flags = base_flags;
      uint32_t clear_color_bits[4]{};
      const uint32_t* clear_color_pointer = nullptr;
      double clear_depth = final_clear_depth;
      uint32_t clear_stencil = final_clear_stencil;
      uint32_t parameters = final_parameters;
      if (index + 1 < resolve_count) {
        if (LoadU32(base, device + DeviceOffset(12432))) {
          flags |= 0x100;
        }
        if (LoadU32(base, device + DeviceOffset(12448))) {
          flags |= 0x200;
        }
        for (uint32_t color_index = 0; color_index < 4; ++color_index) {
          clear_color_bits[color_index] =
              LoadU32(base, device + DeviceOffset(13184) + color_index * sizeof(uint32_t));
        }
        clear_color_pointer = clear_color_bits;
        clear_depth = double(std::bit_cast<float>(LoadU32(base, device + DeviceOffset(13200))));
        clear_stencil = LoadU32(base, device + DeviceOffset(13204));
        parameters = 0;
      } else if (final_clear_color) {
        for (uint32_t color_index = 0; color_index < 4; ++color_index) {
          clear_color_bits[color_index] =
              LoadU32(base, final_clear_color + color_index * sizeof(uint32_t));
        }
        clear_color_pointer = clear_color_bits;
      }

      SubmitNativeResolve(base, device, flags, &source_rectangle, destination_texture,
                          &destination_point, 0, 0, clear_color_pointer, clear_depth, clear_stencil,
                          parameters, 2, uint32_t(ctx.lr), 0);
    }
  } else if (destination_texture && resolve_count) {
    REXLOG_ERROR(
        "xnative-hook: ResolveBatch #{} missing source surface device={:08X} count={} "
        "destination={:08X}",
        call, device, resolve_count, destination_texture);
  }

  if (kObserve) {
    // The game's function restores what tiling changed (and puts its resolves in the ring buffer).
    XD3D_IMP(EndTiling)(ctx, base);
    return;
  }
  InvokeGuest(ctx, base, XD3D_TilingRestoreState, device, 2);
  FinalizeNativeResolveBatchState(base, device);
  InvokeGuest(ctx, base, XD3D_TilingRestoreConstants, device, XD3D_ADDR_TilingRestoreArgument);
  ctx.r3.u32 = 0;
}
#endif

PPC_FUNC(XD3D_DrawIndexedVertices) {
  if (!IsNativeMode()) {
    XD3D_IMP(DrawIndexedVertices)(ctx, base);
    return;
  }

  static std::atomic<uint64_t> call_count{0};
  const uint64_t call = NextNativeHookDiagnosticCall(call_count);
  if (ShouldLogNativeHookCall(call)) {
    REXLOG_INFO(
        "xnative-hook: DrawIndexedPrimitive #{} lr={:08X} device={:08X} primitive={} "
        "base={} start={} indices={}",
        call, ctx.lr, ctx.r3.u32, ctx.r4.u32, ctx.r5.s32, ctx.r6.u32, ctx.r7.u32);
  }

  const uint32_t device = ctx.r3.u32;
  DrawIndexedPrimitiveCommand command{};
  command.device = device;
  command.primitive_type = ctx.r4.u32;
#if XD3D_XDK_20500
  command.base_vertex = 0;
  command.start_index = ctx.r5.u32;
  command.index_count = ctx.r6.u32;
#else
  command.base_vertex = ctx.r5.s32;
  command.start_index = ctx.r6.u32;
  command.index_count = ctx.r7.u32;
#endif
  command.primitive_restart_enabled = (LoadU32(base, device + DeviceOffset(10568)) & uint32_t(1u << 21)) != 0;
  command.primitive_restart_index = LoadU32(base, device + DeviceOffset(10444)) & 0x00FFFFFF;
  command.caller = static_cast<uint32_t>(ctx.lr);
  command.origin_flags = kDrawCommandOriginDirect;
  command.command_list = 0;
  command.draw_id = call;
  NoteDrawChain(ctx, base, "indexed");

  SyncNativeVertexDeclaration(base, device);
  command.dirty_state = TakeNativeDrawDirtyState(base, device);
  SubmitNativeCommand(command);
  if (kObserve) {
    XD3D_IMP(DrawIndexedVertices)(ctx, base);
  }
}

bool SubmitNativeClear(PPCContext& ctx, uint8_t* base);

PPC_FUNC(XD3D_ClearRectangle) {
  if (!IsNativeMode()) {
    XD3D_IMP(ClearRectangle)(ctx, base);
    return;
  }
  const uint32_t device = ctx.r3.u32;
  if (!SubmitNativeClear(ctx, base)) {
    if (kObserve) {
      XD3D_IMP(ClearRectangle)(ctx, base);
    }
    return;
  }
  if (kObserve) {
    XD3D_IMP(ClearRectangle)(ctx, base);
    return;
  }
  StoreU8(base, device + DeviceOffset(10941), LoadU8(base, device + DeviceOffset(10941)) | uint8_t(4));
  MarkDirty(base, device, 2, 0x00020000);
}

// XD3D_ClearRectangle(device, flags, rectangle, color, f1 = depth, stencil): false when it
// clears nothing.
bool SubmitNativeClear(PPCContext& ctx, uint8_t* base) {
  const uint32_t device = ctx.r3.u32;
  uint32_t flags = ctx.r4.u32;
  const uint32_t rectangle = ctx.r5.u32;
  const uint32_t color = ctx.r6.u32;
  if (!device || !rectangle) {
    return false;
  }

  if (!LoadU32(base, device + DeviceOffset(12448))) {
    flags &= ~uint32_t(0x30);
  }
  if (!flags) {
    return false;
  }

  const auto viewport_coordinate = [&](uint32_t reference) {
    return TruncateViewportCoordinate(std::bit_cast<float>(
        NativeViewportCoordinateBits(LoadU32(base, device + DeviceOffset(reference)))));
  };
  const int32_t viewport_x = viewport_coordinate(12640);
  const int32_t viewport_y = viewport_coordinate(12644);
  const int32_t viewport_width = viewport_coordinate(12648);
  const int32_t viewport_height = viewport_coordinate(12652);

  int32_t left = std::max(int32_t(LoadU32(base, rectangle)), viewport_x);
  int32_t top = std::max(int32_t(LoadU32(base, rectangle + 4)), viewport_y);
  int32_t right = std::min(int32_t(LoadU32(base, rectangle + 8)),
                           std::bit_cast<int32_t>(uint32_t(viewport_x) + uint32_t(viewport_width)));
  int32_t bottom =
      std::min(int32_t(LoadU32(base, rectangle + 12)),
               std::bit_cast<int32_t>(uint32_t(viewport_y) + uint32_t(viewport_height)));
  if (int32_t(LoadU32(base, device + DeviceOffset(11848)))) {
    left = std::max(left, int32_t(LoadU32(base, device + DeviceOffset(12668))));
    top = std::max(top, int32_t(LoadU32(base, device + DeviceOffset(12672))));
    right = std::min(right, int32_t(LoadU32(base, device + DeviceOffset(12676))));
    bottom = std::min(bottom, int32_t(LoadU32(base, device + DeviceOffset(12680))));
  }
  if (right <= left || bottom <= top) {
    return false;
  }

  ClearCommand command{};
  command.device = device;
  command.flags = flags;
  command.left = left;
  command.top = top;
  command.right = right;
  command.bottom = bottom;
  if (color) {
    for (uint32_t index = 0; index < 4; ++index) {
      command.color_bits[index] = LoadU32(base, color + index * sizeof(uint32_t));
    }
  } else {
    // generated code loads the null clear color global into every lane; retail is +0.0.
    std::fill(std::begin(command.color_bits), std::end(command.color_bits),
              LoadU32(base, kRetailNullClearColorGlobal));
  }
  command.depth_bits = std::bit_cast<uint64_t>(ctx.f1.f64);
  // XDK 2.0.20500's ClearRectangle takes no stencil (device, flags, rect, color, f1 depth).
  command.stencil = XD3D_XDK_20500 ? 0 : ctx.r8.u32;

  command.dirty_state = TakeNativeDrawDirtyState(base, device);
  SubmitNativeCommand(command);
  return true;
}

#ifdef XD3D_SetPresentBehaviorBit
PPC_FUNC(XD3D_SetPresentBehaviorBit) {
  if (kObserve || !IsNativeMode()) {
    XD3D_IMP(SetPresentBehaviorBit)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
  uint32_t behavior = LoadU32(base, device + DeviceOffset(22280));
  if (ctx.r4.u32) {
    behavior |= 4;
  } else {
    behavior &= ~uint32_t(4);
  }
  StoreU32(base, device + DeviceOffset(22280), behavior);
}
#endif

PPC_FUNC(XD3D_Swap) {
  NameUnnamedCurrentThread("XpRenderThread");
  if (!IsNativeMode()) {
    XD3D_IMP(Swap)(ctx, base);
    return;
  }

  const uint32_t device = ctx.r3.u32;
#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
  SubmitReadyShaderRegistrations();
#endif
  const uint32_t submitted_frame = LoadU32(base, device + kSubmittedFrameOffset) + 1;
  if (!kObserve) {
    StoreU32(base, device + kSubmittedFrameOffset, submitted_frame);
    StoreU8(base, device + DeviceOffset(10941), LoadU8(base, device + DeviceOffset(10941)) & uint8_t(0xEF));
    StoreAllDirty(base, device);
  }

  PresentCommand command{};
  command.device = device;
  command.frontbuffer_texture = ctx.r4.u32;
  if (command.frontbuffer_texture) {
    for (uint32_t index = 0; index < std::size(command.frontbuffer_fetch); ++index) {
      command.frontbuffer_fetch[index] =
          LoadU32(base, command.frontbuffer_texture + 28 + index * sizeof(uint32_t));
    }
  }
  command.diagnostic_guest_caller = ctx.lr;
  command.diagnostic_origin = 0;
  command.diagnostic_persisted_texture = device + kPersistedPresentTextureOffset;
  for (uint32_t index = 0; index < std::size(command.diagnostic_persisted_fetch); ++index) {
    command.diagnostic_persisted_fetch[index] =
        LoadU32(base, command.diagnostic_persisted_texture + 28 + index * sizeof(uint32_t));
  }
  if (command.frontbuffer_texture) {
    command.diagnostic_requested_descriptor_hash =
        XXH3_64bits(GuestPointer(base, command.frontbuffer_texture), kPresentDescriptorSize);
  }
  command.diagnostic_persisted_descriptor_hash =
      XXH3_64bits(GuestPointer(base, command.diagnostic_persisted_texture), kPresentDescriptorSize);
  command.diagnostic_descriptor_equal =
      command.frontbuffer_texture != 0 &&
      std::memcmp(GuestPointer(base, command.frontbuffer_texture),
                  GuestPointer(base, command.diagnostic_persisted_texture),
                  kPresentDescriptorSize) == 0;
  command.diagnostic_device_flag_10941 = LoadU8(base, device + kPresentFlag10941Offset);
  command.diagnostic_device_flag_10942 = LoadU8(base, device + kPresentFlag10942Offset);
  command.diagnostic_force_content_probe = command.diagnostic_trace;
  command.submitted_frame = submitted_frame;
  g_last_present_frontbuffer.store(command.frontbuffer_texture, std::memory_order_relaxed);
  SubmitNativeCommand(command);
  if (kObserve) {
    // The game's Swap: its frame counting and the swap the fake GPU signals.
    XD3D_IMP(Swap)(ctx, base);
    return;
  }
  ctx.r3.u32 = device;
}

