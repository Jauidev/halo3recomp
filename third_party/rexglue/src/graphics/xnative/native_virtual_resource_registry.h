#pragma once

#include <cstdint>
#include <optional>
#include <unordered_map>
#include <vector>

#include <rex/graphics/xnative/title_commands.h>

namespace rex::graphics::xnative {

struct NativeVirtualResourceRecord {
  uint32_t resource = 0;
  VirtualResourceKind kind = VirtualResourceKind::kTexture;
  uint32_t wrapper = 0;
  uint32_t companion = 0;
  uint32_t guest_backing_width = 0;
  uint32_t guest_backing_height = 0;
  uint32_t logical_width = 0;
  uint32_t logical_height = 0;
  uint32_t physical_width = 0;
  uint32_t physical_height = 0;
  // the registered physical size; physical_* is it times the primary scene render
  // scale (dynamic resolution) for kPrimaryScene resources.
  uint32_t base_physical_width = 0;
  uint32_t base_physical_height = 0;
  VirtualResourceScaleDomain scale_domain = VirtualResourceScaleDomain::kLogical;
  uint32_t constructor_caller = 0;
  uint32_t packed_depth_source = 0;
  uint64_t lifetime = 0;
  bool guest_write_conflict = false;

  bool HasSameIdentity(const RegisterVirtualResourceCommand& command) const {
    return resource == command.resource && kind == command.kind && wrapper == command.wrapper &&
           companion == command.companion &&
           guest_backing_width == command.guest_backing_width &&
           guest_backing_height == command.guest_backing_height &&
           logical_width == command.logical_width && logical_height == command.logical_height &&
           base_physical_width == command.physical_width &&
           base_physical_height == command.physical_height &&
           scale_domain == command.scale_domain &&
           constructor_caller == command.constructor_caller &&
           packed_depth_source == command.packed_depth_source;
  }
};

// a primary scene extent at a render scale: even, at least 2 (as the supersampled
// extents are aligned).
inline uint32_t ScalePrimarySceneExtent(uint32_t base, double scale) {
  if (scale >= 0.999) return base;
  uint32_t scaled = uint32_t(double(base) * scale + 0.5);
  scaled = (scaled + 1u) & ~1u;
  return scaled < 2u ? 2u : scaled;
}

enum class VirtualResourceRegistrationResult : uint8_t {
  kCreated,
  kUnchanged,
  kReplaced,
};

class NativeVirtualResourceRegistry {
 public:
  VirtualResourceRegistrationResult Register(const RegisterVirtualResourceCommand& command) {
    auto existing = records_.find(command.resource);
    if (existing != records_.end() && existing->second.HasSameIdentity(command)) {
      return VirtualResourceRegistrationResult::kUnchanged;
    }

    NativeVirtualResourceRecord record;
    record.resource = command.resource;
    record.kind = command.kind;
    record.wrapper = command.wrapper;
    record.companion = command.companion;
    record.guest_backing_width = command.guest_backing_width;
    record.guest_backing_height = command.guest_backing_height;
    record.logical_width = command.logical_width;
    record.logical_height = command.logical_height;
    record.base_physical_width = command.physical_width;
    record.base_physical_height = command.physical_height;
    const double scale =
        command.scale_domain == VirtualResourceScaleDomain::kPrimaryScene ? primary_scene_scale_
                                                                          : 1.0;
    record.physical_width = ScalePrimarySceneExtent(command.physical_width, scale);
    record.physical_height = ScalePrimarySceneExtent(command.physical_height, scale);
    record.scale_domain = command.scale_domain;
    record.constructor_caller = command.constructor_caller;
    record.packed_depth_source = command.packed_depth_source;
    record.lifetime = next_lifetime_++;

    const bool replaced = existing != records_.end();
    records_.insert_or_assign(command.resource, record);
    return replaced ? VirtualResourceRegistrationResult::kReplaced
                    : VirtualResourceRegistrationResult::kCreated;
  }

