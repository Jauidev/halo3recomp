// The native renderer's shader ABI (push constants, shared constants, descriptor heaps), which
// tools/shaders.py hands XenosRecomp for every game. Copied from games/gta4/data/shader_overrides;
// GTA4_RECOMP names the renderer's shared-constant layout, which every game uses.
#ifndef SHADER_COMMON_H_INCLUDED
#define SHADER_COMMON_H_INCLUDED

#ifndef GTA4_RECOMP
#define GTA4_RECOMP 1
#endif

#define SPEC_CONSTANT_R11G11B10_NORMAL  (1 << 0)
#define SPEC_CONSTANT_ALPHA_TEST        (1 << 1)
#define SPEC_CONSTANT_ALPHA_TEST_FUNCTION_SHIFT 8
#define SPEC_CONSTANT_ALPHA_TEST_FUNCTION_MASK  0x700
#define SPEC_CONSTANT_ALPHA_TEST_CAPABILITY_MASK 0x702

#ifdef UNLEASHED_RECOMP
    #define SPEC_CONSTANT_BICUBIC_GI_FILTER (1 << 2)
    #define SPEC_CONSTANT_ALPHA_TO_COVERAGE (1 << 3)
    #define SPEC_CONSTANT_REVERSE_Z         (1 << 4)
#endif

#ifdef MARATHON_RECOMP
    #define SPEC_CONSTANT_CONDITIONAL_RENDERING (1 << 5)
#endif

#if defined(__air__) || !defined(__cplusplus) || defined(__INTELLISENSE__)

#ifndef __air__
#define FLT_MIN asfloat(0xff7fffff)
#define FLT_MAX asfloat(0x7f7fffff)
#endif

#ifdef __spirv__

