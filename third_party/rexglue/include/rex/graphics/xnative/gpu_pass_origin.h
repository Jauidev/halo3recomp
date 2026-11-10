#pragma once
#include <compare>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <span>
#include <type_traits>

namespace rex::graphics::xnative {
// Which of the title's render passes a command came from, for the profiler only: a title's hooks
// may wrap a command in an envelope (PackGpuPassEnvelope) that carries it. pass_id is the title's
// own numbering; the renderer only groups by it.
enum class GpuPassOriginSource : uint32_t { kNone, kListHeader, kExecutionRecord, kVerifiedBoth, kUnresolved, kMismatch, kMixed };
struct GpuPassOrigin {
  uint64_t scope = 0;
  uint32_t pass_id = UINT32_MAX;
  GpuPassOriginSource source = GpuPassOriginSource::kNone;
  uint32_t arena = 0, offset = 0, ordinal = 0, list_id = 0;
  auto operator<=>(const GpuPassOrigin&) const = default;
  bool attributed() const {
    return source==GpuPassOriginSource::kListHeader || source==GpuPassOriginSource::kExecutionRecord ||
           source==GpuPassOriginSource::kVerifiedBoth;
  }
};
static_assert(std::is_trivially_copyable_v<GpuPassOrigin> && sizeof(GpuPassOrigin)==32);
inline constexpr uint32_t kGpuPassEnvelopeAbi = 0x47505031;
inline constexpr uint32_t kGpuPassEnvelopeVersion = 1;
inline const char* GpuPassOriginSourceName(GpuPassOriginSource source) {
  switch(source){
    case GpuPassOriginSource::kNone:return "untagged";
    case GpuPassOriginSource::kListHeader:return "list-header";
    case GpuPassOriginSource::kExecutionRecord:return "task-record";
    case GpuPassOriginSource::kVerifiedBoth:return "header-and-task-record";
    case GpuPassOriginSource::kUnresolved:return "unresolved-list";
    case GpuPassOriginSource::kMismatch:return "conflicting-list-record";
    case GpuPassOriginSource::kMixed:return "mixed-list-origins";
  }return "invalid";
}

struct GpuPassEnvelopeHeader {
  uint32_t magic=kGpuPassEnvelopeAbi, version=kGpuPassEnvelopeVersion, inner_abi=0, payload_size=0;
  GpuPassOrigin origin{};
};
static_assert(sizeof(GpuPassEnvelopeHeader)==48);
inline size_t PackGpuPassEnvelope(std::span<uint8_t> storage,const void* command,size_t size,
                                  uint32_t abi,const GpuPassOrigin& origin) {
  if(!command||!size||size>UINT32_MAX||abi==kGpuPassEnvelopeAbi ||
     storage.size()<sizeof(GpuPassEnvelopeHeader)||size>storage.size()-sizeof(GpuPassEnvelopeHeader))return 0;
  const GpuPassEnvelopeHeader header{kGpuPassEnvelopeAbi,kGpuPassEnvelopeVersion,abi,uint32_t(size),origin};
  std::memcpy(storage.data(),&header,sizeof(header));
  std::memcpy(storage.data()+sizeof(header),command,size);
  return sizeof(header)+size;
}
inline bool UnpackGpuPassEnvelope(const void*& command,size_t& size,uint32_t& abi,GpuPassOrigin& origin) {
  if(!command||size<sizeof(GpuPassEnvelopeHeader))return false;
  GpuPassEnvelopeHeader h;std::memcpy(&h,command,sizeof(h));
  if(h.magic!=kGpuPassEnvelopeAbi||h.version!=kGpuPassEnvelopeVersion||!h.payload_size||
     h.payload_size!=size-sizeof(h)||h.inner_abi==kGpuPassEnvelopeAbi||
     uint32_t(h.origin.source)>uint32_t(GpuPassOriginSource::kMismatch))return false;
  abi=h.inner_abi;origin=h.origin;command=static_cast<const uint8_t*>(command)+sizeof(h);size=h.payload_size;return true;
}
} // namespace rex::graphics::xnative
