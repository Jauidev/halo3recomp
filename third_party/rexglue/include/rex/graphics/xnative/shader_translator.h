#pragma once

// Xenos shaders the SPIR-V cache lacks, translated while the game runs (Xcompiler): the same
// XenosRecomp and DXC that make the cache offline (tools/shaders.py), with the same arguments, so
// a translated shader is the module the cache would have held. DXC is loaded on first use
// (dxcompiler.dll next to the exe, libdxcompiler.so in the APK); without it, or with
// XNATIVE_RUNTIME_SHADERS=0, a shader missing from the cache is still a cache miss.
// Translations are kept on disk ($XNATIVE_SHADER_CACHE_DIR, else <user folder>/xnative-shaders),
// so each shader is translated once.

#include <array>
#include <cstdint>
#include <vector>

#include <shader/shader_cache.h>

namespace rex::graphics::xnative {

struct RuntimeShader {
  // spirvOffset/lateSpirvOffset are offsets into smolv.
  ShaderCacheEntry entry;
  std::vector<uint8_t> smolv;
};

// Called by the D3D hook with the guest's shader container (header, constant table and microcode,
// big-endian) before D3D sees it. Translates the shader when neither the cache nor an earlier
// translation has it. hash is the canonical hash the renderer looks shaders up by.
void PrepareRuntimeShader(uint64_t hash, const uint8_t* container, uint32_t size, bool pixel);

// The translation of a shader the cache lacks, or nullptr.
const RuntimeShader* FindRuntimeShader(uint64_t hash);

#if (XPORT_TITLE_ID == 0x4D5307E6)
// Halo 3: the filters a shader's tfetch instructions force on each fetch constant (their mag, min,
// mip and aniso fields; the console takes the constant's only where they say "use fetch const").
// 11353 of the 36347 (shader, constant) pairs force one, always the same per pair: the water's
// fetch of its GPU-made wave map asks for linear while the map's constant says point, so the
// ripples came out in blocks. Read from the microcode by PrepareRuntimeShader/QueueRuntimeShader,
// stock-cache shaders included.
struct ShaderFetchFilters {
  uint32_t mask = 0;  // fetch constants with a forced filter
  // Per fetch constant: mag bits 0-1, min 2-3, mip 4-5 (3 = the constant's), aniso 6-8 (7 = the
  // constant's), as bits 12-20 of the instruction's second word.
  std::array<uint16_t, 32> filters{};
};
bool FindShaderFetchFilters(uint64_t hash, ShaderFetchFilters& filters);
#endif

#if (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
// Gears of War 3: shaders the cache lacks are translated on a thread of their own (XpShaderXlate)
// instead of in the D3D hook, where each took 40-100 ms of the game's render thread (phone,
// 2026-11-03: six at once when a zone streamed in froze the game ~0.35 s). Their draws are skipped
// until the translation is done, as draws whose pipeline is still compiling are. On by default on
// Android; XNATIVE_ASYNC_SHADERS=0/1 overrides.
bool AsyncRuntimeShaders();

// As PrepareRuntimeShader, but a shader that has to be translated is queued: true while it waits
// or is being translated (the caller registers it once RuntimeShaderPending says it is done).
bool QueueRuntimeShader(uint64_t hash, const uint8_t* container, uint32_t size, bool pixel);
bool RuntimeShaderPending(uint64_t hash);

// How many queued translations have finished (succeeded or failed), to tell cheaply whether any
// pending one may be done.
uint64_t RuntimeShaderSerial();
#endif

}  // namespace rex::graphics::xnative