struct PushConstants
{
    uint64_t VertexShaderConstants;
    uint64_t PixelShaderConstants;
    uint64_t SharedConstants;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PushConstants;

#ifdef GTA4_RECOMP
#define GetTextureLodBias(SLOT) vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 752 + (SLOT) * 4)
#define g_Booleans                  vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 528)
#define g_SwappedTexcoords          vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 532)
#define g_SwappedNormals            vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 536)
#define g_SwappedBinormals          vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 540)
#define g_SwappedTangents           vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 544)
#define g_SwappedBlendWeights       vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 548)
#define g_HalfPixelOffset           vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 552)
#define g_ClipPlane                 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 560)
#define g_ClipPlaneEnabled          vk::RawBufferLoad<bool>(g_PushConstants.SharedConstants + 576)
#define g_AlphaThreshold            vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 580)
#define g_conditionalSurveyIndex    vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 584)
#define g_conditionalRenderingIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 588)
#define g_AlphaToMaskSampleCount    vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 740)
#define g_AlphaToMask               vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 736)
#else
#define g_Booleans                  vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 320)
#define g_SwappedTexcoords          vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 324)
#define g_SwappedNormals            vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 328)
#define g_SwappedBinormals          vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 332)
#define g_SwappedTangents           vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 336)
#define g_SwappedBlendWeights       vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 340)
#define g_HalfPixelOffset           vk::RawBufferLoad<float2>(g_PushConstants.SharedConstants + 344)
#define g_ClipPlane                 vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 352)
#define g_ClipPlaneEnabled          vk::RawBufferLoad<bool>(g_PushConstants.SharedConstants + 368)
#define g_AlphaThreshold            vk::RawBufferLoad<float>(g_PushConstants.SharedConstants + 372)
#define g_conditionalSurveyIndex    vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 376)
#define g_conditionalRenderingIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 380)
#endif

[[vk::constant_id(0)]] const uint g_SpecConstants = 0;

#define g_SpecConstants() g_SpecConstants

#elif defined(__air__)

#include <metal_stdlib>

using namespace metal;

constant uint G_SPEC_CONSTANTS [[function_constant(0)]];
constant uint G_SPEC_CONSTANTS_VAL = is_function_constant_defined(G_SPEC_CONSTANTS) ? G_SPEC_CONSTANTS : 0;

uint g_SpecConstants()
{
    return G_SPEC_CONSTANTS_VAL;
}

struct PushConstants
{
    ulong VertexShaderConstants;
    ulong PixelShaderConstants;
    ulong SharedConstants;
};

#ifdef GTA4_RECOMP
#define GetTextureLodBias(SLOT) (*(reinterpret_cast<device float*>(g_PushConstants.SharedConstants + 752 + (SLOT) * 4)))
#define g_Booleans (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 528)))
#define g_SwappedTexcoords (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 532)))
#define g_SwappedNormals (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 536)))
#define g_SwappedBinormals (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 540)))
#define g_SwappedTangents (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 544)))
#define g_SwappedBlendWeights (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 548)))
#define g_HalfPixelOffset (*(reinterpret_cast<device float2*>(g_PushConstants.SharedConstants + 552)))
#define g_ClipPlane (*(reinterpret_cast<device float4*>(g_PushConstants.SharedConstants + 560)))
#define g_ClipPlaneEnabled (*(reinterpret_cast<device bool*>(g_PushConstants.SharedConstants + 576)))
#define g_AlphaThreshold (*(reinterpret_cast<device float*>(g_PushConstants.SharedConstants + 580)))
#define g_conditionalSurveyIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 584)))
#define g_conditionalRenderingIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 588)))
#else
#define g_Booleans (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 320)))
#define g_SwappedTexcoords (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 324)))
#define g_SwappedNormals (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 328)))
#define g_SwappedBinormals (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 332)))
#define g_SwappedTangents (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 336)))
#define g_SwappedBlendWeights (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 340)))
#define g_HalfPixelOffset (*(reinterpret_cast<device float2*>(g_PushConstants.SharedConstants + 344)))
#define g_ClipPlane (*(reinterpret_cast<device float4*>(g_PushConstants.SharedConstants + 352)))
#define g_ClipPlaneEnabled (*(reinterpret_cast<device bool*>(g_PushConstants.SharedConstants + 368)))
#define g_AlphaThreshold (*(reinterpret_cast<device float*>(g_PushConstants.SharedConstants + 372)))
#define g_conditionalSurveyIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 376)))
#define g_conditionalRenderingIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 380)))
#endif

#else

#ifdef GTA4_RECOMP
#define GetTextureLodBias(SLOT) g_TextureLodBias[(SLOT) / 4][(SLOT) % 4]
#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c33.x); \
    uint g_SwappedTexcoords : packoffset(c33.y); \
    uint g_SwappedNormals : packoffset(c33.z); \
    uint g_SwappedBinormals : packoffset(c33.w); \
    uint g_SwappedTangents : packoffset(c34.x);  \
    uint g_SwappedBlendWeights : packoffset(c34.y); \
    float2 g_HalfPixelOffset : packoffset(c34.z); \
    float4 g_ClipPlane : packoffset(c35.x); \
    bool g_ClipPlaneEnabled : packoffset(c36.x); \
    float g_AlphaThreshold : packoffset(c36.y); \
    uint g_conditionalSurveyIndex : packoffset(c36.z); \
    uint g_conditionalRenderingIndex : packoffset(c36.w); \
    float4 g_TextureLodBias[7] : packoffset(c47);
#else
#define DEFINE_SHARED_CONSTANTS() \
    uint g_Booleans : packoffset(c20.x); \
    uint g_SwappedTexcoords : packoffset(c20.y); \
    uint g_SwappedNormals : packoffset(c20.z); \
    uint g_SwappedBinormals : packoffset(c20.w); \
    uint g_SwappedTangents : packoffset(c21.x);  \
    uint g_SwappedBlendWeights : packoffset(c21.y); \
    float2 g_HalfPixelOffset : packoffset(c21.z); \
    float4 g_ClipPlane : packoffset(c22.x); \
    bool g_ClipPlaneEnabled : packoffset(c23.x); \
    float g_AlphaThreshold : packoffset(c23.y); \
    uint g_conditionalSurveyIndex : packoffset(c23.z); \
    uint g_conditionalRenderingIndex : packoffset(c23.w);
#endif

uint g_SpecConstants();

#endif

#ifndef GTA4_RECOMP
#define GetTextureLodBias(SLOT) 0.0
#endif

#if defined(GTA4_RECOMP) && defined(__spirv__)
uint ComputeXenosAlphaToMask(float alpha, float2 position, uint alphaToMask,
                             uint sampleCount)
{
    if ((alphaToMask & 0x100u) == 0u)
        return 0xFFFFFFFFu;

    uint offsetIndex = (uint(position.x) & 1u) | ((uint(position.y) & 1u) << 1u);
    uint offset = (alphaToMask >> (offsetIndex << 1u)) & 3u;
    float thresholdOffset = float(offset);
    uint sampleMask = 0u;

    if (sampleCount == 1u)
    {
        if (alpha >= 1.0 - thresholdOffset * 0.25)
            sampleMask |= 1u;
    }
    else if (sampleCount == 2u)
    {
        // Xenos top and bottom samples map to Vulkan samples 1 and 0.
        if (alpha >= 0.5 - thresholdOffset * 0.125)
            sampleMask |= 2u;
        if (alpha >= 1.0 - thresholdOffset * 0.125)
            sampleMask |= 1u;
    }
    else if (sampleCount == 4u)
    {
        // Xenos TL, BL, TR, BR map to Vulkan TL, TR, BL, BR.
        if (alpha >= 0.75 - thresholdOffset * 0.0625)
            sampleMask |= 1u;
        if (alpha >= 0.25 - thresholdOffset * 0.0625)
            sampleMask |= 4u;
        if (alpha >= 0.5 - thresholdOffset * 0.0625)
            sampleMask |= 2u;
        if (alpha >= 1.0 - thresholdOffset * 0.0625)
            sampleMask |= 8u;
    }
    else
    {
        return 0xFFFFFFFFu;
    }

    return sampleMask;
}
#endif

float4 cube(float4 value)
{
    float3 src = value.zwx;
    float3 abs_src = abs(src);

    float sc, tc, ma, id;

    if (abs_src.z >= abs_src.x && abs_src.z >= abs_src.y)
    {
        // Z major axis
        tc = -src.y;
        sc = src.z < 0.0 ? -src.x : src.x;
        ma = 2.0 * src.z;
        id = src.z < 0.0 ? 5.0 : 4.0;
    }
    else if (abs_src.y >= abs_src.x)
    {
        // Y major axis
        tc = src.y < 0.0 ? -src.z : src.z;
        sc = src.x;
        ma = 2.0 * src.y;
        id = src.y < 0.0 ? 3.0 : 2.0;
    }
    else
    {
        // X major axis
        tc = -src.y;
        sc = src.x < 0.0 ? src.z : -src.z;
        ma = 2.0 * src.x;
        id = src.x < 0.0 ? 1.0 : 0.0;
    }

    // Return as per Xbox 360 cube instruction output format:
    // x = t coordinate
    // y = s coordinate
    // z = 2 * major axis
    // w = face ID
    return float4(tc, sc, ma, id);
}

float3 cubeDir(float3 texCoord)
{
    // Move from 1...2 to -1...1
    float sc = (texCoord.x * 2.0) - 3.0;
    float tc = (texCoord.y * 2.0) - 3.0;

    uint face = uint(clamp(texCoord.z, 0.0, 5.0));

    // Split face into axis and sign
    uint axis = face >> 1;
    uint neg = face & 1;

    float3 dir;

    switch(axis)
    {
    case 0: // X major axis
        dir.y = -tc;
        dir.z = neg ? sc : -sc;
        dir.x = neg ? -1.0 : 1.0;
        break;

    case 1: // Y major axis
        dir.x = sc;
        dir.z = neg ? -tc : tc;
        dir.y = neg ? -1.0 : 1.0;
        break;

    default: // Z major axis
        dir.x = neg ? -sc : sc;
        dir.y = -tc;
        dir.z = neg ? -1.0 : 1.0;
        break;
    }

    return dir;
}

#ifdef __air__

struct Texture2DDescriptorHeap
{
    texture2d<float> tex;
};

struct Texture2DArrayDescriptorHeap
{
    texture2d_array<float> tex;
};

struct Texture3DDescriptorHeap
{
    texture3d<float> tex;
};

struct TextureCubeDescriptorHeap
{
    texturecube<float> tex;
};

struct SamplerDescriptorHeap
{
    sampler samp;
};

struct AtomicUintBuffer
{
    device atomic_uint* buffer;
};

uint2 getTexture2DDimensions(texture2d<float> texture)
{
    return uint2(texture.get_width(), texture.get_height());
}

uint3 getTexture2DArrayDimensions(texture2d_array<float> texture)
{
    return uint3(texture.get_width(), texture.get_height(), texture.get_array_size());
}

uint3 getTexture3DDimensions(texture3d<float> texture)
{
    return uint3(texture.get_width(), texture.get_height(), texture.get_depth());
}

float4 tfetch2D(constant Texture2DDescriptorHeap* textureHeap,
                constant SamplerDescriptorHeap* samplerHeap,
                uint resourceDescriptorIndex,
                uint samplerDescriptorIndex,
                float2 texCoord, float2 offset, float lodBias = 0.0)
{
    texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    return texture.sample(sampler, texCoord + offset / (float2)getTexture2DDimensions(texture), bias(lodBias));
}

float4 tfetch2DArray(constant Texture2DArrayDescriptorHeap* textureHeap,
                     constant SamplerDescriptorHeap* samplerHeap,
                     uint resourceDescriptorIndex,
                     uint samplerDescriptorIndex,
                     float3 texCoord, float3 offset, float lodBias = 0.0)
{
    texture2d_array<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    uint3 dimensions = getTexture2DArrayDimensions(texture);
    // The Z offset is in slices (see the SPIR-V version).
    return texture.sample(sampler, texCoord.xy + offset.xy / float2(dimensions.xy),
                          uint(clamp(floor(texCoord.z * dimensions.z + offset.z), 0.0, float(dimensions.z) - 1.0)), bias(lodBias));
}

float4 tfetch3D(constant Texture3DDescriptorHeap* textureHeap,
                constant SamplerDescriptorHeap* samplerHeap,
                uint resourceDescriptorIndex,
                uint samplerDescriptorIndex,
                float3 texCoord, float3 offset, float lodBias = 0.0)
{
    texture3d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    return texture.sample(sampler, texCoord + offset / float3(getTexture3DDimensions(texture)), bias(lodBias));
}

float4 tfetchCube(constant TextureCubeDescriptorHeap* textureHeap,
                  constant SamplerDescriptorHeap* samplerHeap,
                  uint resourceDescriptorIndex,
                  uint samplerDescriptorIndex,
                  float3 texCoord, float lodBias = 0.0)
{
    texturecube<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    float3 dir = cubeDir(texCoord);
    return texture.sample(sampler, dir, bias(lodBias));
}

float2 getWeights2D(constant Texture2DDescriptorHeap* textureHeap,
                    constant SamplerDescriptorHeap* samplerHeap,
                    uint resourceDescriptorIndex,
                    uint samplerDescriptorIndex,
                    float2 texCoord, float2 offset)
{
    texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    return select(fract(texCoord * float2(getTexture2DDimensions(texture)) + offset - 0.5), 0.0, isnan(texCoord));
}

float3 getWeights2DArray(constant Texture2DArrayDescriptorHeap* textureHeap,
                         constant SamplerDescriptorHeap* samplerHeap,
                         uint resourceDescriptorIndex,
                         uint samplerDescriptorIndex,
                         float3 texCoord, float3 offset)
{
    texture2d_array<float> texture = textureHeap[resourceDescriptorIndex].tex;
    return select(fract(texCoord * float3(getTexture2DArrayDimensions(texture)) + offset - 0.5), 0.0, isnan(texCoord));
}

float3 getWeights3D(constant Texture3DDescriptorHeap* textureHeap,
                    constant SamplerDescriptorHeap* samplerHeap,
                    uint resourceDescriptorIndex,
                    uint samplerDescriptorIndex,
                    float3 texCoord, float3 offset)
{
    texture3d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    return select(fract(texCoord * float3(getTexture3DDimensions(texture)) + offset - 0.5),
                  0.0, isnan(texCoord));
}

// Xenos getGradients (fetch opcode 18): the screen derivatives of a coordinate pair as
// (d.x/dx, d.x/dy, d.y/dx, d.y/dy), as Xenia's translators return them. Halo 3's antialiased HUD
// icons (PS 9A6740E32276BDFF: grenades, weapon icon, motion tracker) take 4 rotated taps at
// uv +- 0.22 * (dx +- dy); the instruction was dropped, the taps used the interpolator's
// pixel position instead and landed outside the icons (transparent).
#ifdef XENOS_RECOMP_PIXEL_SHADER
float4 xcGetGradients(float2 texCoord)
{
    float2 h = dfdx(texCoord);
    float2 v = dfdy(texCoord);
    return float4(h.x, v.x, h.y, v.y);
}
#else
float4 xcGetGradients(float2 texCoord)
{
    return float4(0.0);
}
#endif

#else

Texture2D<float4> g_Texture2DDescriptorHeap[] : register(t0, space0);
Texture2DArray<float4> g_Texture2DArrayDescriptorHeap[] : register(t0, space1);
Texture3D<float4> g_Texture3DDescriptorHeap[] : register(t0, space2);
TextureCube<float4> g_TextureCubeDescriptorHeap[] : register(t0, space3);
SamplerState g_SamplerDescriptorHeap[] : register(s0, space4);

#ifdef MARATHON_RECOMP
RWStructuredBuffer<uint> g_ConditionalSurveyBuffer : register(u0, space5);
#endif

uint2 getTexture2DDimensions(Texture2D<float4> texture)
{
    uint2 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y);
    return dimensions;
}

