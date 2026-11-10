#include <atomic>
#include <cstdarg>
#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <memory>
#include <mutex>
#include <new>
#include <span>
#include <unordered_map>
#include <vector>
#include <utility>

#include <rex/hook.h>
#include <rex/logging.h>
#include <rex/system/lzx.h>

#include "xd3d_init.h"

#ifdef __ANDROID__
#include <android/log.h>
#endif

namespace xport::lzx {
namespace {

// All offsets and limits below come directly from generated XD3D_LzxDecodeFrame,
// XD3D_LzxDecodeInternal, XD3D_LzxCreateDecoder, and the block parser in XD3D_LzxParseBlock.
constexpr uint32_t kEmbeddedDecoderOffset = 0x14;
constexpr uint32_t kInputBeginOffset = 0x2B04;
constexpr uint32_t kInputEndOffset = 0x2B08;
constexpr uint32_t kOutputOffset = 0x2B0C;
constexpr uint32_t kTotalOutputOffset = 0x2B10;
constexpr uint32_t kDecodeCountOffset = 0x2EC4;
constexpr uint32_t kOutputProtectionOffset = 0x2FE4;
constexpr uint32_t kWindowSizeOffset = 0x4;
constexpr uint32_t kRetailWindowSize = 0x20000;
constexpr uint32_t kMaximumFrameSize = 0x8000;
constexpr uint32_t kMaximumCompressedFrameSize = 0xFFFF;
constexpr uint32_t kRetailReadPadding = 0x4;
constexpr uint64_t kGuestAddressSpaceSize = uint64_t{1} << 32;
constexpr uint32_t kDecoderSpan = kOutputProtectionOffset + sizeof(uint8_t);

struct DecoderState {
  explicit DecoderState(uint32_t window_size)
      : decoder(rex::lzx::PersistentDecoder::Create(window_size)),
        input_snapshot(new (std::nothrow)
                           uint8_t[kMaximumCompressedFrameSize + kRetailReadPadding]) {}

  std::mutex mutex;
  std::unique_ptr<rex::lzx::PersistentDecoder> decoder;
  std::unique_ptr<uint8_t[]> input_snapshot;
  bool failed = false;
};

std::mutex g_registry_mutex;
std::unordered_map<uint32_t, std::shared_ptr<DecoderState>> g_registry;

// the native decoder runs unless XPORT_LZX=0 (the recompiled one). Native frames and
// failures are reported (logcat tag xport, stderr on the PC) without the renderer's logger, which
// starts later.
bool NativeEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("XPORT_LZX");
    return !(value && value[0] == '0');
  }();
  return enabled;
}

// XPORT_LZX_DUMP=<path prefix>: every stream's frames since its last reset are kept (input with the
// 4 read-padding bytes, output) and written to <prefix>lzx_<decoder>_<n>.bin when a native decode
// fails, or, with the recompiled decoder, once the stream's total output reaches XPORT_LZX_DUMP_AT
// bytes. Per frame: u32 input size, u32 output size, u32 total before, input, output (little endian).
const char* DumpPrefix() {
  static const char* prefix = [] {
    const char* value = std::getenv("XPORT_LZX_DUMP");
    return value && *value ? value : nullptr;
  }();
  return prefix;
}

struct Trace {
  std::vector<uint8_t> bytes;
  bool dumped = false;
};
std::mutex g_trace_mutex;
std::unordered_map<uint32_t, Trace> g_traces;
constexpr size_t kMaximumTraceBytes = 96u << 20;

void ClearTrace(uint32_t decoder) {
  if (!DumpPrefix()) return;
  std::lock_guard lock(g_trace_mutex);
  g_traces.erase(decoder);
}

void Report(const char* format, ...);

