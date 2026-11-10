#pragma once

// ReXGlue's PPC context header, for the ported hooks only: PPCContext is this
// runtime's (XenonRecomp's ppc_context.h), so the hooks see the same type as the recompiled code.
#include "ppc_config.h"
#include "ppc_context.h"

#define REX_JOIN(x, y) x##y
#define REX_XSTRINGIFY(x) #x
#define REX_STRINGIFY(x) REX_XSTRINGIFY(x)
#define REX_FUNC(x) void x([[maybe_unused]] PPCContext& __restrict ctx, uint8_t* base)
#define REX_EXTERN(x) extern "C" REX_FUNC(x)
#define REX_WEAK_FUNC(x) __attribute__((weak, noinline)) REX_FUNC(x)

namespace rex::runtime {
PPCFunc* ResolveIndirectFunction(uint32_t guest_address);
}  // namespace rex::runtime