uint3 getTexture2DArrayDimensions(Texture2DArray<float4> texture)
{
    uint4 dimensions;
    texture.GetDimensions(0, dimensions.x, dimensions.y, dimensions.z, dimensions.w);
    return dimensions.xyz;
}

uint3 getTexture3DDimensions(Texture3D<float4> texture)
{
    uint3 dimensions;
    texture.GetDimensions(dimensions.x, dimensions.y, dimensions.z);
    return dimensions;
}

// Bit 31 of a sampler index: the fetch has TextureSign::kGamma on its colour. The textures stay
// UNORM and the shader applies the Xenos curve, which is piecewise and not sRGB (it gives twice
// as much light in the darkest tones).
// Bits 24-27: the fetched x, y, z, w have TextureSign::kUnsignedBiased (the GPU returns value * 2 - 1:
// Halo 3's CTX1 and DXN normal maps). Sampler indices stay below bit 24.
#define XENOS_SAMPLER_INDEX_MASK 0x00FFFFFFu

// Bits 28-29 of a sampler index (Halo 3): the texture a 3D/stacked fetch (XenosRecomp's 2DArray
// functions) reads is a plain 2D image (28) or a real 3D one (29). The console addresses a fetch by
// its fetch constant's dimension; here the 2D array slot of such a texture held the null image, so
// it read zeros: Halo 3's water PS fetches its V8U8 wave map (a 2D texture) this way and lost its
// ripples. These read the texture's own view instead.
#define XENOS_SAMPLER_PLAIN_2D 0x10000000u
#define XENOS_SAMPLER_VOLUME 0x20000000u