void WriteTrace(uint32_t decoder, Trace& trace) {
  static std::atomic<uint32_t> s_index{0};
  trace.dumped = true;
  const uint32_t index = s_index++;
  if (index >= 24) return;
  char path[512];
  snprintf(path, sizeof(path), "%slzx_%08X_%u.bin", DumpPrefix(), decoder, index);
  if (FILE* file = fopen(path, "wb")) {
    fwrite(trace.bytes.data(), 1, trace.bytes.size(), file);
    fclose(file);
    Report("lzx: stream dumped to %s (%zu bytes)", path, trace.bytes.size());
  } else {
    Report("lzx: cannot write %s", path);
  }
  trace.dumped = true;
}

void RecordFrame(uint32_t decoder, const uint8_t* input, uint32_t input_size,
                 const uint8_t* output, uint32_t output_size, uint32_t total_before, bool dump) {
  if (!DumpPrefix()) return;
  std::lock_guard lock(g_trace_mutex);
  Trace& trace = g_traces[decoder];
  if (trace.dumped || trace.bytes.size() + input_size + output_size + 12 > kMaximumTraceBytes) {
    return;
  }
  auto put = [&trace](uint32_t value) {
    const uint8_t* p = reinterpret_cast<const uint8_t*>(&value);
    trace.bytes.insert(trace.bytes.end(), p, p + 4);
  };
  put(input_size);
  put(output_size);
  put(total_before);
  trace.bytes.insert(trace.bytes.end(), input, input + input_size);
  if (output) {
    trace.bytes.insert(trace.bytes.end(), output, output + output_size);
  } else {
    trace.bytes.resize(trace.bytes.size() + output_size);
  }
  if (dump) WriteTrace(decoder, trace);
}

uint32_t DumpAt() {
  static const uint32_t at = [] {
    const char* value = std::getenv("XPORT_LZX_DUMP_AT");
    return value ? uint32_t(strtoul(value, nullptr, 0)) : 0u;
  }();
  return at;
}

std::atomic<uint64_t> g_native_frames{0};
std::atomic<uint64_t> g_failed_frames{0};

void Report(const char* format, ...) {
  va_list args;
  va_start(args, format);
#ifdef __ANDROID__
  __android_log_vprint(ANDROID_LOG_INFO, "xport", format, args);
#else
  vfprintf(stderr, format, args);
  fputc('\n', stderr);
#endif
  va_end(args);
}

void CountFrame(bool success, uint32_t decoder, uint32_t expected_output,
                uint32_t compressed_size) {
  if (success) {
    const uint64_t frames = g_native_frames.fetch_add(1, std::memory_order_relaxed) + 1;
    if (frames == 1 || frames % 20000 == 0) {
      Report("lzx: %llu frames decoded natively, %llu failed", (unsigned long long)frames,
             (unsigned long long)g_failed_frames.load(std::memory_order_relaxed));
    }
  } else if (g_failed_frames.fetch_add(1, std::memory_order_relaxed) < 16) {
    Report("lzx: native decode FAILED (decoder %08X, output %u, input %u)", decoder,
           expected_output, compressed_size);
  }
}

// Announce at initialization, not in the per-frame decoding path. Native
// decoding is unconditional; comparison with retail lives in the test harness.
void LogBackendOnce() {
  static std::once_flag once;
  std::call_once(once,
                 [] { REXLOG_INFO("xport-lzx: backend=libmspack mode=native-only format=xmem"); });
}

bool IsGuestRange(uint32_t address, uint64_t size) {
  if (size == 0) {
    return true;
  }
  if (address == 0 || size > kGuestAddressSpaceSize) {
    return false;
  }
  const uint64_t end = static_cast<uint64_t>(address) + size;
  return end <= kGuestAddressSpaceSize && !(address < 0xE0000000u && end > 0xE0000000ull);
}

std::shared_ptr<DecoderState> FindEntry(uint32_t decoder_address) {
  std::lock_guard lock(g_registry_mutex);
  const auto found = g_registry.find(decoder_address);
  return found == g_registry.end() ? nullptr : found->second;
}

