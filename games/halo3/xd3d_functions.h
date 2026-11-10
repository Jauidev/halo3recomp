#pragma once

// This game's D3D and XDK library functions, by name (tools/find_d3d.py; the hooks and the
// runtime use only these names). Generated: check the report before trusting a line.
#include "ppc_recomp_shared.h"

#define XD3D_PASTE2(a, b) a##b
#define XD3D_PASTE(a, b) XD3D_PASTE2(a, b)
#define XD3D_IMP(name) XD3D_PASTE(__imp__, XD3D_##name)

#define XD3D_AcquireCommandScratch sub_825AA870
#define XD3D_BeginTiling sub_825A9A60
#define XD3D_BeginVertices sub_8216E068
#define XD3D_ClearRectangle sub_82168F38
#define XD3D_CommandListBindFences sub_825AA768
#define XD3D_CreateDevice sub_825A82F8
#define XD3D_CreatePixelShader sub_825A3550
#define XD3D_CreateVertexShader sub_825A3660
#define XD3D_DeleteCommandList sub_825AAAB0
#define XD3D_DeviceClearFlagBit2 sub_825A8160
#define XD3D_DeviceCpuPrefix sub_825AEEB0
#define XD3D_DirtyAllState sub_82195618
#define XD3D_DrawIndexedVertices sub_82174DD8
#define XD3D_DrawVertices sub_8214EAB8
#define XD3D_EndTiling sub_825A9EF8
#define XD3D_GpuHangHandler sub_825AC100
#define XD3D_InitDeviceCpuState sub_825AD388
#define XD3D_InitDisplay sub_825ACF30
#define XD3D_InitDisplayMode sub_825ACCC8
#define XD3D_InitEngines sub_825AF1A0
#define XD3D_InitRing sub_825B12F0
#define XD3D_InitVertexShader sub_825A32E0  // called by CreateVertexShader
#define XD3D_KickOff sub_82147D38
#define XD3D_LockResource sub_82145CA0
#define XD3D_LockTextureRect sub_825A46B0
#define XD3D_LockTextureRectArray sub_8216B7B0
#define XD3D_PhysicalAlloc sub_825A7E78
#define XD3D_PresentWorker sub_825AE850
#define XD3D_QueryPerformanceFrequency sub_8259F310
#define XD3D_ReleaseResource sub_825A7618
#define XD3D_Resolve sub_82177870
#define XD3D_RunCommandList sub_82172570
#define XD3D_SetDepthStencilSurface sub_82168710
#define XD3D_SetDeviceBits10916 sub_82195710
#define XD3D_SetIndices sub_8214EA28
#define XD3D_SetPixelShader sub_8214D5B0
#define XD3D_SetPredication sub_82192C50
#define XD3D_SetRenderTarget sub_8214CAB0
#define XD3D_SetScissorRect sub_82152228
#define XD3D_SetShaderGprAllocation sub_8214C310
#define XD3D_SetStreamSource sub_8214CF40
#define XD3D_SetTexture sub_82167A60
#define XD3D_SetVertexDeclaration sub_8214D2D0
#define XD3D_SetVertexDeclarationDirect sub_82176F78
#define XD3D_SetVertexShader sub_8214D2E8
#define XD3D_Swap sub_82192F58
#define XD3D_TilingRestoreConstants sub_82168598
#define XD3D_TilingRestoreState sub_825A5C50
#define XD3D_UnlockResource sub_82145FB0
// XD3D_EndVertices: not found
// XD3D_GuestFree: inlined in sub_825A3550, sub_825A3660, sub_825A3EC0, sub_825A4B40, sub_825A4C60, sub_825A73B8, sub_825A7FA0, sub_825A82F8, sub_825A8C50, sub_825AAAB0, sub_825AC410, sub_825AD3F8, sub_825AD658, sub_826E4B70
// XD3D_LzxCreateDecoder: not found
// XD3D_LzxDecodeFrame: not found
// XD3D_LzxDecodeInternal: not found
// XD3D_LzxDestroyDecoder: not found
// XD3D_LzxParseBlock: not found
// XD3D_LzxResetDecoder: not found
// XD3D_QueryFrameSlot: not found
// XD3D_SetPresentBehaviorBit: not found