float4 xenosGammaFetch(uint samplerDescriptorIndex, float4 value)
{
    if (samplerDescriptorIndex & 0x80000000u)
    {
        float3 g = value.rgb;
        value.rgb = max(max(g * 0.25, g * 0.5 - 16.0 / 255.0), max(g - 64.0 / 255.0, g * 2.0 - 256.0 / 255.0));
    }
    if (samplerDescriptorIndex & 0x0F000000u)
    {
        uint biased = samplerDescriptorIndex >> 24;
        float4 bias = float4((biased & 1u) != 0u, (biased & 2u) != 0u, (biased & 4u) != 0u, (biased & 8u) != 0u);
        value = lerp(value, value * 2.0 - 1.0, bias);
    }
    return value;
}

// Xcompiler (Halo 3): textureInfo is the stage's s<N>_TextureInfo (texture_info in the renderer's
// NativeSharedConstants): (1/width, 1/height, 1/depth, depth) of the image the stage binds, which
// sits in the slot of its own dimension, or (1, 1, 1, 1), the null images' size, for no image or a
// cube. The fetches queried their texture's size at every fetch of every pixel (a size query, a
// conversion and a division each: 24 per pixel in the waterfall's water) for a value that is the
// same for the whole draw. The 2D slot holds the stage's texture only for a plain 2D (or 1D)
// image (bit 28), else the null image (1x1), whose size a 2D fetch of it read.
float2 xcTextureInvSize2D(uint samplerDescriptorIndex, float4 textureInfo)
{
    const bool2 plain2D = (samplerDescriptorIndex & XENOS_SAMPLER_PLAIN_2D) != 0u;
    return select(plain2D, textureInfo.xy, float2(1.0, 1.0));
}