std::shared_ptr<DecoderState> GetOrCreateEntry(uint32_t decoder_address, uint32_t window_size) {
  {
    std::lock_guard lock(g_registry_mutex);
    const auto found = g_registry.find(decoder_address);
    if (found != g_registry.end()) {
      return found->second;
    }
  }
  // Decoder/window allocation is independent across streams and does not hold
  // the registry lock. Another creator can win publication; use its entry.
  try {
    auto entry = std::make_shared<DecoderState>(window_size);
    if (!entry->decoder || !entry->input_snapshot) {
      return nullptr;
    }
    std::lock_guard lock(g_registry_mutex);
    return g_registry.try_emplace(decoder_address, std::move(entry)).first->second;
  } catch (const std::bad_alloc&) {
    return nullptr;
  }
}

void RemoveEntry(uint32_t decoder_address) {
  std::shared_ptr<DecoderState> retired;
  {
    std::lock_guard lock(g_registry_mutex);
    const auto found = g_registry.find(decoder_address);
    if (found == g_registry.end()) {
      return;
    }
    retired = std::move(found->second);
    g_registry.erase(found);
  }
  // A decoder in use retains its entry; its native storage cannot be freed
  // underneath DecodeFrame. Destruction never holds the global registry lock.
}

void ResetDecoder(PPCContext& ctx, uint8_t* base) {
  const uint32_t decoder_address = ctx.r3.u32;
  ClearTrace(decoder_address);
  const auto entry =
      IsGuestRange(decoder_address, kDecoderSpan) ? FindEntry(decoder_address) : nullptr;
  if (!entry) {
    XD3D_IMP(LzxResetDecoder)(ctx, base);
    return;
  }
  // Reset the guest counters and native dictionary under the same stream lock
  // used by decoding. Neither half of a reset may be observed independently.
  std::lock_guard lock(entry->mutex);
  XD3D_IMP(LzxResetDecoder)(ctx, base);
  entry->failed = !entry->decoder->Reset();
}

void SetRetailDecodeBookkeeping(PPCContext& ctx, uint8_t* base, uint32_t decoder_address,
                                uint32_t input_address, uint32_t compressed_size,
                                uint32_t output_address) {
  REX_STORE_U32(decoder_address + kInputBeginOffset, input_address);
  REX_STORE_U32(decoder_address + kInputEndOffset,
                input_address + compressed_size + kRetailReadPadding);
  REX_STORE_U32(decoder_address + kOutputOffset, output_address);

  rex::CallFrame protection_query(ctx);
  protection_query.ctx.r3.u64 = output_address;
  __imp__MmQueryAddressProtect(protection_query.ctx, base);
  REX_STORE_U8(decoder_address + kOutputProtectionOffset,
               (protection_query.ctx.r3.u32 & 0x600) != 0 ? 1 : 0);
}

void CompleteDecode(PPCContext& ctx, uint8_t* base, uint32_t decoder_address,
                    uint32_t bytes_written_address, bool success, uint32_t bytes_written) {
  const uint32_t count = REX_LOAD_U32(decoder_address + kDecodeCountOffset);
  REX_STORE_U32(decoder_address + kDecodeCountOffset, count + 1);
  REX_STORE_U32(bytes_written_address, success ? bytes_written : 0);
  if (success) {
    const uint32_t total = REX_LOAD_U32(decoder_address + kTotalOutputOffset);
    REX_STORE_U32(decoder_address + kTotalOutputOffset, total + bytes_written);
    ctx.r3.u64 = 0;
  } else {
    ctx.r3.u64 = 1;
  }
}

struct DecodeArguments {
  uint32_t decoder = 0;
  uint32_t expected_output = 0;
  uint32_t input = 0;
  uint32_t compressed_size = 0;
  uint32_t output = 0;
  uint32_t bytes_written = 0;
};

