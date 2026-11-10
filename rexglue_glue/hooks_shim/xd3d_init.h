#pragma once

// stands in for LibertyRecomp's generated xd3d_init.h when its hooks
// (rexglue_glue/xd3d, ported by tools/port_native_hooks.py) are built against this runtime's
// recompiled the title code: the REX_ memory/call macros map to XenonRecomp's PPC_ ones, and every
// recompiled function the hooks call is declared.
#include <rex/types.h>

#include "xd3d_functions.h"

// This runtime maps every guest view at base + address (no 0xE0000000 page offset).
#define REX_PHYS_HOST_OFFSET(addr) 0u
#define REX_RAW_ADDR(x) (base + (uint32_t)(x))

#define REX_LOAD_U8(x) PPC_LOAD_U8(x)
#define REX_LOAD_U16(x) PPC_LOAD_U16(x)
#define REX_LOAD_U32(x) PPC_LOAD_U32(x)
#define REX_LOAD_U64(x) PPC_LOAD_U64(x)
#define REX_STORE_U8(x, y) PPC_STORE_U8(x, y)
#define REX_STORE_U16(x, y) PPC_STORE_U16(x, y)
#define REX_STORE_U32(x, y) PPC_STORE_U32(x, y)
#define REX_STORE_U64(x, y) PPC_STORE_U64(x, y)

#define REX_FUNC_PROLOGUE() PPC_FUNC_PROLOGUE()
#define REX_CALL_INDIRECT_FUNC(x) PPC_CALL_INDIRECT_FUNC(x)