// Pixel shaders need implicit derivatives for authored mip selection,
// trilinear filtering and anisotropic footprints. Vertex shaders still use
// explicit level zero because implicit derivatives are not available there.
// Vulkan applies sampler bias after either explicit or implicit base LOD;
// the host has already clamped lodBias to the device sampler-bias limit.
float4 tfetch2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float2 texCoord, float2 offset, float lodBias = 0.0)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    const float2 coord = texCoord + offset * xcTextureInvSize2D(samplerDescriptorIndex, textureInfo);
#ifdef XENOS_RECOMP_PIXEL_SHADER
    return xenosGammaFetch(samplerDescriptorIndex, texture.SampleBias(g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK], coord, lodBias));
#else
    return xenosGammaFetch(samplerDescriptorIndex, texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK], coord, 0.0 + lodBias));
#endif
}

// 1D fetches (XenosRecomp passes the coordinate alone): s<N>_Texture1DDescriptorIndex reads the 2D
// entry, so a texture bound there is sampled along its middle row. Only shaders that use it see it.
float4 tfetch1D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float texCoord, float lodBias = 0.0)
{
    return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo, float2(texCoord, 0.5), float2(0.0, 0.0), lodBias);
}

// Only through tfetch2DArray (XenosRecomp names the 3D fetches 2DArray), for a real 3D texture
// (bit 29): textureInfo is its size.
float4 tfetch3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float3 texCoord, float3 offset, float lodBias = 0.0)
{
    Texture3D<float4> texture = g_Texture3DDescriptorHeap[resourceDescriptorIndex];
    const float3 coord = texCoord + offset * textureInfo.xyz;
#ifdef XENOS_RECOMP_PIXEL_SHADER
    return xenosGammaFetch(samplerDescriptorIndex, texture.SampleBias(g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK],
                          coord, lodBias));
#else
    return xenosGammaFetch(samplerDescriptorIndex, texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK],
                               coord, 0.0 + lodBias));
#endif
}

float4 tfetch2DArray(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float3 texCoord, float3 offset, float lodBias = 0.0)
{
    if (samplerDescriptorIndex & XENOS_SAMPLER_PLAIN_2D)
        return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo, texCoord.xy, offset.xy, lodBias);
    if (samplerDescriptorIndex & XENOS_SAMPLER_VOLUME)
        return tfetch3D(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo, texCoord, offset, lodBias);
    Texture2DArray<float4> texture = g_Texture2DArrayDescriptorHeap[resourceDescriptorIndex];
    // A stacked texture's layer count, the null image's 1 for any other.
    const float layers = textureInfo.w;
#ifdef XENOS_RECOMP_PIXEL_SHADER
    // Xenos filters between the slices of a stacked texture (Gears of War 3's color grading table
    // would show bands otherwise); a host array texture only filters inside each layer.
    // The fetch's Z offset is in slices, added to the unnormalised layer: Halo 3's per-pixel
    // lightmaps read their SH layers 0-7 with z = 0 and offsets 0.5-7.5 (ignored before, every
    // read returned layer 0 and the terrain came out several times too bright).
    // A fetch at a slice's centre (those lightmaps: z = 0, the offsets put the layer on k exactly;
    // XenosRecomp adds no rounding nudge in Z) has weight 0 and does not read the next slice, which
    // lerp would multiply by 0: one texture read instead of two, the same value. The first read
    // stays outside the branch, so its implicit LOD keeps the quad's derivatives.
    const float layer = texCoord.z * layers + offset.z - 0.5;
    const float firstLayer = clamp(floor(layer), 0.0, layers - 1.0);
    const float nextLayer = min(firstLayer + 1.0, layers - 1.0);
    const float weight = saturate(layer - firstLayer);
    const float2 layerCoord = texCoord.xy + offset.xy * textureInfo.xy;
    SamplerState layerSampler = g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK];
    float4 value = texture.SampleBias(layerSampler, float3(layerCoord, firstLayer), lodBias);
    [branch] if (weight != 0.0)
        value = lerp(value, texture.SampleBias(layerSampler, float3(layerCoord, nextLayer), lodBias), weight);
    return xenosGammaFetch(samplerDescriptorIndex, value);
#else
    // As in pixel shaders (Halo 3's water reads its animated wave slices here). Before, the host
    // rounded the array coordinate to the nearest even layer and ignored the Z offset.
    // The next slice only when it has weight, as in pixel shaders.
    const float layer = texCoord.z * layers + offset.z - 0.5;
    const float firstLayer = clamp(floor(layer), 0.0, layers - 1.0);
    const float nextLayer = min(firstLayer + 1.0, layers - 1.0);
    const float weight = saturate(layer - firstLayer);
    const float2 layerCoord = texCoord.xy + offset.xy * textureInfo.xy;
    SamplerState layerSampler = g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK];
    float4 value = texture.SampleLevel(layerSampler, float3(layerCoord, firstLayer), 0.0 + lodBias);
    [branch] if (weight != 0.0)
        value = lerp(value, texture.SampleLevel(layerSampler, float3(layerCoord, nextLayer), 0.0 + lodBias), weight);
    return xenosGammaFetch(samplerDescriptorIndex, value);
#endif
}

// Fetches whose instruction has tx_coord_denorm: the coordinates (and offsets) are in texels.
float4 tfetch2DDenorm(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float2 texCoord, float2 offset, float lodBias = 0.0)
{
    return tfetch2D(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo,
                    texCoord * xcTextureInvSize2D(samplerDescriptorIndex, textureInfo), offset, lodBias);
}

float4 tfetch1DDenorm(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float texCoord, float lodBias = 0.0)
{
    return tfetch1D(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo,
                    texCoord * xcTextureInvSize2D(samplerDescriptorIndex, textureInfo).x, lodBias);
}