DecodeArguments ReadArguments(const PPCContext& ctx) {
  // r8 redundantly carries the output size at the generated call site, but
  // retail XD3D_LzxDecodeFrame never reads it. Deliberately ignoring r8 is required
  // for ABI fidelity; r4 is the sole decode-size argument.
  return {
      ctx.r3.u32, ctx.r4.u32, ctx.r5.u32, ctx.r6.u32, ctx.r7.u32, ctx.r9.u32,
  };
}

bool ValidateArguments(const DecodeArguments& args) {
  const uint64_t padded_input_size =
      static_cast<uint64_t>(args.compressed_size) + kRetailReadPadding;
  const uint64_t input_end = static_cast<uint64_t>(args.input) + padded_input_size;
  return args.expected_output <= kMaximumFrameSize &&
         args.compressed_size <= kMaximumCompressedFrameSize &&
         IsGuestRange(args.decoder, kDecoderSpan) && input_end < kGuestAddressSpaceSize &&
         IsGuestRange(args.input, padded_input_size) &&
         IsGuestRange(args.output, args.expected_output) &&
         IsGuestRange(args.bytes_written, sizeof(uint32_t));
}

void DecodeFrame(PPCContext& ctx, uint8_t* base, const DecodeArguments& args) {
  if (!ValidateArguments(args)) {
    if (IsGuestRange(args.decoder, kDecoderSpan) &&
        REX_LOAD_U32(args.decoder + kWindowSizeOffset) == kRetailWindowSize) {
      if (const auto entry = GetOrCreateEntry(args.decoder, kRetailWindowSize)) {
        std::lock_guard lock(entry->mutex);
        entry->failed = true;
      }
    }
    if (IsGuestRange(args.bytes_written, sizeof(uint32_t))) {
      REX_STORE_U32(args.bytes_written, 0);
    }
    ctx.r3.u64 = 1;
    CountFrame(false, args.decoder, args.expected_output, args.compressed_size);
    return;
  }

  const auto entry = GetOrCreateEntry(args.decoder, kRetailWindowSize);
  if (!entry) {
    SetRetailDecodeBookkeeping(ctx, base, args.decoder, args.input, args.compressed_size,
                               args.output);
    CompleteDecode(ctx, base, args.decoder, args.bytes_written, false, 0);
    CountFrame(false, args.decoder, args.expected_output, args.compressed_size);
    return;
  }

  std::lock_guard lock(entry->mutex);
  SetRetailDecodeBookkeeping(ctx, base, args.decoder, args.input, args.compressed_size,
                             args.output);
  // Never attach a fresh native dictionary partway into an existing stream.
  // Initialization/reset hooks are the only permitted lifetime transitions.
  if (REX_LOAD_U32(args.decoder + kWindowSizeOffset) != kRetailWindowSize ||
      static_cast<uint32_t>(entry->decoder->total_output_bytes()) !=
          REX_LOAD_U32(args.decoder + kTotalOutputOffset)) {
    if (!entry->failed) {
      Report("lzx: stream state mismatch (decoder %08X window %X, native total %llu, guest total "
             "%u, decode count %u)",
             args.decoder, REX_LOAD_U32(args.decoder + kWindowSizeOffset),
             (unsigned long long)entry->decoder->total_output_bytes(),
             REX_LOAD_U32(args.decoder + kTotalOutputOffset),
             REX_LOAD_U32(args.decoder + kDecodeCountOffset));
    }
    entry->failed = true;
  }
  if (entry->failed) {
    CompleteDecode(ctx, base, args.decoder, args.bytes_written, false, 0);
    CountFrame(false, args.decoder, args.expected_output, args.compressed_size);
    return;
  }
  const size_t readable_input_size = static_cast<size_t>(args.compressed_size) + kRetailReadPadding;
  std::memcpy(entry->input_snapshot.get(), REX_RAW_ADDR(args.input), readable_input_size);
  const uint32_t total_before = uint32_t(entry->decoder->total_output_bytes());
  const auto result = entry->decoder->DecodeFrame(
      std::span<const uint8_t>(entry->input_snapshot.get(), readable_input_size),
      std::span<uint8_t>(REX_RAW_ADDR(args.output), args.expected_output));
  RecordFrame(args.decoder, entry->input_snapshot.get(), uint32_t(readable_input_size),
              result ? REX_RAW_ADDR(args.output) : nullptr, args.expected_output, total_before,
              !result);
  if (!result) {
    Report("lzx: decode error status %d (decoder %08X, wrote %zu of %u, consumed %zu of %u, "
           "native total %llu)",
           int(result.status), args.decoder, size_t(result.bytes_written), args.expected_output,
           size_t(result.bytes_consumed), args.compressed_size,
           (unsigned long long)entry->decoder->total_output_bytes());
    entry->failed = true;
  }

  CompleteDecode(ctx, base, args.decoder, args.bytes_written, static_cast<bool>(result),
                 static_cast<uint32_t>(result.bytes_written));
  CountFrame(static_cast<bool>(result), args.decoder, args.expected_output, args.compressed_size);
}