#define XD3D_ADDR_NullClearColor 0x82000AF8
#define XD3D_ADDR_TilingRestoreArgument 0x820010CC
#define XD3D_ADDR_VdGlobalDevice 0x820006B8

PPC_FUNC_IMPL(__imp__sub_825AA870);
PPC_FUNC_IMPL(__imp__sub_825A9A60);
PPC_FUNC_IMPL(__imp__sub_8216E068);
PPC_FUNC_IMPL(__imp__sub_82168F38);
PPC_FUNC_IMPL(__imp__sub_825AA768);
PPC_FUNC_IMPL(__imp__sub_825A82F8);
PPC_FUNC_IMPL(__imp__sub_825A3550);
PPC_FUNC_IMPL(__imp__sub_825A3660);
PPC_FUNC_IMPL(__imp__sub_825AAAB0);
PPC_FUNC_IMPL(__imp__sub_825A8160);
PPC_FUNC_IMPL(__imp__sub_825AEEB0);
PPC_FUNC_IMPL(__imp__sub_82195618);
PPC_FUNC_IMPL(__imp__sub_82174DD8);
PPC_FUNC_IMPL(__imp__sub_8214EAB8);
PPC_FUNC_IMPL(__imp__sub_825A9EF8);
PPC_FUNC_IMPL(__imp__sub_825AC100);
PPC_FUNC_IMPL(__imp__sub_825AD388);
PPC_FUNC_IMPL(__imp__sub_825ACF30);
PPC_FUNC_IMPL(__imp__sub_825ACCC8);
PPC_FUNC_IMPL(__imp__sub_825AF1A0);
PPC_FUNC_IMPL(__imp__sub_825B12F0);
PPC_FUNC_IMPL(__imp__sub_825A32E0);
PPC_FUNC_IMPL(__imp__sub_82147D38);
PPC_FUNC_IMPL(__imp__sub_82145CA0);
PPC_FUNC_IMPL(__imp__sub_825A46B0);
PPC_FUNC_IMPL(__imp__sub_8216B7B0);
PPC_FUNC_IMPL(__imp__sub_825A7E78);
PPC_FUNC_IMPL(__imp__sub_825AE850);
PPC_FUNC_IMPL(__imp__sub_8259F310);
PPC_FUNC_IMPL(__imp__sub_825A7618);
PPC_FUNC_IMPL(__imp__sub_82177870);
PPC_FUNC_IMPL(__imp__sub_82172570);
PPC_FUNC_IMPL(__imp__sub_82168710);
PPC_FUNC_IMPL(__imp__sub_82195710);
PPC_FUNC_IMPL(__imp__sub_8214EA28);
PPC_FUNC_IMPL(__imp__sub_8214D5B0);
PPC_FUNC_IMPL(__imp__sub_82192C50);
PPC_FUNC_IMPL(__imp__sub_8214CAB0);
PPC_FUNC_IMPL(__imp__sub_82152228);
PPC_FUNC_IMPL(__imp__sub_8214C310);
PPC_FUNC_IMPL(__imp__sub_8214CF40);
PPC_FUNC_IMPL(__imp__sub_82167A60);
PPC_FUNC_IMPL(__imp__sub_8214D2D0);
PPC_FUNC_IMPL(__imp__sub_82176F78);
PPC_FUNC_IMPL(__imp__sub_8214D2E8);
PPC_FUNC_IMPL(__imp__sub_82192F58);
PPC_FUNC_IMPL(__imp__sub_82168598);
PPC_FUNC_IMPL(__imp__sub_825A5C50);
PPC_FUNC_IMPL(__imp__sub_82145FB0);