float4 tfetch2DArrayDenorm(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float3 texCoord, float3 offset, float lodBias = 0.0)
{
    if (samplerDescriptorIndex & XENOS_SAMPLER_PLAIN_2D)
        return tfetch2DDenorm(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo, texCoord.xy, offset.xy, lodBias);
    if (samplerDescriptorIndex & XENOS_SAMPLER_VOLUME)
        return tfetch3D(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo, texCoord * textureInfo.xyz, offset, lodBias);
    return tfetch2DArray(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo, texCoord * textureInfo.xyz, offset, lodBias);
}

float4 tfetchCube(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float lodBias = 0.0)
{
    float3 dir = cubeDir(texCoord);
#ifdef XENOS_RECOMP_PIXEL_SHADER
    return xenosGammaFetch(samplerDescriptorIndex, g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleBias(
        g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK], dir, lodBias));
#else
    return xenosGammaFetch(samplerDescriptorIndex, g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleLevel(
        g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK], dir, 0.0 + lodBias));
#endif
}

float2 getWeights2D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DDimensions(texture) + offset - 0.5));
}

float3 getWeights3D(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float3 offset)
{
    Texture3D<float4> texture = g_Texture3DDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0,
                  frac(texCoord * getTexture3DDimensions(texture) + offset - 0.5));
}

float3 getWeights2DArray(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float3 offset)
{
    if (samplerDescriptorIndex & XENOS_SAMPLER_PLAIN_2D)
        return float3(getWeights2D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord.xy, offset.xy), 0.0);
    if (samplerDescriptorIndex & XENOS_SAMPLER_VOLUME)
        return getWeights3D(resourceDescriptorIndex, samplerDescriptorIndex, texCoord, offset);
    Texture2DArray<float4> texture = g_Texture2DArrayDescriptorHeap[resourceDescriptorIndex];
    return select(isnan(texCoord), 0.0, frac(texCoord * getTexture2DArrayDimensions(texture) + offset - 0.5));
}

// Xenos getGradients (fetch opcode 18): the screen derivatives of a coordinate pair as
// (d.x/dx, d.x/dy, d.y/dx, d.y/dy), as Xenia's translators return them. Halo 3's antialiased HUD
// icons (PS 9A6740E32276BDFF: grenades, weapon icon, motion tracker) take 4 rotated taps at
// uv +- 0.22 * (dx +- dy); the instruction was dropped, the taps used the interpolator's
// pixel position instead and landed outside the icons (transparent).
#ifdef XENOS_RECOMP_PIXEL_SHADER
float4 xcGetGradients(float2 texCoord)
{
    float2 h = ddx(texCoord);
    float2 v = ddy(texCoord);
    return float4(h.x, v.x, h.y, v.y);
}
#else
float4 xcGetGradients(float2 texCoord)
{
    return 0.0;
}
#endif

#if defined(XENOS_RECOMP_PIXEL_SHADER) && defined(__spirv__)
// Xcompiler (Halo 3): true in the four pixels of a 2x2 quad when `value` is true in all four, false
// in all four otherwise. The fine derivatives of a 0/1 value are exact: a horizontal pair keeps 1
// only when both pixels are 1, then the vertical pair of those. Only in quad-uniform control flow.
// XenosRecomp returns early from a quad whose four pixels are discarded (a kill, or an emulated clip
// plane): a discard is a demote, so they kept running to the end of the shader as helpers (Halo's
// foliage clips after its alpha-mask fetch, then still read six textures). Live quads keep all four
// pixels, so their derivatives and results do not change.
bool XcQuadAllTrue(bool value)
{
    float v = value ? 1.0 : 0.0;
    v -= abs(ddx_fine(v));
    return v - abs(ddy_fine(v)) > 0.5;
}
#endif

// Xenos fetches with UseRegisterLOD (XenosRecomp appends "Lod" and passes the setTexLOD register
// plus the instruction's bias): that mip level, in pixel and vertex shaders alike. 3D fetches go
// through the 2D array path (XenosRecomp names them 2DArray).
float4 tfetch2DLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float2 texCoord, float2 offset, float lod)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    const float2 coord = texCoord + offset * xcTextureInvSize2D(samplerDescriptorIndex, textureInfo);
    return xenosGammaFetch(samplerDescriptorIndex, texture.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK], coord, lod));
}

float4 tfetch1DLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float texCoord, float lod)
{
    return tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo, float2(texCoord, 0.5), float2(0.0, 0.0), lod);
}

float4 tfetch2DArrayLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float3 texCoord, float3 offset, float lod)
{
    if (samplerDescriptorIndex & XENOS_SAMPLER_PLAIN_2D)
        return tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo, texCoord.xy, offset.xy, lod);
    if (samplerDescriptorIndex & XENOS_SAMPLER_VOLUME)
    {
        Texture3D<float4> volume = g_Texture3DDescriptorHeap[resourceDescriptorIndex];
        return xenosGammaFetch(samplerDescriptorIndex, volume.SampleLevel(g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK],
                               texCoord + offset * textureInfo.xyz, lod));
    }
    Texture2DArray<float4> texture = g_Texture2DArrayDescriptorHeap[resourceDescriptorIndex];
    // A stacked texture's layer count, the null image's 1 for any other.
    const float layers = textureInfo.w;
    // The next slice only when it has weight (see tfetch2DArray).
    const float layer = texCoord.z * layers + offset.z - 0.5;
    const float firstLayer = clamp(floor(layer), 0.0, layers - 1.0);
    const float nextLayer = min(firstLayer + 1.0, layers - 1.0);
    const float weight = saturate(layer - firstLayer);
    const float2 layerCoord = texCoord.xy + offset.xy * textureInfo.xy;
    SamplerState layerSampler = g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK];
    float4 value = texture.SampleLevel(layerSampler, float3(layerCoord, firstLayer), lod);
    [branch] if (weight != 0.0)
        value = lerp(value, texture.SampleLevel(layerSampler, float3(layerCoord, nextLayer), lod), weight);
    return xenosGammaFetch(samplerDescriptorIndex, value);
}