// XPORT_LZX_SHADOW=1 (test only): the recompiled decoder stays in charge and the native one decodes
// every frame again into a scratch buffer; the first frame of a stream that differs is reported and,
// with XPORT_LZX_DUMP, that stream's trace is written.
bool ShadowEnabled() {
  static const bool enabled = [] {
    const char* value = std::getenv("XPORT_LZX_SHADOW");
    return value && value[0] == '1';
  }();
  return enabled;
}

std::atomic<uint64_t> g_shadow_frames{0};

void ShadowFrame(const DecodeArguments& args, uint8_t* base, const uint8_t* input,
                 uint32_t input_size, uint32_t total_before, uint32_t decode_count) {
  const auto entry = GetOrCreateEntry(args.decoder, kRetailWindowSize);
  if (!entry) return;
  std::lock_guard lock(entry->mutex);
  if (entry->failed) return;
  const uint8_t* reference = REX_RAW_ADDR(args.output);
  const uint32_t native_total = uint32_t(entry->decoder->total_output_bytes());
  if (native_total != total_before) {
    Report("lzx shadow: stream state mismatch (decoder %08X, native total %u, guest total %u, "
           "decode count %u, input %u)",
           args.decoder, native_total, total_before, decode_count, args.compressed_size);
    entry->failed = true;
    RecordFrame(args.decoder, input, input_size, reference, args.expected_output, total_before,
                true);
    return;
  }
  std::vector<uint8_t> output(args.expected_output);
  const auto result = entry->decoder->DecodeFrame(std::span<const uint8_t>(input, input_size),
                                                  std::span<uint8_t>(output));
  const uint32_t retail_written = REX_LOAD_U32(args.bytes_written);
  const bool same = result && result.bytes_written == retail_written &&
                    std::memcmp(output.data(), reference, retail_written) == 0;
  if (!same) {
    size_t first = 0;
    while (first < retail_written && output[first] == reference[first]) ++first;
    Report("lzx shadow: DIFFERS (decoder %08X, status %d, wrote %zu of %u, consumed %zu of %u, "
           "retail wrote %u, first difference %zu, total %u, decode count %u)",
           args.decoder, int(result.status), size_t(result.bytes_written), args.expected_output,
           size_t(result.bytes_consumed), args.compressed_size, retail_written, first, total_before,
           decode_count);
    entry->failed = true;
  }
  const uint64_t frames = g_shadow_frames.fetch_add(1, std::memory_order_relaxed) + 1;
  if (frames == 1 || frames % 20000 == 0) {
    Report("lzx shadow: %llu frames compared", (unsigned long long)frames);
  }
  RecordFrame(args.decoder, input, input_size, reference, args.expected_output, total_before,
              !same);
}

}  // namespace
}  // namespace xport::lzx