  const NativeVirtualResourceRecord* Find(uint32_t resource) const {
    const auto existing = records_.find(resource);
    return existing == records_.end() ? nullptr : &existing->second;
  }

  bool MarkGuestWrite(uint32_t resource) {
    const auto existing = records_.find(resource);
    if (existing == records_.end()) {
      return false;
    }
    existing->second.guest_write_conflict = true;
    return true;
  }

  bool PublishHostWrite(uint32_t resource) {
    const auto existing = records_.find(resource);
    if (existing == records_.end()) {
      return false;
    }
    existing->second.guest_write_conflict = false;
    return true;
  }

  bool Erase(uint32_t resource) { return records_.erase(resource) != 0; }

  void Clear() { records_.clear(); }

  // (dynamic resolution) the physical size of every primary scene resource becomes its
  // registered size times scale, for those registered from now on too. Appends the resources whose
  // size changed (their host images must be created again) and gives each a new lifetime.
  void SetPrimarySceneScale(double scale, std::vector<uint32_t>& changed) {
    primary_scene_scale_ = scale;
    for (auto& [resource, record] : records_) {
      if (record.scale_domain != VirtualResourceScaleDomain::kPrimaryScene) continue;
      const uint32_t width = ScalePrimarySceneExtent(record.base_physical_width, scale);
      const uint32_t height = ScalePrimarySceneExtent(record.base_physical_height, scale);
      if (width == record.physical_width && height == record.physical_height) continue;
      record.physical_width = width;
      record.physical_height = height;
      record.lifetime = next_lifetime_++;
      changed.push_back(resource);
    }
  }

  double primary_scene_scale() const { return primary_scene_scale_; }

 private:
  std::unordered_map<uint32_t, NativeVirtualResourceRecord> records_;
  uint64_t next_lifetime_ = 1;
  double primary_scene_scale_ = 1.0;
};

enum class ResourceUnlockDecision : uint8_t {
  kIgnoreNoDirtyUpdate,
  kDirtyGuestResource,
  kInvalidateVirtualHostOwnership,
};

enum class VirtualTextureCaptureAction : uint8_t {
  kUseHostGeneration,
  kCreateDeterministicHostGeneration,
  kCaptureCompleteGuestBacking,
  kRejectDescriptorMismatch,
  kRejectIncompleteGuestBacking,
};

// Virtual render targets have two legitimate ownership modes. A render-to-
// texture generation is host-owned and must never be reconstructed from its
// tiny placeholder allocation. A concretely backed, CPU-lockable texture may
// become guest-owned after an explicit guest write. Keep that distinction in
// one policy so capture cannot accidentally expand a 1x1 placeholder into a
// logical-resolution guest snapshot.
constexpr VirtualTextureCaptureAction ClassifyVirtualTextureCapture(
    bool shape_matches, bool host_generation_matches, bool guest_write_conflict,
    bool guest_backing_complete) {
  if (!shape_matches) {
    return VirtualTextureCaptureAction::kRejectDescriptorMismatch;
  }
  if (host_generation_matches) {
    return VirtualTextureCaptureAction::kUseHostGeneration;
  }
  if (!guest_write_conflict) {
    return VirtualTextureCaptureAction::kCreateDeterministicHostGeneration;
  }
  return guest_backing_complete
             ? VirtualTextureCaptureAction::kCaptureCompleteGuestBacking
             : VirtualTextureCaptureAction::kRejectIncompleteGuestBacking;
}

inline ResourceUnlockDecision ClassifyResourceUnlock(bool virtual_resource,
                                                     ResourceUnlockAccess access) {
  if (access == ResourceUnlockAccess::kNoDirtyUpdate) {
    return ResourceUnlockDecision::kIgnoreNoDirtyUpdate;
  }
  return virtual_resource ? ResourceUnlockDecision::kInvalidateVirtualHostOwnership
                          : ResourceUnlockDecision::kDirtyGuestResource;
}

}  // namespace rex::graphics::xnative