float4 tfetchCubeLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float3 texCoord, float lod)
{
    float3 dir = cubeDir(texCoord);
    return xenosGammaFetch(samplerDescriptorIndex, g_TextureCubeDescriptorHeap[resourceDescriptorIndex].SampleLevel(
        g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK], dir, lod));
}

float4 tfetch2DDenormLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float2 texCoord, float2 offset, float lod)
{
    return tfetch2DLod(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo,
                       texCoord * xcTextureInvSize2D(samplerDescriptorIndex, textureInfo), offset, lod);
}

float4 tfetch1DDenormLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float texCoord, float lod)
{
    return tfetch1DLod(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo,
                       texCoord * xcTextureInvSize2D(samplerDescriptorIndex, textureInfo).x, lod);
}

float4 tfetch2DArrayDenormLod(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float4 textureInfo, float3 texCoord, float3 offset, float lod)
{
    if (samplerDescriptorIndex & XENOS_SAMPLER_PLAIN_2D)
        return tfetch2DDenormLod(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo, texCoord.xy, offset.xy, lod);
    // A real 3D texture (bit 29, its size) or a stacked one (the null image's for any other).
    return tfetch2DArrayLod(resourceDescriptorIndex, samplerDescriptorIndex, textureInfo, texCoord * textureInfo.xyz, offset, lod);
}

#endif

#ifdef __air__
#define selectWrapper(a, b, c) select(c, b, a)
#else
#define selectWrapper(a, b, c) select(a, b, c)
#endif

#ifdef __air__
#define frac(X) fract(X)

template<typename T>
void clip(T a)
{
    if (a < 0.0) {
        discard_fragment();
    }
}

template<typename T>
float rcp(T a)
{
    return 1.0 / a;
}

template<typename T>
float4x4 mul(T a, T b)
{
    return b * a;
}
#endif

#ifdef __air__
#define UNROLL
#define BRANCH
#else
#define UNROLL [unroll]
#define BRANCH [branch]
#endif

#ifdef __air__
#define XC_ASUINT(x) as_type<uint>(x)
#define XC_ASFLOAT(x) as_type<float>(x)
#else
#define XC_ASUINT(x) asuint(x)
#define XC_ASFLOAT(x) asfloat(x)
#endif

// Results of rcp/rsq/log tested on their bits: a Vulkan driver may assume floats are finite
// unless the shader declares SignedZeroInfNanPreserve, and then drop isinf() or
// clamp(x, -FLT_MAX, FLT_MAX) as no-ops; integer operations it cannot drop.

// Xenos _clamp forms (and the IEEE ones, since the products here are not Xenos' zero-preserving
// ones): +-Inf -> +-FLT_MAX. NaN -> -FLT_MAX, which is what clamp() gave on the PC (NVIDIA's
// min/max return the number), so the PC picture does not change.
float xcClampInf(float value)
{
    uint bits = XC_ASUINT(value);
    uint magnitude = bits & 0x7FFFFFFFu;
    if (magnitude < 0x7F800000u)
        return value;
    return XC_ASFLOAT(magnitude == 0x7F800000u ? ((bits & 0x80000000u) | 0x7F7FFFFFu) : 0xFF7FFFFFu);
}

// Xenos RECIP_FF / RECIPSQ_FF (rcpf, rsqf): an infinite result becomes a zero of the same sign,
// as in Xenia's SPIR-V translator (kRcpf, kRsqf).
float xcInfToZero(float value)
{
    uint bits = XC_ASUINT(value);
    return (bits & 0x7FFFFFFFu) == 0x7F800000u ? XC_ASFLOAT(bits & 0x80000000u) : value;
}

float w0(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-a + 3.0f) - 3.0f) + 1.0f);
}

float w1(float a)
{
    return (1.0f / 6.0f) * (a * a * (3.0f * a - 6.0f) + 4.0f);
}

float w2(float a)
{
    return (1.0f / 6.0f) * (a * (a * (-3.0f * a + 3.0f) + 3.0f) + 1.0f);
}

float w3(float a)
{
    return (1.0f / 6.0f) * (a * a * a);
}

float g0(float a)
{
    return w0(a) + w1(a);
}

float g1(float a)
{
    return w2(a) + w3(a);
}

float h0(float a)
{
    return -1.0f + w1(a) / (w0(a) + w1(a)) + 0.5f;
}

float h1(float a)
{
    return 1.0f + w3(a) / (w2(a) + w3(a)) + 0.5f;
}

#ifdef __air__