PPC_FUNC(XD3D_LzxDecodeFrame) {
  if (!xport::lzx::NativeEnabled()) {
    if (xport::lzx::ShadowEnabled()) {
      const xport::lzx::DecodeArguments args = xport::lzx::ReadArguments(ctx);
      if (!xport::lzx::ValidateArguments(args)) {
        XD3D_IMP(LzxDecodeFrame)(ctx, base);
        return;
      }
      const uint32_t total_before = REX_LOAD_U32(args.decoder + xport::lzx::kTotalOutputOffset);
      const uint32_t decode_count = REX_LOAD_U32(args.decoder + xport::lzx::kDecodeCountOffset);
      const uint32_t input_size = args.compressed_size + xport::lzx::kRetailReadPadding;
      std::vector<uint8_t> input(REX_RAW_ADDR(args.input), REX_RAW_ADDR(args.input) + input_size);
      XD3D_IMP(LzxDecodeFrame)(ctx, base);
      if (ctx.r3.u32 == 0) {
        xport::lzx::ShadowFrame(args, base, input.data(), input_size, total_before, decode_count);
      }
      return;
    }
    if (!xport::lzx::DumpPrefix()) {
      XD3D_IMP(LzxDecodeFrame)(ctx, base);
      return;
    }
    // Recompiled decoder with a trace: the same frames and their reference output.
    const xport::lzx::DecodeArguments args = xport::lzx::ReadArguments(ctx);
    const uint32_t total_before = REX_LOAD_U32(args.decoder + xport::lzx::kTotalOutputOffset);
    const uint32_t input_size = args.compressed_size + xport::lzx::kRetailReadPadding;
    std::vector<uint8_t> input(REX_RAW_ADDR(args.input), REX_RAW_ADDR(args.input) + input_size);
    XD3D_IMP(LzxDecodeFrame)(ctx, base);
    const uint32_t total_after = REX_LOAD_U32(args.decoder + xport::lzx::kTotalOutputOffset);
    const bool ok = ctx.r3.u32 == 0;
    xport::lzx::RecordFrame(args.decoder, input.data(), input_size,
                           ok ? REX_RAW_ADDR(args.output) : nullptr, args.expected_output,
                           total_before,
                           xport::lzx::DumpAt() && total_before < xport::lzx::DumpAt() &&
                               total_after >= xport::lzx::DumpAt());
    return;
  }
  xport::lzx::DecodeFrame(ctx, base, xport::lzx::ReadArguments(ctx));
}

PPC_FUNC(XD3D_LzxResetDecoder) {
  xport::lzx::ResetDecoder(ctx, base);
}

PPC_FUNC(XD3D_LzxCreateDecoder) {
  xport::lzx::LogBackendOnce();
  const uint32_t outer = ctx.r3.u32;
  const uint64_t embedded = static_cast<uint64_t>(outer) + ctx.r6.u32;
  if (outer && embedded < xport::lzx::kGuestAddressSpaceSize &&
      xport::lzx::IsGuestRange(static_cast<uint32_t>(embedded), xport::lzx::kDecoderSpan)) {
    // The same guest allocation can be initialized again without destruction.
    // Do not let an old dictionary or failed-stream marker cross that boundary.
    xport::lzx::RemoveEntry(static_cast<uint32_t>(embedded));
    xport::lzx::ClearTrace(static_cast<uint32_t>(embedded));
  }
  XD3D_IMP(LzxCreateDecoder)(ctx, base);
}

PPC_FUNC(XD3D_LzxDestroyDecoder) {
  const uint32_t outer_object = ctx.r3.u32;
  if (xport::lzx::IsGuestRange(outer_object, xport::lzx::kEmbeddedDecoderOffset + sizeof(uint32_t))) {
    xport::lzx::RemoveEntry(outer_object + xport::lzx::kEmbeddedDecoderOffset);
    xport::lzx::ClearTrace(outer_object + xport::lzx::kEmbeddedDecoderOffset);
  }
  XD3D_IMP(LzxDestroyDecoder)(ctx, base);
}