float4 tfetch2DBicubic(constant Texture2DDescriptorHeap* textureHeap,
                       constant SamplerDescriptorHeap* samplerHeap,
                       uint resourceDescriptorIndex,
                       uint samplerDescriptorIndex,
                       float2 texCoord, float2 offset, float lodBias = 0.0)
{
    texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    sampler sampler = samplerHeap[samplerDescriptorIndex].samp;
    uint2 dimensions = getTexture2DDimensions(texture);

    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.sample(sampler, float2(px + h0x, py + h0y) / float2(dimensions), bias(lodBias)) +
              g1x * texture.sample(sampler, float2(px + h1x, py + h0y) / float2(dimensions), bias(lodBias))) +
        g1(fy) * (g0x * texture.sample(sampler, float2(px + h0x, py + h1y) / float2(dimensions), bias(lodBias)) +
              g1x * texture.sample(sampler, float2(px + h1x, py + h1y) / float2(dimensions), bias(lodBias)));

    return r;
}

#else

float4 tfetch2DBicubic(uint resourceDescriptorIndex, uint samplerDescriptorIndex, float2 texCoord, float2 offset, float lodBias = 0.0)
{
    Texture2D<float4> texture = g_Texture2DDescriptorHeap[resourceDescriptorIndex];
    SamplerState samplerState = g_SamplerDescriptorHeap[samplerDescriptorIndex & XENOS_SAMPLER_INDEX_MASK];
    uint2 dimensions = getTexture2DDimensions(texture);

    float x = texCoord.x * dimensions.x + offset.x;
    float y = texCoord.y * dimensions.y + offset.y;

    x -= 0.5f;
    y -= 0.5f;
    float px = floor(x);
    float py = floor(y);
    float fx = x - px;
    float fy = y - py;

    float g0x = g0(fx);
    float g1x = g1(fx);
    float h0x = h0(fx);
    float h1x = h1(fx);
    float h0y = h0(fy);
    float h1y = h1(fy);

    float4 r =
        g0(fy) * (g0x * texture.SampleBias(samplerState, float2(px + h0x, py + h0y) / float2(dimensions), lodBias) +
            g1x * texture.SampleBias(samplerState, float2(px + h1x, py + h0y) / float2(dimensions), lodBias)) +
        g1(fy) * (g0x * texture.SampleBias(samplerState, float2(px + h0x, py + h1y) / float2(dimensions), lodBias) +
            g1x * texture.SampleBias(samplerState, float2(px + h1x, py + h1y) / float2(dimensions), lodBias));

    return xenosGammaFetch(samplerDescriptorIndex, r);
}

#endif

float4 tfetchR11G11B10(uint4 value)
{
    if (g_SpecConstants() & SPEC_CONSTANT_R11G11B10_NORMAL)
    {
        return float4(
            (value.x & 0x00000400 ? -1.0 : 0.0) + ((value.x & 0x3FF) / 1024.0),
            (value.x & 0x00200000 ? -1.0 : 0.0) + (((value.x >> 11) & 0x3FF) / 1024.0),
            (value.x & 0x80000000 ? -1.0 : 0.0) + (((value.x >> 22) & 0x1FF) / 512.0),
            0.0);
    }
    else
    {
#ifdef __air__
        return as_type<float4>(value);
#else
        return asfloat(value);
#endif
    }
}

float4 swapFloats(uint swappedFloats, float4 value, uint semanticIndex)
{
    return (swappedFloats & (1ull << semanticIndex)) != 0 ? value.yxwz : value;
}

float4 dst(float4 src0, float4 src1)
{
    float4 dest;
    dest.x = 1.0;
    dest.y = src0.y * src1.y;
    dest.z = src0.z;
    dest.w = src1.w;
    return dest;
}

float4 max4(float4 src0)
{
    return max(max(src0.x, src0.y), max(src0.z, src0.w));
}

#ifdef __air__

float2 getPixelCoord(constant Texture2DDescriptorHeap* textureHeap,
                     uint resourceDescriptorIndex,
                     float2 texCoord)
{
    texture2d<float> texture = textureHeap[resourceDescriptorIndex].tex;
    return (float2)getTexture2DDimensions(texture) * texCoord;
}

#else

float2 getPixelCoord(uint resourceDescriptorIndex, float2 texCoord)
{
    return getTexture2DDimensions(g_Texture2DDescriptorHeap[resourceDescriptorIndex]) * texCoord;
}

#endif

float computeMipLevel(float2 pixelCoord)
{
#ifdef __air__
    float2 dx = dfdx(pixelCoord);
    float2 dy = dfdy(pixelCoord);
#else
    float2 dx = ddx(pixelCoord);
    float2 dy = ddy(pixelCoord);
#endif
    float deltaMaxSqr = max(dot(dx, dx), dot(dy, dy));
    return max(0.0, 0.5 * log2(deltaMaxSqr));
}

#ifdef __air__

uint atomicLoadUint(device AtomicUintBuffer* buffer, uint index)
{
    return atomic_load_explicit(&buffer->buffer[index], memory_order_relaxed);
}

uint atomicFetchAddUint(device AtomicUintBuffer* buffer, uint index, uint value)
{
    return atomic_fetch_add_explicit(&buffer->buffer[index], value, memory_order_relaxed);
}

#else

uint atomicLoadUint(RWStructuredBuffer<uint> buffer, uint index)
{
    return buffer[index];
}

uint atomicFetchAddUint(RWStructuredBuffer<uint> buffer, uint index, uint value)
{
    uint originalValue;
    InterlockedAdd(buffer[index], value, originalValue);
    return originalValue;
}

#endif

#endif

#endif
