#include "shader_recompiler.h"
#include "shader_common.h"

#include <cstdio>
#include <set>

static constexpr char SWIZZLES[] = 
{ 
    'x',
    'y', 
    'z', 
    'w', 
    '0', 
    '1',
    '_',
    '_'
};

static constexpr const char* USAGE_TYPES[] =
{
    "float4", // POSITION
    "float4", // BLENDWEIGHT
    "uint4", // BLENDINDICES
    "float4", // NORMAL
    "float4", // PSIZE
    "float4", // TEXCOORD
    "float4", // TANGENT
    "float4", // BINORMAL
    "float4", // TESSFACTOR
    "float4", // POSITIONT
    "float4", // COLOR
    "float4", // FOG
    "float4", // DEPTH
    "float4", // SAMPLE
};

static constexpr const char* USAGE_VARIABLES[] =
{
    "Position",
    "BlendWeight",
    "BlendIndices",
    "Normal",
    "PointSize",
    "TexCoord",
    "Tangent",
    "Binormal",
    "TessFactor",
    "PositionT",
    "Color",
    "Fog",
    "Depth",
    "Sample"
};

static constexpr const char* USAGE_SEMANTICS[] =
{
    "POSITION",
    "BLENDWEIGHT",
    "BLENDINDICES",
    "NORMAL",
    "PSIZE",
    "TEXCOORD",
    "TANGENT",
    "BINORMAL",
    "TESSFACTOR",
    "POSITIONT",
    "COLOR",
    "FOG",
    "DEPTH",
    "SAMPLE"
};

struct DeclUsageLocation
{
    DeclUsage usage;
    uint32_t usageIndex;
    uint32_t location;
};

// NOTE: These are specialized Vulkan locations for Unleashed Recompiled. Change as necessary. Likely not going to work with other games.
static constexpr DeclUsageLocation USAGE_LOCATIONS[] =
{
    { DeclUsage::Position, 0, 0 },
    { DeclUsage::Position, 1, 1 },
    { DeclUsage::Position, 2, 2 },
    { DeclUsage::Position, 3, 3 },
    { DeclUsage::Normal, 0, 4 },
    { DeclUsage::Normal, 1, 5 },
    { DeclUsage::Normal, 2, 6 },
    { DeclUsage::Normal, 3, 7 },
    { DeclUsage::Tangent, 0, 8 },
    { DeclUsage::Tangent, 1, 9 },
    { DeclUsage::Tangent, 2, 10 },
    { DeclUsage::Tangent, 3, 11 },
    { DeclUsage::Binormal, 0, 12 },
    { DeclUsage::TexCoord, 0, 13 },
    { DeclUsage::TexCoord, 1, 14 },
    { DeclUsage::TexCoord, 2, 15 },
    { DeclUsage::TexCoord, 3, 16 },
    { DeclUsage::Color, 0, 17 },
    { DeclUsage::BlendIndices, 0, 18 },
    { DeclUsage::BlendWeight, 0, 19 },
    // Xcompiler: the rest of the native renderer's table (ConvertVertexUsageToLocation in
    // graphics_system.cpp), which must match this one location for location.
    { DeclUsage::PositionT, 0, 0 },
    { DeclUsage::TexCoord, 4, 20 },
    { DeclUsage::TexCoord, 5, 21 },
    { DeclUsage::TexCoord, 6, 22 },
    { DeclUsage::TexCoord, 7, 23 },
    { DeclUsage::TexCoord, 8, 24 },
    { DeclUsage::TexCoord, 9, 25 },
    { DeclUsage::TexCoord, 10, 26 },
    { DeclUsage::TexCoord, 11, 27 },
    { DeclUsage::TexCoord, 12, 28 },
    { DeclUsage::TexCoord, 13, 29 },
    { DeclUsage::TexCoord, 14, 30 },
    { DeclUsage::TexCoord, 15, 31 },
    { DeclUsage::Color, 1, 32 },
    { DeclUsage::Normal, 4, 33 },
    { DeclUsage::Normal, 5, 34 },
    { DeclUsage::Tangent, 4, 35 },
    { DeclUsage::Tangent, 5, 36 },
    { DeclUsage::Binormal, 1, 37 },
    { DeclUsage::Binormal, 2, 38 },
    { DeclUsage::Binormal, 3, 39 },
    // Xcompiler (Halo 3): skinned vertices with a second weight stream (BLENDWEIGHT1, 18 shaders)
    // and a fifth position (POSITION4, 2): "no vertex input location". Same in graphics_system.cpp.
    { DeclUsage::BlendWeight, 1, 40 },
    { DeclUsage::BlendWeight, 2, 41 },
    { DeclUsage::BlendWeight, 3, 42 },
    { DeclUsage::BlendIndices, 1, 43 },
    { DeclUsage::BlendIndices, 2, 44 },
    { DeclUsage::BlendIndices, 3, 45 },
    { DeclUsage::Position, 4, 46 },
    { DeclUsage::Position, 5, 47 },
    { DeclUsage::Position, 6, 48 },
    { DeclUsage::Position, 7, 49 },
    // Xcompiler (Halo 3): COLOR2/COLOR3 (3 vertex shaders failed: "no vertex input location").
    { DeclUsage::Color, 2, 50 },
    { DeclUsage::Color, 3, 51 },
};

static constexpr std::pair<DeclUsage, size_t> INTERPOLATORS[] =
{
    { DeclUsage::TexCoord, 0 },
    { DeclUsage::TexCoord, 1 },
    { DeclUsage::TexCoord, 2 },
    { DeclUsage::TexCoord, 3 },
    { DeclUsage::TexCoord, 4 },
    { DeclUsage::TexCoord, 5 },
    { DeclUsage::TexCoord, 6 },
    { DeclUsage::TexCoord, 7 },
    { DeclUsage::TexCoord, 8 },
    { DeclUsage::TexCoord, 9 },
    { DeclUsage::TexCoord, 10 },
    { DeclUsage::TexCoord, 11 },
    { DeclUsage::TexCoord, 12 },
    { DeclUsage::TexCoord, 13 },
    { DeclUsage::TexCoord, 14 },
    { DeclUsage::TexCoord, 15 },
    { DeclUsage::Color, 0 },
    { DeclUsage::Color, 1 },
    // Xcompiler (Halo 3): POSITION1-3 as plain interpolators (2 shaders wrote oPosition2: "no member
    // named"). Last, so the others keep their places (and their SPIR-V locations).
    { DeclUsage::Position, 1 },
    { DeclUsage::Position, 2 },
    { DeclUsage::Position, 3 }
};

static constexpr std::string_view TEXTURE_DIMENSIONS[] = 
{
    "2D",
    "2DArray",
    "Cube" 
};

static FetchDestinationSwizzle getDestSwizzle(uint32_t dstSwizzle, uint32_t index)
{
    return FetchDestinationSwizzle((dstSwizzle >> (index * 3)) & 0x7);
}

uint32_t ShaderRecompiler::printDstSwizzle(uint32_t dstSwizzle, bool operand)
{
    uint32_t size = 0;

    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W)
        {
            out += SWIZZLES[operand ? uint32_t(swizzle) : i];
            size++;
        }
    }

    return size;
}

void ShaderRecompiler::printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle)
{
    for (size_t i = 0; i < 4; i++)
    {
        const auto swizzle = getDestSwizzle(dstSwizzle, i);
        if (swizzle == FetchDestinationSwizzle::Zero)
        {
            indent();
            println("r{}.{} = 0.0;", dstRegister, SWIZZLES[i]);
        }
        else if (swizzle == FetchDestinationSwizzle::One)
        {
            indent();
            println("r{}.{} = 1.0;", dstRegister, SWIZZLES[i]);
        }
    }
}

void ShaderRecompiler::recompile(const VertexFetchInstruction& instr, uint32_t address)
{
    // Xcompiler: XENOS_RECOMP_FETCH_COMMENTS=1 notes each fetch's index register (Xenos starts r0.x
    // at the vertex index; shaders that compute another index fetch other vertices). Comment only.
    static const bool fetchComments = [] {
        const char* value = getenv("XENOS_RECOMP_FETCH_COMMENTS");
        return value && *value == '1';
    }();
    if (fetchComments)
    {
        indent();
        println("// vfetch index r{}.{} const {} stride {} offset {} format {}", uint32_t(instr.srcRegister),
                "xyzw"[instr.srcSwizzle & 3], uint32_t(instr.constIndex * 3 + instr.constIndexSelect),
                uint32_t(instr.stride), uint32_t(instr.offset), uint32_t(instr.format));
    }
    if (!isPixelShader &&
        (instr.srcRegister != 0 || instr.srcRegisterAm || (instr.srcSwizzle & 3) != 0 || vertexIndexWritten))
        computedVertexFetch = true;
    if (instr.dstRegister == 0 && (instr.dstSwizzle & 7) != 7)
        vertexIndexWritten = true;

    // Xcompiler (Halo 3): the fetch's slot, index register and stride (a mini fetch reuses the
    // previous full fetch's), for the raw read of computed-index shaders.
    uint32_t fetchSlot, fetchStride;
    bool fetchRounded;
    std::string fetchIndex;
    if (!instr.isMiniFetch)
    {
        fetchSlot = instr.constIndex * 3 + instr.constIndexSelect;
        fetchStride = instr.stride;
        fetchRounded = instr.isIndexRounded;
        fetchIndex = fmt::format("r{}.{}", uint32_t(instr.srcRegister), "xyzw"[instr.srcSwizzle & 3]);
        lastFetchSlot = fetchSlot;
        lastFetchStride = fetchStride;
        lastFetchRounded = fetchRounded;
        lastFetchIndex = fetchIndex;
        lastFetchIndexRegister = instr.srcRegister;
        lastFetchIndexComponent = instr.srcSwizzle & 3;
    }
    else
    {
        fetchSlot = lastFetchSlot;
        fetchStride = lastFetchStride;
        fetchRounded = lastFetchRounded;
        fetchIndex = lastFetchIndex;
    }
    if (!isPixelShader && fetchSlot < 96)
        vertexFetchSlots[fetchSlot / 32] |= 1u << (fetchSlot % 32);

    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    uint32_t plannedEntry = UINT32_MAX;
    if (!isPixelShader)
    {
        out += "#if XC_COMPUTED_FETCH\n";
        indent();
        print("r{}.", instr.dstRegister);
        const uint32_t rawSize = printDstSwizzle(instr.dstSwizzle, false);
        out += " = ";
        if (rawSize <= 1)
            out += "(float)(";
        else
            print("(float{})(", rawSize);
        if (uint32_t(instr.format) == 0)
        {
            // Xcompiler (Halo 3): an unbound fetch (the container keeps placeholders, slot 95 and
            // format 0, that Xbox 360 D3D patches from the vertex declaration at draw time): read
            // the declaration's element for this input's location.
            uint32_t declLocation = UINT32_MAX;
            const auto declElement = vertexElements.find(address);
            if (declElement != vertexElements.end())
            {
                for (auto& usageLocation : USAGE_LOCATIONS)
                {
                    if (usageLocation.usage == declElement->second.usage &&
                        usageLocation.usageIndex == declElement->second.usageIndex)
                    {
                        declLocation = usageLocation.location;
                        break;
                    }
                }
            }
            // Xcompiler (2026-11-10, performance): a fetch of the patch plan (shader_recompiler.h)
            // reads the renderer's per-patch result when the draw has one (XcPatchFetch).
            const PatchSymbol indexSymbol = lastFetchIndexRegister < 64
                ? patchSymbols[lastFetchIndexRegister][lastFetchIndexComponent] : PatchSymbol();
            if (patchPlanOpen && declLocation < 64 && !instr.isPredicated && !instr.srcRegisterAm &&
                indexSymbol.kind != 0 && patchPlanCount < kPatchPlanCapacity && instr.dstRegister < 64)
            {
                plannedEntry = patchPlanCount++;
                patchPlan[plannedEntry] = declLocation | (uint32_t(indexSymbol.kind) << 6) |
                    (uint32_t(indexSymbol.argument) << 8) | (uint32_t(indexSymbol.component) << 16) |
                    (fetchRounded ? 1u << 18 : 0u) | 0x80000000u;
                print("XcPatchFetch({}u, {}u, {}, {}, xcDecl{})", plannedEntry, declLocation, fetchIndex,
                    fetchRounded ? "true" : "false", declLocation);
                declLocationsUsed |= uint64_t(1) << declLocation;
            }
            else if (declLocation < 64)
            {
                print("XcFetchVertexDecl({}u, {}, {}, xcDecl{})", declLocation, fetchIndex,
                    fetchRounded ? "true" : "false", declLocation);
                declLocationsUsed |= uint64_t(1) << declLocation;
            }
            else
            {
                print("XcFetchVertexDecl({}u, {}, {}, 0u)", declLocation, fetchIndex, fetchRounded ? "true" : "false");
            }
        }
        else
        {
            print("XcFetchVertex({}u, {}, {}, {}, {}u, {}u, {}, {}, {})", fetchSlot, fetchIndex,
                fetchRounded ? "true" : "false", int32_t(instr.offset), fetchStride, uint32_t(instr.format),
                instr.formatCompAll ? "true" : "false", instr.numFormatAll ? "true" : "false",
                int32_t(instr.expAdjust));
        }
        out += ").";
        printDstSwizzle(instr.dstSwizzle, true);
        out += ";\n";
        out += "#else\n";
    }

    indent();
    print("r{}.", instr.dstRegister);
    uint32_t size = printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";

    if (size <= 1)
        out += "(float)(";
    else
        print("(float{})(", size);

    auto findResult = vertexElements.find(address);
    assert(findResult != vertexElements.end());

    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
        print("swapFloats(g_SwappedNormals, ");
        break;
    case DeclUsage::Tangent:
        print("swapFloats(g_SwappedTangents, ");
        break;
    case DeclUsage::Binormal:
        print("swapFloats(g_SwappedBinormals, ");
        break;
    case DeclUsage::BlendWeight:
        print("swapFloats(g_SwappedBlendWeights, ");
        break;
    case DeclUsage::TexCoord:
        print("swapFloats(g_SwappedTexcoords, ");
        break;
    }

    print("(input.i{}{})", USAGE_VARIABLES[uint32_t(findResult->second.usage)], uint32_t(findResult->second.usageIndex));

    switch (findResult->second.usage)
    {
    case DeclUsage::Normal:
    case DeclUsage::Tangent:
    case DeclUsage::Binormal:
    case DeclUsage::BlendWeight:
    case DeclUsage::TexCoord:
        print(", {})", uint32_t(findResult->second.usageIndex));
        break;
    }

    out += ").";
    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";
    if (!isPixelShader)
        out += "#endif\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    // The patch plan: what the fetch left in its destination. A planned fetch's components are
    // known (a later fetch may use one as its index); any other fetch closes the plan.
    if (plannedEntry != UINT32_MAX)
    {
        for (uint32_t i = 0; i < 4; i++)
        {
            const auto swizzle = getDestSwizzle(instr.dstSwizzle, i);
            if (swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W)
                patchSymbols[instr.dstRegister][i] = { 3, uint8_t(plannedEntry), uint8_t(swizzle) };
            else if (swizzle != FetchDestinationSwizzle::Keep)
                patchSymbols[instr.dstRegister][i] = {};
        }
    }
    else
    {
        closePatchPlan();
    }

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

// Xcompiler (2026-11-10, performance): see the patch plan in shader_recompiler.h.
void ShaderRecompiler::patchPlanAfterAlu(size_t outBegin)
{
    if (!patchPlanOpen)
        return;
    // Exactly one line: "<tabs>rD.c = (float)((r1.x + XcConstant(K).j));" with r1.x still the
    // patch index. Anything else writes registers the plan does not follow.
    const std::string_view text(out.data() + outBegin, out.size() - outBegin);
    unsigned dstRegister = 0, constantRegister = 0;
    char dstComponent = 0, constantComponent = 0;
    int consumed = 0;
    size_t start = 0;
    while (start < text.size() && text[start] == '\t')
        ++start;
    const std::string line(text.substr(start));
    static constexpr std::string_view components = "xyzw";
    if (patchSymbols[1][0].kind == 1 &&
        sscanf(line.c_str(), "r%u.%c = (float)((r1.x + XcConstant(%u).%c));\n%n", &dstRegister,
            &dstComponent, &constantRegister, &constantComponent, &consumed) == 4 &&
        consumed == int(line.size()) && dstRegister < 64 && constantRegister < 256 &&
        components.find(dstComponent) != std::string_view::npos &&
        components.find(constantComponent) != std::string_view::npos)
    {
        patchSymbols[dstRegister][components.find(dstComponent)] =
            { 2, uint8_t(constantRegister), uint8_t(components.find(constantComponent)) };
        return;
    }
    closePatchPlan();
}

void ShaderRecompiler::recompile(const TextureFetchInstruction& instr, bool bicubic)
{
    // Xcompiler: getGradients (opcode 18) writes the coordinate's screen derivatives; it was dropped
    // with the other non-fetch opcodes, so its destination kept an interpolator (Halo 3's 4-tap HUD
    // icons sampled far outside the icon and came out transparent). xcGetGradients is in
    // xenos_shader_common.h; it needs no texture binding.
    if (instr.opcode == FetchOpcode::GetTextureGradients)
    {
        uint32_t written = 0;
        for (size_t i = 0; i < 4; i++)
        {
            const auto swizzle = getDestSwizzle(instr.dstSwizzle, i);
            written += swizzle >= FetchDestinationSwizzle::X && swizzle <= FetchDestinationSwizzle::W;
        }
        if (instr.isPredicated)
        {
            indent();
            println("if ({}p0)", instr.predCondition ? "" : "!");
            indent();
            out += "{\n";
            ++indentation;
        }
        if (written)
        {
            indent();
            print("r{}.", instr.dstRegister);
            printDstSwizzle(instr.dstSwizzle, false);
            print(" = xcGetGradients(r{}.", instr.srcRegister);
            for (size_t i = 0; i < 2; i++)
                out += SWIZZLES[((instr.srcSwizzle >> (i * 2))) & 0x3];
            out += ").";
            printDstSwizzle(instr.dstSwizzle, true);
            out += ";\n";
        }
        printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);
        if (instr.isPredicated)
        {
            --indentation;
            indent();
            out += "}\n";
        }
        return;
    }

    // Xcompiler: setTexLOD (opcode 24) loads the LOD register that fetches with UseRegisterLOD
    // sample at (Xenia: var_main_tfetch_lod_, src.x). 1050 in Halo 3's shaders; dropped before, so
    // those fetches took the implicit LOD (another mip, undefined inside divergent control flow).
    if (instr.opcode == FetchOpcode::SetTextureLod)
    {
        if (instr.isPredicated)
        {
            indent();
            println("if ({}p0)", instr.predCondition ? "" : "!");
            indent();
            out += "{\n";
            ++indentation;
        }
        indent();
        println("xcTexLod = r{}.{};", uint32_t(instr.srcRegister), SWIZZLES[instr.srcSwizzle & 0x3]);
        if (instr.isPredicated)
        {
            --indentation;
            indent();
            out += "}\n";
        }
        return;
    }

    if (instr.opcode != FetchOpcode::TextureFetch && instr.opcode != FetchOpcode::GetTextureWeights)
    {
        // Xcompiler: the other non-fetch opcodes (computed LOD, set LOD/gradients, border colour
        // fraction) are still not translated; say so in the HLSL and on stderr so they can be found.
        indent();
        println("// Xcompiler: untranslated fetch opcode {} (dst r{}, src r{})", uint32_t(instr.opcode),
                uint32_t(instr.dstRegister), uint32_t(instr.srcRegister));
        fprintf(stderr, "XenosRecomp: untranslated fetch opcode %u\n", uint32_t(instr.opcode));
        return;
    }

    usedTextureMask |= 1u << instr.constIndex;

    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predCondition ? "" : "!");

        indent();
        out += "{\n";
        ++indentation;
    }

    auto printSrcRegister = [&](size_t componentCount)
        {
            print("r{}.", instr.srcRegister);

            for (size_t i = 0; i < componentCount; i++)
                out += SWIZZLES[((instr.srcSwizzle >> (i * 2))) & 0x3];
        };

    std::string constName;
    const char* constNamePtr = nullptr;
#ifdef UNLEASHED_RECOMP
    bool subtractFromOne = false;
#endif

    // Xcompiler: every fetch goes by its fetch constant (s<N>), the slot the native renderer binds
    // and the cache's usedTextureMask names; a vertex shader's constant table numbers its samplers
    // from 0 while its fetches use the vertex slots.
    auto findResult = samplers.end();
    if (findResult != samplers.end())
    {
        constNamePtr = findResult->second;

    #ifdef UNLEASHED_RECOMP
        subtractFromOne = hasMtxPrevInvViewProjection && strcmp(constNamePtr, "sampZBuffer") == 0;
    #endif
    }
    else
    {
        constName = fmt::format("s{}", instr.constIndex);
        constNamePtr = constName.c_str();
    }

#ifdef UNLEASHED_RECOMP
    if (instr.constIndex == 0 && instr.dimension == TextureDimension::Texture2D)
    {
        indent();
        println("pixelCoord = getPixelCoord(");
        println("#ifdef __air__");
        indent();
        println("g_Texture2DDescriptorHeap,");
        println("#endif");
        indent();
        print("{}_Texture2DDescriptorIndex, ", constNamePtr);
        printSrcRegister(2);
        out += ");\n";
    }
#endif

    indent();
    print("r{}.", instr.dstRegister);
    printDstSwizzle(instr.dstSwizzle, false);

    out += " = ";
    switch (instr.opcode)
    {
    case FetchOpcode::TextureFetch:
    {
    #ifdef UNLEASHED_RECOMP
        if (subtractFromOne)
            out += "1.0 - ";
    #endif

        out += "tfetch";
        break;
    }
    case FetchOpcode::GetTextureWeights:
    {
        out += "getWeights";
        break;
    }
    }

    std::string_view dimension;
    uint32_t componentCount = 0;

    switch (instr.dimension)
    {
    case TextureDimension::Texture1D:
        dimension = "1D";
        componentCount = 1;
        break;
    case TextureDimension::Texture2D:
        dimension = "2D";
        componentCount = 2;
        break;
    case TextureDimension::Texture3D:
        dimension = "2DArray";
        componentCount = 3;
        break;
    case TextureDimension::TextureCube:
        dimension = "Cube";
        componentCount = 3;
        break;
    }

    out += dimension;

#ifdef UNLEASHED_RECOMP
    if (bicubic)
        out += "Bicubic";
#endif
    // Xcompiler: the fetch's tx_coord_denorm bit: the coordinates are in texels, not 0-1. Halo 3's
    // lighting pass reads its albedo and normal buffers at the pixel position this way: taken
    // as 0-1 every pixel read the buffers' last texel, and the world came out in flat colours.
    // tfetch<dim>Denorm (xenos_shader_common.h) divides by the texture's size.
    if (instr.opcode == FetchOpcode::TextureFetch && instr.texCoordDenorm &&
        instr.dimension != TextureDimension::TextureCube)
        out += "Denorm";
    // Xcompiler: UseRegisterLOD: sample at the setTexLOD register plus the instruction's LOD bias
    // (signed 7 bits, 1/16 units, as Xenia's lod_bias()), via tfetch<dim>[Denorm]Lod.
    const bool registerLod = instr.opcode == FetchOpcode::TextureFetch && instr.useRegLod;
    // Xcompiler: neither computed nor register LOD: the console samples level 0 plus the
    // instruction's bias, as Xenia's translators do. Only pixel shaders pick a level from the
    // derivatives; vertex shaders read level 0 already.
    const bool fixedLod = instr.opcode == FetchOpcode::TextureFetch && isPixelShader &&
        !instr.useRegLod && !instr.useCompLod;
    const float instructionBias = float(int32_t(instr.lodBias)) / 16.0f;
    if (registerLod || fixedLod)
        out += "Lod";
    println("(");

    println("#ifdef __air__");
    indent();
    println("\tg_Texture{}DescriptorHeap,", dimension);
    indent();
    println("\tg_SamplerDescriptorHeap,");
    println("#endif");

    indent();
    print("\t{0}_Texture{1}DescriptorIndex, {0}_SamplerDescriptorIndex, ", constNamePtr, dimension);
    // Xcompiler (Halo 3): the stage's texture_info (s<N>_TextureInfo, the size of its texture as the
    // renderer bound it): the 1D, 2D and 2D array/3D fetch helpers read it instead of querying the
    // texture's size at every fetch. The cube fetches and getWeights read no size from it.
    if (instr.opcode == FetchOpcode::TextureFetch && instr.dimension != TextureDimension::TextureCube)
        print("{}_TextureInfo, ", constNamePtr);
    printSrcRegister(componentCount);

    // Xcompiler: 1.5/1024 of a texel added to the offset, as Xenia's translators do: coordinates
    // exactly between two texels (a full-screen pass under the half-pixel offset) otherwise
    // point-sample the left/upper one in some columns. UE3's scene depth restore did that and
    // the lights' depth test failed in thin vertical streaks.
    constexpr float roundingOffset = 1.5f / 1024.0f;
    switch (instr.dimension)
    {
        case TextureDimension::Texture2D:
            print(", float2({}, {})", instr.offsetX * 0.5f + roundingOffset, instr.offsetY * 0.5f + roundingOffset);
            break;
        case TextureDimension::Texture3D:
            // Xcompiler: none in Z. The slices are blended in the shader (tfetch2DArray), never
            // point-sampled by the host, so the nudge only moved every fetch 1.5/1024 of a slice off
            // its centre: Halo 3's lightmaps (SH layers read at z = 0 with offsets 0.5-7.5) mixed
            // 0.15 % of the next layer in and had to read it, two texture reads each instead of one.
            print(", float3({}, {}, {})", instr.offsetX * 0.5f + roundingOffset, instr.offsetY * 0.5f + roundingOffset,
                instr.offsetZ * 0.5f);
            break;
    }
    if (registerLod)
    {
        if (instructionBias != 0.0f)
            print(", xcTexLod + {}", instructionBias);
        else
            out += ", xcTexLod";
    }
    else if (fixedLod)
    {
        print(", {:.6f}", instructionBias);
    }

    out += ").";

    printDstSwizzle(instr.dstSwizzle, true);

    out += ";\n";

    printDstSwizzle01(instr.dstRegister, instr.dstSwizzle);

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }
}

void ShaderRecompiler::recompile(const AluInstruction& instr)
{
    if (instr.isPredicated)
    {
        indent();
        println("if ({}p0)", instr.predicateCondition ? "" : "!");

        indent(); 
        out += "{\n";
        ++indentation;
    }

    enum
    {
        VECTOR_0,
        VECTOR_1,
        VECTOR_2,
        SCALAR_0,
        SCALAR_1,
        SCALAR_CONSTANT_0,
        SCALAR_CONSTANT_1
    };

    struct OperationResult
    {
        std::string expression;
        size_t componentCount;
    };

    auto op = [&](size_t operand)
        {
            size_t reg = 0;
            size_t swizzle = 0;
            bool select = true;
            bool negate = false;
            bool abs = false;

            switch (operand)
            {
            case SCALAR_CONSTANT_0:
                reg = instr.src3Register;
                swizzle = instr.src3Swizzle;
                select = false;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            case SCALAR_CONSTANT_1:
                reg = (uint32_t(instr.scalarOpcode) & 1) | (instr.src3Select << 1) | (instr.src3Swizzle & 0x3C);
                swizzle = instr.src3Swizzle;
                select = true;
                negate = instr.src3Negate;
                abs = instr.absConstants;
                break;

            default:
                switch (operand)
                {
                case VECTOR_0:
                    reg = instr.src1Register;
                    swizzle = instr.src1Swizzle;
                    select = instr.src1Select;
                    negate = instr.src1Negate;
                    break;
                case VECTOR_1:
                    reg = instr.src2Register;
                    swizzle = instr.src2Swizzle;
                    select = instr.src2Select;
                    negate = instr.src2Negate;
                    break;
                case VECTOR_2:
                case SCALAR_0:
                case SCALAR_1:
                    reg = instr.src3Register;
                    swizzle = instr.src3Swizzle;
                    select = instr.src3Select;
                    negate = instr.src3Negate;
                    break;
                }

                if (select)
                {
                    abs = (reg & 0x80) != 0;
                    reg &= 0x3F;
                }
                else
                {
                    abs = instr.absConstants;
                }

                break;
            }

            std::string regFormatted;

            if (select)
            {
                regFormatted = fmt::format("r{}", reg);
                if (!isPixelShader && reg == 0 && !vertexIndexWritten)
                    vertexIndexRead = true;
            }
            else
            {
                auto findResult = float4Constants.find(reg);
                if (findResult != float4Constants.end())
                {
                    const char* constantName = reinterpret_cast<const char*>(constantTableData + findResult->second->name);
                    if (findResult->second->registerCount > 1)
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (hasMtxProjection && strcmp(constantName, "g_MtxProjection") == 0)
                        {
                            regFormatted = fmt::format("(iterationIndex == 0 ? mtxProjectionReverseZ[{0}] : mtxProjection[{0}])",
                                reg - findResult->second->registerIndex);
                        }
                        else
                    #endif
                        {
                            regFormatted = fmt::format("{}({}{})", constantName,
                                reg - findResult->second->registerIndex, instr.const0Relative ? (instr.constAddressRegisterRelative ? " + a0" : " + aL") : "");
                        }
                    }
                    else
                    {
                        assert(!instr.const0Relative && !instr.const1Relative);
                        regFormatted = constantName;
                    }
                }
                else if (!literalFloat4Constants.count(reg))
                {
                    // Xcompiler: in no constant table (stripped) and not a literal: by register.
                    regFormatted = fmt::format("XcConstant({}{})", reg,
                        instr.const0Relative ? (instr.constAddressRegisterRelative ? " + a0" : " + aL") : "");
                }
                else
                {
                    assert(!instr.const0Relative && !instr.const1Relative);
                    regFormatted = fmt::format("c{}", reg);
                }
            }

            OperationResult opResult {};

            if (negate)
                opResult.expression += '-';

            if (abs)
                opResult.expression += "abs(";

            opResult.expression += regFormatted;
            opResult.expression += '.';

            switch (operand)
            {
            case VECTOR_0:
            case VECTOR_1:
            case VECTOR_2:
            {
                uint32_t mask;

                switch (instr.vectorOpcode)
                {
                case AluVectorOpcode::Dp2Add:
                    mask = (operand == VECTOR_2) ? 0b1 : 0b11;
                    break;

                case AluVectorOpcode::Dp3:
                    mask = 0b111;
                    break;

                case AluVectorOpcode::Dp4:
                case AluVectorOpcode::Max4:
                    mask = 0b1111;
                    break;

                default:
                    mask = instr.vectorWriteMask != 0 ? instr.vectorWriteMask : 0b1;
                    break;
                }

                for (size_t i = 0; i < 4; i++)
                {
                    if ((mask >> i) & 0x1) {
                        opResult.componentCount++;
                        opResult.expression += SWIZZLES[((swizzle >> (i * 2)) + i) & 0x3];
                    }
                }

                break;
            }

            case SCALAR_0:
            case SCALAR_CONSTANT_0:
                opResult.componentCount = 1;
                opResult.expression += SWIZZLES[((swizzle >> 6) + 3) & 0x3];
                break;

            case SCALAR_1:
            case SCALAR_CONSTANT_1:
                opResult.componentCount = 1;
                opResult.expression += SWIZZLES[swizzle & 0x3];
                break;
            }

            if (abs)
                opResult.expression += ")";

            return opResult;
        };

    // Xcompiler (Halo 3): the pixels a kill discarded (xcKill), for the quad check at the end of the
    // instruction. The clip itself is unchanged.
    auto trackKill = [&](const std::string& condition)
    {
        out += "#ifdef __spirv__\n";
        indent();
        println("xcKill = xcKill || {};", condition);
        out += "#endif\n";
        quadKillPending = true;
    };

    switch (instr.vectorOpcode)
    {
    case AluVectorOpcode::KillEq:
        indent();
        println("clip(any({} == {}) ? -1 : 1);", op(VECTOR_0).expression, op(VECTOR_1).expression);
        trackKill(fmt::format("any({} == {})", op(VECTOR_0).expression, op(VECTOR_1).expression));
        break;

    case AluVectorOpcode::KillGt:
        indent();
        println("clip(any({} > {}) ? -1 : 1);", op(VECTOR_0).expression, op(VECTOR_1).expression);
        trackKill(fmt::format("any({} > {})", op(VECTOR_0).expression, op(VECTOR_1).expression));
        break;

    case AluVectorOpcode::KillGe:
        indent();
        println("clip(any({} >= {}) ? -1 : 1);", op(VECTOR_0).expression, op(VECTOR_1).expression);
        trackKill(fmt::format("any({} >= {})", op(VECTOR_0).expression, op(VECTOR_1).expression));
        break;

    case AluVectorOpcode::KillNe:
        indent();
        println("clip(any({} != {}) ? -1 : 1);", op(VECTOR_0).expression, op(VECTOR_1).expression);
        trackKill(fmt::format("any({} != {})", op(VECTOR_0).expression, op(VECTOR_1).expression));
        break;
    }

    bool closeIfBracket = false;

    std::string_view exportRegister;
    bool vectorRegister = true;
    int memExportDataIndex = -1;

    if (instr.exportData)
    {
        if (isPixelShader)
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::PSColor0:
                exportRegister = "output.oC0";
                break;        
            case ExportRegister::PSColor1:
                exportRegister = "output.oC1";
                break;        
            case ExportRegister::PSColor2:
                exportRegister = "output.oC2";
                break;            
            case ExportRegister::PSColor3:
                exportRegister = "output.oC3";
                break;           
            case ExportRegister::PSDepth:
                exportRegister = "output.oDepth";
                vectorRegister = false;
                break;
            }
        }
        else
        {
            switch (ExportRegister(instr.vectorDest))
            {
            case ExportRegister::VSPosition:
                exportRegister = "output.oPos";

            #ifdef UNLEASHED_RECOMP
                if (hasMtxProjection)
                {
                    indent();
                    out += "if ((g_SpecConstants() & SPEC_CONSTANT_REVERSE_Z) == 0 || iterationIndex == 0)\n";
                    indent();
                    out += "{\n";
                    ++indentation;

                    closeIfBracket = true;
                }
            #endif

                break;

            default:
            {
                // Xcompiler (Halo 3): an export that is no interpolator (32 = eA, 33-37 = eM0-eM4:
                // memory export, which the native renderer does not do) goes to a dummy. Before,
                // the end() iterator was used in release builds and the translation crashed
                // (shader 2C4A1AE49B496D79).
                // Xcompiler (Halo 3): export 63 is the point size (x), edge flag (y) and kill vertex (z);
                // its x becomes the host PointSize of point-list draws (XcPointDiameter).
                auto findResult = interpolators.find(instr.vectorDest);
                static constexpr std::string_view MEMEXPORT_DATA[] = { "xcEM0", "xcEM1", "xcEM2", "xcEM3", "xcEM4" };
                if (findResult != interpolators.end())
                    exportRegister = findResult->second;
                else if (instr.vectorDest == uint32_t(ExportRegister::VSPointSizeEdgeFlagKillVertex))
                    exportRegister = "xcPointSize";
                else if (instr.vectorDest == 32)
                {
                    // Xcompiler (Halo 3): a new export address stores what the previous one gathered.
                    exportRegister = "xcEA";
                    memExportUsed = true;
                    out += "#ifdef __spirv__\n";
                    indent();
                    out += "if (xcEMWritten != 0u) { XcMemExport(xcEA, xcEM0, xcEM1, xcEM2, xcEM3, xcEM4, xcEMWritten); xcEMWritten = 0u; }\n#endif\n";
                }
                else if (instr.vectorDest >= 33 && instr.vectorDest <= 37)
                {
                    exportRegister = MEMEXPORT_DATA[instr.vectorDest - 33];
                    memExportDataIndex = int(instr.vectorDest - 33);
                    memExportUsed = true;
                }
                else
                    exportRegister = "xcMemExport";
                break;
            }
            }
        }
    }

    if (instr.vectorOpcode >= AluVectorOpcode::SetpEqPush && instr.vectorOpcode <= AluVectorOpcode::SetpGePush)
    {
        indent();
        print("p0 = {} == 0.0 && {} ", op(VECTOR_0).expression, op(VECTOR_1).expression);

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::SetpEqPush:
            out += "==";
            break;
        case AluVectorOpcode::SetpNePush:
            out += "!=";
            break;
        case AluVectorOpcode::SetpGtPush:
            out += ">";
            break;
        case AluVectorOpcode::SetpGePush:
            out += ">=";
            break;
        }

        out += " 0.0;\n";
    }
    else if (instr.vectorOpcode >= AluVectorOpcode::MaxA)
    {
        indent();
        println("a0 = (int)clamp(floor(({}).w + 0.5), -256.0, 255.0);", op(VECTOR_0).expression);
    }

    uint32_t vectorWriteMask = instr.vectorWriteMask;
    if (instr.exportData)
        vectorWriteMask &= ~instr.scalarWriteMask;

    if (vectorWriteMask != 0)
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            if (vectorRegister)
                out += '.';
        }
        else
        {
            print("r{}.", instr.vectorDest);
            if (instr.vectorDest == 0 && (vectorWriteMask & 0x1))
                vertexIndexWritten = true;
        }

        uint32_t vectorWriteSize = 0;

        for (size_t i = 0; i < 4; i++)
        {
            if ((vectorWriteMask >> i) & 0x1)
            {
                if (vectorRegister)
                    out += SWIZZLES[i];
                vectorWriteSize++;
            }
        }

        out += " = ";

        if (vectorWriteSize > 1)
            print("(float{})((", vectorWriteSize);
        else
            out += "(float)((";

        if (instr.vectorSaturate)
            out += "saturate(";

        size_t operationResultComponentCount;

        switch (instr.vectorOpcode)
        {
        case AluVectorOpcode::Add:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} + {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Mul:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} * {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Max:
        case AluVectorOpcode::MaxA:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("max({}, {})", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Min:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("min({}, {})", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Seq:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} == {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Sgt:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} > {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Sge:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} >= {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Sne:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                operationResultComponentCount = std::max(v0.componentCount, v1.componentCount);

                print("{} != {}", v0.expression, v1.expression);
                break;
            }

        case AluVectorOpcode::Frc:
            {
                auto v0 = op(VECTOR_0);
                operationResultComponentCount = v0.componentCount;

                print("frac({})", v0.expression);
                break;
            }

        case AluVectorOpcode::Trunc:
            {
                auto v0 = op(VECTOR_0);
                operationResultComponentCount = v0.componentCount;

                print("trunc({})", v0.expression);
                break;
            }

        case AluVectorOpcode::Floor:
            {
                auto v0 = op(VECTOR_0);
                operationResultComponentCount = v0.componentCount;

                print("floor({})", v0.expression);
                break;
            }

        case AluVectorOpcode::Mad:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                auto v2 = op(VECTOR_2);
                operationResultComponentCount = std::max(std::max(v0.componentCount, v1.componentCount), v2.componentCount);

                print("{} * {} + {}", v0.expression, v1.expression, v2.expression);
                break;
            }

        case AluVectorOpcode::CndEq:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                auto v2 = op(VECTOR_2);
                operationResultComponentCount = std::max(v1.componentCount, v2.componentCount);

                print("selectWrapper({} == 0.0, {}, {})", v0.expression, v1.expression, v2.expression);
                break;
            }

        case AluVectorOpcode::CndGe:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                auto v2 = op(VECTOR_2);
                operationResultComponentCount = std::max(v1.componentCount, v2.componentCount);

                print("selectWrapper({} >= 0.0, {}, {})", v0.expression, v1.expression, v2.expression);
                break;
            }

        case AluVectorOpcode::CndGt:
            {
                auto v0 = op(VECTOR_0);
                auto v1 = op(VECTOR_1);
                auto v2 = op(VECTOR_2);
                operationResultComponentCount = std::max(v1.componentCount, v2.componentCount);

                print("selectWrapper({} > 0.0, {}, {})", v0.expression, v1.expression, v2.expression);
                break;
            }

        case AluVectorOpcode::Dp4:
        case AluVectorOpcode::Dp3:
            operationResultComponentCount = 1;
            print("dot({}, {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;

        case AluVectorOpcode::Dp2Add:
            {
                auto v2 = op(VECTOR_2);
                operationResultComponentCount = v2.componentCount;

                print("dot({}, {}) + {}", op(VECTOR_0).expression, op(VECTOR_1).expression, v2.expression);
                break;
            }

        case AluVectorOpcode::Cube:
            operationResultComponentCount = 4;
            print("cube({})", op(VECTOR_0).expression);
            break;

        case AluVectorOpcode::Max4:
            operationResultComponentCount = 4;
            print("max4({})", op(VECTOR_0).expression);
            break;

        case AluVectorOpcode::SetpEqPush:
        case AluVectorOpcode::SetpNePush:
        case AluVectorOpcode::SetpGtPush:
        case AluVectorOpcode::SetpGePush:
            {
                auto v0 = op(VECTOR_0);
                operationResultComponentCount = v0.componentCount;

                print("p0 ? 0.0 : {} + 1.0", v0.expression);
                break;
            }

        case AluVectorOpcode::KillEq:
            operationResultComponentCount = 1;
            print("any({} == {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;

        case AluVectorOpcode::KillGt:
            operationResultComponentCount = 1;
            print("any({} > {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;

        case AluVectorOpcode::KillGe:
            operationResultComponentCount = 1;
            print("any({} >= {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;

        case AluVectorOpcode::KillNe:
            operationResultComponentCount = 1;
            print("any({} != {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;

        case AluVectorOpcode::Dst:
            operationResultComponentCount = 4;
            print("dst({}, {})", op(VECTOR_0).expression, op(VECTOR_1).expression);
            break;
        }

		out += ")";

        if (operationResultComponentCount > vectorWriteSize) {
            if (vectorWriteSize == 1) {
                out += ".x";
            } else if (vectorWriteSize == 2) {
                out += ".xy";
            } else if (vectorWriteSize == 3) {
                out += ".xyz";
            }
        }

        out += ")";

        if (instr.vectorSaturate)
            out += ')';

        out += ";\n";
    }

    if (instr.scalarOpcode != AluScalarOpcode::RetainPrev)
    {
        if (instr.scalarOpcode >= AluScalarOpcode::SetpEq && instr.scalarOpcode <= AluScalarOpcode::SetpRstr)
        {
            indent();
            out += "p0 = ";

            switch (instr.scalarOpcode)
            {
            case AluScalarOpcode::SetpEq:
                print("{} == 0.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpNe:
                print("{} != 0.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpGt:
                print("{} > 0.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpGe:
                print("{} >= 0.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpInv:
                print("{} == 1.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpPop:
                print("{} - 1.0 <= 0.0", op(SCALAR_0).expression);
                break;

            case AluScalarOpcode::SetpClr:
                out += "false";
                break;

            case AluScalarOpcode::SetpRstr:
                print("{} == 0.0", op(SCALAR_0).expression);
                break;
            }

            out += ";\n";
        }

        indent();
        out += "ps = ";
        if (instr.scalarSaturate)
            out += "saturate((float)(";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::Adds:
            print("{} + {}", op(SCALAR_0).expression, op(SCALAR_1).expression);
            break;

        case AluScalarOpcode::AddsPrev:
            print("{} + ps", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Muls:
            print("{} * {}", op(SCALAR_0).expression, op(SCALAR_1).expression);
            break;

        case AluScalarOpcode::MulsPrev:
        case AluScalarOpcode::MulsPrev2:
            print("{} * ps", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Maxs:
        case AluScalarOpcode::MaxAs:
        case AluScalarOpcode::MaxAsf:
            print("max({}, {})", op(SCALAR_0).expression, op(SCALAR_1).expression);
            break;

        case AluScalarOpcode::Mins:
            print("min({}, {})", op(SCALAR_0).expression, op(SCALAR_1).expression);
            break;

        case AluScalarOpcode::Seqs:
            print("{} == 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Sgts:
            print("{} > 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Sges:
            print("{} >= 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Snes:
            print("{} != 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Frcs:
            print("frac({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Truncs:
            print("trunc({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Floors:
            print("floor({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Exp:
            print("exp2({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Logc:
        case AluScalarOpcode::Log:
            print("xcClampInf(log2({}))", op(SCALAR_0).expression);
            break;

        // The _ff forms turn an infinite result into zero (1/0 -> 0), not FLT_MAX.
        case AluScalarOpcode::Rcpf:
            print("xcInfToZero(rcp({}))", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Rcpc:
        case AluScalarOpcode::Rcp:
            print("xcClampInf(rcp({}))", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Rsqf:
            print("xcInfToZero(rsqrt({}))", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Rsqc:
        case AluScalarOpcode::Rsq:
            print("xcClampInf(rsqrt({}))", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Subs:
            print("{} - {}", op(SCALAR_0).expression, op(SCALAR_1).expression);
            break;

        case AluScalarOpcode::SubsPrev:
            print("{} - ps", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::SetpEq:
        case AluScalarOpcode::SetpNe:
        case AluScalarOpcode::SetpGt:
        case AluScalarOpcode::SetpGe:
            out += "p0 ? 0.0 : 1.0";
            break;

        case AluScalarOpcode::SetpInv:
            print("p0 ? 0.0 : {0} == 0.0 ? 1.0 : {0}", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::SetpPop:
            print("p0 ? 0.0 : ({} - 1.0)", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::SetpClr:
            out += "FLT_MAX";
            break;

        case AluScalarOpcode::SetpRstr:
            print("p0 ? 0.0 : {}", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::KillsEq:
            print("{} == 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::KillsGt:
            print("{} > 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::KillsGe:
            print("{} >= 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::KillsNe:
            print("{} != 0.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::KillsOne:
            print("{} == 1.0", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Sqrt:
            print("sqrt({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Mulsc0:
        case AluScalarOpcode::Mulsc1:
            print("{} * {}", op(SCALAR_CONSTANT_0).expression, op(SCALAR_CONSTANT_1).expression);
            break;

        case AluScalarOpcode::Addsc0:
        case AluScalarOpcode::Addsc1:
            print("{} + {}", op(SCALAR_CONSTANT_0).expression, op(SCALAR_CONSTANT_1).expression);
            break;

        case AluScalarOpcode::Subsc0:
        case AluScalarOpcode::Subsc1:
            print("{} - {}", op(SCALAR_CONSTANT_0).expression, op(SCALAR_CONSTANT_1).expression);
            break;

        case AluScalarOpcode::Sin:
            print("sin({})", op(SCALAR_0).expression);
            break;

        case AluScalarOpcode::Cos:
            print("cos({})", op(SCALAR_0).expression);
            break;
        }

        if (instr.scalarSaturate)
            out += "))";

        out += ";\n";

        switch (instr.scalarOpcode)
        {
        case AluScalarOpcode::MaxAs:
            indent();
            println("a0 = (int)clamp(floor({} + 0.5), -256.0, 255.0);", op(SCALAR_0).expression);
            break;     
        case AluScalarOpcode::MaxAsf:
            indent();
            println("a0 = (int)clamp(floor({}), -256.0, 255.0);", op(SCALAR_0).expression);
            break;
        }
    }

    uint32_t scalarWriteMask = instr.scalarWriteMask;
    if (instr.exportData)
        scalarWriteMask &= ~instr.vectorWriteMask;

    if (scalarWriteMask != 0)
    {
        indent();
        if (!exportRegister.empty())
        {
            out += exportRegister;
            if (vectorRegister)
                out += '.';
        }
        else
        {
            print("r{}.", instr.scalarDest);
            if (instr.scalarDest == 0 && (scalarWriteMask & 0x1))
                vertexIndexWritten = true;
        }

        for (size_t i = 0; i < 4; i++)
        {
            if (((scalarWriteMask >> i) & 0x1) && vectorRegister)
                out += SWIZZLES[i];
        }

        out += " = ps;\n";
    }

    if (instr.exportData)
    {
        uint32_t zeroMask = instr.scalarDestRelative ? (0b1111 & ~(instr.vectorWriteMask | instr.scalarWriteMask)) : 0;
        uint32_t oneMask = instr.vectorWriteMask & instr.scalarWriteMask;

        for (size_t i = 0; i < 4; i++)
        {
            uint32_t mask = 1 << i;
            if (zeroMask & mask)
            {
                indent();
                println("{}.{} = 0.0;", exportRegister, SWIZZLES[i]);
            }
            else if (oneMask & mask)
            {
                indent();
                println("{}.{} = 1.0;", exportRegister, SWIZZLES[i]);
            }
        }
    }

    if (memExportDataIndex >= 0)
    {
        indent();
        println("xcEMWritten |= {}u;", 1u << memExportDataIndex);
    }

    if (instr.scalarOpcode >= AluScalarOpcode::KillsEq && instr.scalarOpcode <= AluScalarOpcode::KillsOne)
    {
        indent();
        out += "clip(ps != 0.0 ? -1 : 1);\n";
        trackKill("ps != 0.0");
    }

    if (closeIfBracket)
    {
        --indentation;
        indent();
        out += "}\n";
    }

    if (instr.isPredicated)
    {
        --indentation;
        indent();
        out += "}\n";
    }

    // Xcompiler (Halo 3): back in shaderMain's own scope after a kill, a quad whose four pixels are
    // all discarded stops here instead of running the rest of the shader as helpers (XcQuadAllTrue).
    if (quadKillPending && quadKillChecks && indentation == 1)
    {
        quadKillPending = false;
        out += "#ifdef __spirv__\n\tBRANCH if (XcQuadAllTrue(xcKill))\n\t{\n\t\tdiscard;\n\t\treturn output;\n\t}\n#endif\n";
    }
}

// Xcompiler (Halo 3): pixel shaders read all 256 float registers (Halo uses c224-c255;
// the renderer uploads 0x1000 bytes for it). Other titles keep 224.
#if defined(XPORT_TITLE_ID) && (XPORT_TITLE_ID == 0x4D5307E6)
static constexpr uint32_t kXcPixelConstantRegisters = 256;
#else
static constexpr uint32_t kXcPixelConstantRegisters = 224;
#endif

void ShaderRecompiler::recompile(const uint8_t* shaderData, const std::string_view& include)
{
    const auto shaderContainer = reinterpret_cast<const ShaderContainer*>(shaderData);

    assert((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100);

    isPixelShader = (shaderContainer->flags & 0x1) == 0;

    // Xcompiler: the header's pixel shader paths (implicit-LOD SampleBias, the derivatives of
    // getGradients) test XENOS_RECOMP_PIXEL_SHADER, which nothing defined, and it has to come
    // before the header: every pixel shader fetch read mip 0 (distant textures aliased, and the
    // GPU read their largest level), and getGradients gave 0 (Halo 3's antialiased HUD icons
    // took a single tap).
    if (isPixelShader)
        out += "#define XENOS_RECOMP_PIXEL_SHADER 1\n";
    out += include;
    out += '\n';
    computedFetchDefinePos = out.size();

    // Xcompiler: a container without a constant table (offset 0: stripped, as Gears of War 3's
    // are) has none, instead of reading its own header as one.
    alignas(8) static const uint8_t NO_CONSTANT_TABLE[sizeof(ConstantTableContainer)] = {};
    const auto constantTableContainer = shaderContainer->constantTableOffset != NULL
        ? reinterpret_cast<const ConstantTableContainer*>(shaderData + shaderContainer->constantTableOffset)
        : reinterpret_cast<const ConstantTableContainer*>(NO_CONSTANT_TABLE);
    constantTableData = reinterpret_cast<const uint8_t*>(&constantTableContainer->constantTable);

    // Xcompiler: the literal float constants (written as c<N> below), so that any other register
    // the code reads without a constant table entry goes through XcConstant.
    if (shaderContainer->definitionTableOffset != NULL)
    {
        auto definitionTable = reinterpret_cast<const DefinitionTable*>(shaderData + shaderContainer->definitionTableOffset);
        auto definitions = definitionTable->definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Float4Definition*>(definitions);
            for (uint16_t i = 0; i < (definition->count + 3) / 4; i++)
                literalFloat4Constants.emplace(definition->registerIndex + i - (isPixelShader ? 256 : 0), true);
            definitions += 2;
        }
    }

    // Xcompiler: the float constant at a register (relative addressing included), for the ones
    // in no constant table entry.
    const char* constantsName = isPixelShader ? "Pixel" : "Vertex";
    const uint32_t constantsCount = isPixelShader ? kXcPixelConstantRegisters : 256;

    out += "#ifdef __spirv__\n\n";

    println("#define XcConstant(INDEX) selectWrapper(uint(INDEX) < {0}u, vk::RawBufferLoad<float4>(g_PushConstants.{1}ShaderConstants + uint(clamp(int(INDEX), 0, {2})) * 16, 0x10), 0.0)",
        constantsCount, constantsName, constantsCount - 1);
    // Xcompiler (Halo 3): a loop constant the shader does not define, as the game set it (the native
    // renderer copies SHADER_CONSTANT_LOOP_00-31 to its shared constants at 0x420): count in bits
    // 0-7, start (aL) in 8-15, step in 16-23 (signed).
    out += "#define XcUnpackLoop(V) int4(int((V) & 0xFFu), int(((V) >> 8) & 0xFFu), (int((V) << 8) >> 24), 0)\n";
    out += "#define XcLoopConstant(INDEX) XcUnpackLoop(vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 1056 + uint(INDEX) * 4))\n";
    // Xcompiler (Halo 3): the boolean words g_Booleans has no room for (SharedConstants + 720):
    // vertex b0-31, vertex b96-127, pixel b128-159, pixel b224-255.
    out += "#define XcBoolWord(WORD) vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 720 + uint(WORD) * 4)\n";
    if (!isPixelShader)
        out += R"XCFETCH(// Xcompiler (Halo 3): a vertex fetch read from guest memory, for shaders that compute the index
// (instancing: Xenos starts r0.x at the vertex index and lets the shader fetch any record). The
// renderer puts a table of the draw's vertex fetch constants at SharedConstants + 1184: per slot
// the guest data's device address, its size in bytes and the fetch constant's endian swap (128
// slots: 0-95 the fetch constants, 96 + n vertex stream n), then 64 vertex input locations, then
// the same 64 locations resolved (two entries each, see XcFetchVertexDecl): 320 entries, 5120 bytes.
uint XcFetchSwap(uint v, uint endian)
{
    if (endian == 1u) return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
    if (endian == 2u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (endian == 3u) return (v >> 16) | (v << 16);
    return v;
}
float XcFetchComponent(uint bits, uint width, bool isSigned, bool isInteger)
{
    if (isSigned)
    {
        int value = int(bits << (32u - width)) >> (32u - width);
        if (isInteger) return float(value);
        float range = width >= 32u ? 2147483647.0 : float((1u << (width - 1u)) - 1u);
        return max(float(value) / range, -1.0);
    }
    if (isInteger) return float(bits);
    return float(bits) / (width >= 32u ? 4294967295.0 : float((1u << width) - 1u));
}
// Xcompiler (Halo 3, 2026-11-09, performance): the table's address as two 32-bit words. The
// renderer's uniform-buffer rewrite (native_spirv_ubo_shared.h, 32-bit words only) turns this into
// a uniform read the driver has ready before the shader runs; read as one 64-bit value it stayed a
// memory read that every fetch of every vertex waited for.
uint64_t XcFetchTable()
{
    uint2 words = vk::RawBufferLoad<uint2>(g_PushConstants.SharedConstants + 1184, 8);
    return uint64_t(words.x) | (uint64_t(words.y) << 32);
}
// The fetch itself, from a slot's {address low, address high, size, endian}.
float4 XcFetchVertexFrom(uint4 slotEntry, float indexValue, bool rounded, int offsetDwords,
    uint strideDwords, uint format, bool isSigned, bool isInteger, int expAdjust)
{
    float4 result = float4(0.0, 0.0, 0.0, 1.0);
    uint64_t base = uint64_t(slotEntry.x) | (uint64_t(slotEntry.y) << 32);
    uint size = slotEntry.z;
    uint endian = slotEntry.w;
    int index = int(rounded ? floor(indexValue + 0.5) : floor(indexValue));
    int dword = index * int(strideDwords) + offsetDwords;
    // Components, their bit width, and kind: 2 32-bit float, 1 16-bit float, 0 fixed point.
    uint count = 4u, width = 32u, kind = 0u;
    if (format == 6u) width = 8u; // 8_8_8_8
    else if (format == 7u) width = 10u; // 2_10_10_10
    else if (format == 16u || format == 17u) { width = 11u; count = 3u; } // 10_11_11, 11_11_10
    else if (format == 25u || format == 31u) { width = 16u; count = 2u; kind = format == 31u ? 1u : 0u; }
    else if (format == 26u || format == 32u) { width = 16u; kind = format == 32u ? 1u : 0u; }
    else if (format == 33u || format == 36u) { count = 1u; kind = format == 36u ? 2u : 0u; }
    else if (format == 34u || format == 37u) { count = 2u; kind = format == 37u ? 2u : 0u; }
    else if (format == 35u || format == 38u) { kind = format == 38u ? 2u : 0u; }
    else if (format == 57u) { count = 3u; kind = 2u; } // 32_32_32_FLOAT
    else return result;
    uint words = width == 32u ? count : (width == 16u ? (count + 1u) / 2u : 1u);
    if (base == 0 || dword < 0 || (uint(dword) + words) * 4u > size) return result;
    uint64_t address = base + uint64_t(dword) * 4;
    float scale = (kind == 0u && expAdjust != 0) ? exp2(float(expAdjust)) : 1.0;
    // Xcompiler (2026-11-07, performance): each word of the element is read once, before the
    // components (the component loop read it again for every component of the packed formats, and
    // as a [loop] it kept the loads one after another; the water and particle vertex shaders make
    // 10-27 of these fetches per vertex). Same words, same order of components, same values.
    uint4 elementWords = uint4(XcFetchSwap(vk::RawBufferLoad<uint>(address, 4), endian), 0u, 0u, 0u);
    if (words > 1u) elementWords.y = XcFetchSwap(vk::RawBufferLoad<uint>(address + 4u, 4), endian);
    if (words > 2u) elementWords.z = XcFetchSwap(vk::RawBufferLoad<uint>(address + 8u, 4), endian);
    if (words > 3u) elementWords.w = XcFetchSwap(vk::RawBufferLoad<uint>(address + 12u, 4), endian);
    [unroll] for (uint i = 0u; i < 4u; i++)
    {
        if (i >= count) break;
        uint componentWidth = width, shift = 0u, wordIndex = 0u;
        if (width == 32u) wordIndex = i;
        // 16-bit: x is the first in memory (the high half after the swap).
        else if (width == 16u) { wordIndex = i >> 1; shift = (i & 1u) != 0u ? 0u : 16u; }
        else if (format == 6u) shift = 8u * i;
        else if (format == 7u) { shift = 10u * i; componentWidth = i == 3u ? 2u : 10u; }
        else if (format == 16u) { shift = 11u * i; componentWidth = i == 2u ? 10u : 11u; }
        else { shift = i == 0u ? 0u : (i == 1u ? 10u : 21u); componentWidth = i == 0u ? 10u : 11u; }
        uint word = wordIndex == 0u ? elementWords.x : (wordIndex == 1u ? elementWords.y
            : (wordIndex == 2u ? elementWords.z : elementWords.w));
        uint bits = componentWidth >= 32u ? word : (word >> shift) & ((1u << componentWidth) - 1u);
        float value = kind == 2u ? asfloat(bits) : (kind == 1u ? f16tof32(bits)
            : XcFetchComponent(bits, componentWidth, isSigned, isInteger) * scale);
        if (i == 0u) result.x = value;
        else if (i == 1u) result.y = value;
        else if (i == 2u) result.z = value;
        else result.w = value;
    }
    return result;
}
float4 XcFetchVertex(uint slot, float indexValue, bool rounded, int offsetDwords, uint strideDwords,
    uint format, bool isSigned, bool isInteger, int expAdjust)
{
    uint64_t table = XcFetchTable();
    if (table == 0 || slot >= 128u) return float4(0.0, 0.0, 0.0, 1.0);
    // The slot's {address, size, endian} in one load (the table and its entries are 16-byte
    // aligned: AllocateUpload(..., 16) in the renderer).
    uint4 slotEntry = vk::RawBufferLoad<uint4>(table + uint64_t(slot) * 16, 16);
    return XcFetchVertexFrom(slotEntry, indexValue, rounded, offsetDwords, strideDwords, format,
        isSigned, isInteger, expAdjust);
}
// Xcompiler (Halo 3): an unbound fetch, read through the draw's vertex declaration. Per vertex input
// location (after the 128 slots): {slot, offset dwords, stride dwords, format | signed << 8 |
// integer << 9 | swizzle << 16 | valid << 31}; the swizzle (3 bits per component: 0-3 xyzw, 4
// zero, 5 one) is the declaration type's (D3DCOLOR's BGRA, FLOAT3's w = 1).
// Xcompiler (2026-11-09, performance): after those 64, two entries per location with the element
// already resolved by the renderer (cvar xnative_flat_vertex_fetch): its slot's {address low,
// address high, size, endian}, then the element as above. Both depend only on the table, so the
// data waits for one read instead of two (the element, then its slot). Not written (the switch
// off, all zero): the element and its slot, as before.
float XcFetchSelect(float4 v, uint selector)
{
    if (selector == 0u) return v.x;
    if (selector == 1u) return v.y;
    if (selector == 2u) return v.z;
    if (selector == 3u) return v.w;
    return selector == 5u ? 1.0 : 0.0;
}
// Xcompiler (2026-11-10, performance): declared is the location's specialization constant
// (xcDecl<location>): the element's format word (the fourth above) from the pipeline's vertex
// declaration, known when the driver compiles the pipeline, so the format tests below and the
// swizzle fold to the one format's few operations. 0 (not given): the table's word, as before.
float4 XcFetchVertexDecl(uint location, float indexValue, bool rounded, uint declared)
{
    float4 result = float4(0.0, 0.0, 0.0, 1.0);
    uint64_t table = XcFetchTable();
    if (table == 0 || location >= 64u) return result;
    uint4 slotEntry = vk::RawBufferLoad<uint4>(table + uint64_t(192u + 2u * location) * 16, 16);
    uint4 element = vk::RawBufferLoad<uint4>(table + uint64_t(193u + 2u * location) * 16, 16);
    if ((element.w & 0x80000000u) == 0u)
    {
        element = vk::RawBufferLoad<uint4>(table + uint64_t(128u + location) * 16, 16);
        if ((element.w & 0x80000000u) == 0u || element.x >= 128u) return result;
        slotEntry = vk::RawBufferLoad<uint4>(table + uint64_t(element.x) * 16, 16);
    }
    if ((declared & 0x80000000u) != 0u) element.w = declared;
    float4 value = XcFetchVertexFrom(slotEntry, indexValue, rounded, int(element.y), element.z,
        element.w & 0x3Fu, (element.w & 0x100u) != 0u, (element.w & 0x200u) != 0u, 0);
    uint swizzle = (element.w >> 16) & 0xFFFu;
    return float4(XcFetchSelect(value, swizzle & 7u), XcFetchSelect(value, (swizzle >> 3) & 7u),
        XcFetchSelect(value, (swizzle >> 6) & 7u), XcFetchSelect(value, (swizzle >> 9) & 7u));
}
)XCFETCH";
    // Xcompiler (Halo 3): point sprites (the loading screen's point cloud, particles). A point-list
    // pipeline sets SPEC_CONSTANT_POINT_SPRITE: the vertex shader writes the host PointSize from
    // the guest diameter, and the pixel shader's ps_param_gen gets the sprite coordinate in zw
    // (Xenos: y negative for points, z/w the point S/T), from the Vulkan PointCoord.
    out += "#define SPEC_CONSTANT_POINT_SPRITE (1 << 12)\n";
    out += "#define SPEC_CONSTANT_MEMEXPORT (1 << 13)\n";
    // Xcompiler (Halo 3): a GPU without shaderClipDistance (Mali) ignores the user clip plane
    // (water reflection and refraction passes). With SPEC_CONSTANT_CLIP_EMULATION the vertex
    // shader passes the plane distance as an interpolator and the pixel shader discards the
    // fragments behind it.
    out += "#define SPEC_CONSTANT_CLIP_EMULATION (1 << 15)\n";
    if (!isPixelShader)
        out += R"XCPOINT(// SharedConstants + 1200: minimum and maximum diameter (PA_SU_POINT_MINMAX), the constant
// diameter (PA_SU_POINT_SIZE), in guest pixels, and the host pixels per guest pixel.
float XcPointDiameter(float vertexDiameter)
{
    float4 params = vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1200, 4);
    float diameter = vertexDiameter >= 0.0 ? clamp(vertexDiameter, params.x, params.y) : params.z;
    return max(diameter * params.w, 1.0);
}
// Xcompiler (Halo 3): memory export (eA, eM0-eM4: Halo 3's GPU particles). eA is the stream
// constant plus the index: x the base address in dwords (bits 30-31 = 01), y 2^23 + the index of
// eM0, z the format (endian 0-2, format 8-13, signed 16, integer 17, swap red/blue 19), w 2^23 +
// the stream's element count. The renderer keeps the streams in GPU regions, listed at the device
// address in SharedConstants + 1216: {count, 0, 0, 0} then per region {guest base, bytes, device
// address}. The regions hold guest memory's bytes (XcFetchVertex reads them as it reads guest data).
uint XcExportSwap(uint v, uint endian)
{
    if (endian == 1u) return ((v >> 8) & 0x00FF00FFu) | ((v << 8) & 0xFF00FF00u);
    if (endian == 2u || endian >= 4u) return (v >> 24) | ((v >> 8) & 0xFF00u) | ((v << 8) & 0xFF0000u) | (v << 24);
    if (endian == 3u) return (v >> 16) | (v << 16);
    return v;
}
uint XcExportFixed(float value, uint bits, bool isSigned, bool isInteger)
{
    value = value != value ? 0.0 : value;
    uint mask = bits >= 32u ? 0xFFFFFFFFu : ((1u << bits) - 1u);
    if (isSigned)
    {
        float range = float((1u << (bits - 1u)) - 1u);
        float scaled = isInteger ? value : clamp(value, -1.0, 1.0) * range;
        scaled = isInteger ? clamp(scaled, -range - 1.0, range) : scaled;
        return uint(int(scaled + (scaled >= 0.0 ? 0.5 : -0.5))) & mask;
    }
    float range = float(mask);
    float scaled = isInteger ? clamp(value, 0.0, range) : saturate(value) * range;
    return uint(scaled + 0.5) & mask;
}
// The packed words of one element (X in the low bits), and their count (0: not supported).
uint XcExportPack(float4 v, uint format, bool isSigned, bool isInteger, out uint4 words)
{
    words = uint4(0u, 0u, 0u, 0u);
    uint count = 4u, width = 32u, kind = 0u;
    if (format == 6u) width = 8u; // 8_8_8_8
    else if (format == 7u) width = 10u; // 2_10_10_10
    else if (format == 25u || format == 31u) { width = 16u; count = 2u; kind = format == 31u ? 1u : 0u; }
    else if (format == 26u || format == 32u) { width = 16u; kind = format == 32u ? 1u : 0u; }
    else if (format == 36u) { count = 1u; kind = 2u; }
    else if (format == 37u) { count = 2u; kind = 2u; }
    else if (format == 38u) kind = 2u;
    else return 0u;
    [loop] for (uint i = 0u; i < count; i++)
    {
        float c = i == 0u ? v.x : (i == 1u ? v.y : (i == 2u ? v.z : v.w));
        uint componentWidth = (format == 7u && i == 3u) ? 2u : width;
        uint bits = kind == 2u ? asuint(c) : (kind == 1u ? f32tof16(c)
            : XcExportFixed(c, componentWidth, isSigned, isInteger));
        uint wordIndex = width == 32u ? i : (width == 16u ? (i >> 1) : 0u);
        bits <<= width == 16u ? (i & 1u) * 16u : (width == 32u ? 0u : i * width);
        if (wordIndex == 0u) words.x |= bits;
        else if (wordIndex == 1u) words.y |= bits;
        else if (wordIndex == 2u) words.z |= bits;
        else words.w |= bits;
    }
    return width == 32u ? count : (width == 16u ? (count + 1u) / 2u : 1u);
}
void XcMemExportElement(uint address, float4 v, uint format, uint endian, bool isSigned, bool isInteger,
    bool swapRedBlue, uint64_t table)
{
    if (swapRedBlue) v = v.zyxw;
    uint4 words;
    uint count = XcExportPack(v, format, isSigned, isInteger, words);
    if (count == 0u) return;
    uint bytes = count * 4u;
    uint regions = min(vk::RawBufferLoad<uint>(table, 4), 32u);
    for (uint r = 0u; r < regions; r++)
    {
        uint64_t entry = table + 16 + uint64_t(r) * 16;
        uint regionBase = vk::RawBufferLoad<uint>(entry, 4);
        uint regionBytes = vk::RawBufferLoad<uint>(entry + 4, 4);
        if (address < regionBase || address - regionBase + bytes > regionBytes) continue;
        uint64_t target = vk::RawBufferLoad<uint64_t>(entry + 8, 8) + uint64_t(address - regionBase);
        // 8in64 and 8in128 also reverse the order of the 32-bit words.
        uint4 ordered = endian == 4u ? words.yxwz : endian == 5u ? words.wzyx : words;
        vk::RawBufferStore<uint>(target, XcExportSwap(ordered.x, endian), 4);
        if (count > 1u) vk::RawBufferStore<uint>(target + 4, XcExportSwap(ordered.y, endian), 4);
        if (count > 2u)
        {
            vk::RawBufferStore<uint>(target + 8, XcExportSwap(ordered.z, endian), 4);
            vk::RawBufferStore<uint>(target + 12, XcExportSwap(ordered.w, endian), 4);
        }
        return;
    }
}
void XcMemExport(float4 eA, float4 eM0, float4 eM1, float4 eM2, float4 eM3, float4 eM4, uint written)
{
    uint4 a = asuint(eA);
    if (any((a >> uint4(30u, 23u, 23u, 23u)) != uint4(1u, 0x96u, 0x96u, 0x96u))) return;
    uint64_t table = vk::RawBufferLoad<uint64_t>(g_PushConstants.SharedConstants + 1216, 8);
    if (table == 0) return;
    uint info = a.z;
    uint format = (info >> 8) & 0x3Fu;
    uint endian = info & 7u;
    bool isSigned = (info & (1u << 16)) != 0u;
    bool isInteger = (info & (1u << 17)) != 0u;
    bool swapRedBlue = (info & (1u << 19)) != 0u;
    uint index = a.y & 0x7FFFFFu;
    uint elements = a.w & 0x7FFFFFu;
    uint base = (a.x << 2) & 0x1FFFFFFFu;
    uint elementBytes = (format == 26u || format == 32u || format == 37u) ? 8u : format == 38u ? 16u : 4u;
    float4 data[5] = { eM0, eM1, eM2, eM3, eM4 };
    [loop] for (uint i = 0u; i < 5u; i++)
    {
        if ((written & (1u << i)) == 0u || index + i >= elements) continue;
        XcMemExportElement(base + (index + i) * elementBytes, data[i], format, endian, isSigned, isInteger,
            swapRedBlue, table);
    }
}
)XCPOINT";
    else
        out += "[[vk::ext_builtin_input(16)]] static const float2 xcPointCoord;\n";
    // Xcompiler (Halo 3): adaptive triangle patches (the water, DrawIndexedTessellatedPrimitive)
    // have no host tessellation stage: the renderer draws N x N sub-triangles per patch as a
    // triangle list (SPEC_CONSTANT_TESSELLATION, N in bits 16-20) and the vertex shader rebuilds
    // what Xenos gives a patch-indexed domain shader from the vertex index: r0.xyz the domain
    // location in zyx order, r1.x the patch index (Xenia's kTriangleDomainPatchIndexed).
    out += "#define SPEC_CONSTANT_TESSELLATION (1 << 14)\n";
    if (!isPixelShader)
        out += R"XCTESS(// Xcompiler (2026-11-10, performance): the patch records (specialization bit 22, the renderer's
// xnative_tess_patch_records). The shader's leading vertex fetches depend only on the patch index
// (its control point indices, then the control points: 25 fetches in the water's shader), so
// the renderer runs them once per patch and leaves the results, one float4 per fetch in the
// shader's order, in a buffer: entry 320 of the fetch table is {address low, address high, bytes
// per patch, first patch}. A vertex then reads each result with one load instead of decoding the
// vertex format again (27 of every 28 vertex shader runs of an N = 6 patch repeated that work).
static uint64_t xcPatchRecord = 0;
float4 XcPatchFetch(uint entry, uint location, float indexValue, bool rounded, uint declared)
{
    if ((g_SpecConstants() & (1u << 22)) != 0u)
        return vk::RawBufferLoad<float4>(xcPatchRecord + uint64_t(entry * 16u), 16);
    return XcFetchVertexDecl(location, indexValue, rounded, declared);
}
void XcTessellatedPatchInputs(uint vertexId, inout float4 r0, inout float4 r1)
{
    uint segments = max((g_SpecConstants() >> 16) & 31u, 1u);
    uint patchIndex;
    uint2 p;
    if ((g_SpecConstants() & (1u << 21)) != 0u)
    {
        // Xcompiler (2026-11-10, performance): dense vertex indices (the renderer's indexed patch
        // draw, xnative_tess_dense_vertices). The vertex index is patch * (N+1)(N+2)/2 + point,
        // the grid points row by row: x = 0..N, y = 0..N-x. With the sub-triangle corner as the
        // index, the 28 points of an N = 6 patch were spread over 92 indices and a GPU that
        // shades vertices in blocks of consecutive indices (Mali) shaded 2-3.5 times too many.
        // Same point, so the same weights below.
        uint perPatch = (segments + 1u) * (segments + 2u) / 2u;
        patchIndex = vertexId / perPatch;
        uint local = vertexId - patchIndex * perPatch;
        uint x = 0u;
        uint rowLength = segments + 1u;
        for (uint row = 0u; row < segments; row++)
        {
            if (local >= rowLength)
            {
                local -= rowLength;
                rowLength -= 1u;
                x += 1u;
            }
        }
        p = uint2(x, local);
    }
    else
    {
        uint perPatch = segments * segments * 3u;
        patchIndex = vertexId / perPatch;
        uint local = vertexId - patchIndex * perPatch;
        uint cell = local / 3u;
        uint corner = local - cell * 3u;
        uint a = cell / segments;
        uint b = cell - a * segments;
        if (a + b < segments)
        {
            // Upward sub-triangle: (a,b) (a+1,b) (a,b+1).
            p = corner == 0u ? uint2(a, b) : (corner == 1u ? uint2(a + 1u, b) : uint2(a, b + 1u));
        }
        else
        {
            // The other half of the N x N square maps onto the downward sub-triangles:
            // (a2+1,b2) (a2+1,b2+1) (a2,b2+1) with a2 = N-1-a, b2 = N-1-b.
            uint a2 = segments - 1u - a;
            uint b2 = segments - 1u - b;
            p = corner == 0u ? uint2(a2 + 1u, b2) : (corner == 1u ? uint2(a2 + 1u, b2 + 1u) : uint2(a2, b2 + 1u));
        }
    }
    float scale = 1.0 / float(segments);
    // Weights of control points 0, 1, 2 (exact on the edges, so neighbouring patches meet).
    float3 location = float3(float(p.x), float(p.y), float(segments - p.x - p.y)) * scale;
    r0 = float4(location.zyx, 0.0);
    r1 = float4(float(patchIndex), 0.0, 0.0, 0.0);
    if ((g_SpecConstants() & (1u << 22)) != 0u)
    {
        uint4 records = vk::RawBufferLoad<uint4>(XcFetchTable() + 320u * 16u, 16);
        xcPatchRecord = (uint64_t(records.x) | (uint64_t(records.y) << 32)) +
            uint64_t((patchIndex - records.w) * records.z);
    }
}
)XCTESS";

#ifdef UNLEASHED_RECOMP
    bool isMetaInstancer = false;
    bool hasIndexCount = false;
#endif

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

    #ifdef UNLEASHED_RECOMP
        if (!isPixelShader)
        {
            if (strcmp(constantName, "g_MtxProjection") == 0)
                hasMtxProjection = true;
            else if (strcmp(constantName, "g_InstanceTypes") == 0)
                isMetaInstancer = true;
            else if (strcmp(constantName, "g_IndexCount") == 0)
                hasIndexCount = true;
        }
        else
        {
            if (strcmp(constantName, "g_MtxPrevInvViewProjection") == 0)
                hasMtxPrevInvViewProjection = true;
        }
    #endif

        switch (constantInfo->registerSet)
        {
        case RegisterSet::Float4:
        {
            const char* shaderName = isPixelShader ? "Pixel" : "Vertex";

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? kXcPixelConstantRegisters : 256) - constantInfo->registerIndex;

                println("#define {}(INDEX) selectWrapper((INDEX) < {}, vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + ({} + min(INDEX, {})) * 16, 0x10), 0.0)",
                    constantName, tailCount, shaderName, constantInfo->registerIndex.get(), tailCount - 1);
            }
            else
            {
                println("#define {} vk::RawBufferLoad<float4>(g_PushConstants.{}ShaderConstants + {}, 0x10)",
                    constantName, shaderName, constantInfo->registerIndex * 16);
            }
            
            for (uint16_t j = 0; j < constantInfo->registerCount; j++)
                float4Constants.emplace(constantInfo->registerIndex + j, constantInfo);

            break;
        }

        case RegisterSet::Sampler:
        {
            samplers.emplace(constantInfo->registerIndex, constantName);
            break;
        }

        }
    }

    // Xcompiler: the native renderer's shared constants (NativeSharedConstants in xnative's
    // graphics_system.cpp): for each of its 26 texture slots, descriptor indices in four arrays of
    // 26 (2D, 2D array, 3D, cube), then the samplers'. 1D fetches read the 2D array's entry.
    {
        static constexpr std::pair<const char*, uint32_t> NATIVE_TEXTURE_ARRAYS[] =
            { { "1D", 0 }, { "2D", 0 }, { "2DArray", 104 }, { "3D", 208 }, { "Cube", 312 } };
        for (uint32_t slot = 0; slot < 26; slot++)
        {
            for (const auto& [dimension, offset] : NATIVE_TEXTURE_ARRAYS)
            {
                println("#define s{}_Texture{}DescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
                    slot, dimension, offset + slot * 4);
            }
            println("#define s{}_SamplerDescriptorIndex vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + {})",
                slot, 416 + slot * 4);
            // Xcompiler (Halo 3): texture_info (SharedConstants + 1312 + 16 * slot): (1/width,
            // 1/height, 1/depth, depth) of the slot's texture, one 16-byte register (the renderer's
            // uniform-buffer rewrite reads it from its uniform block).
            println("#define s{}_TextureInfo vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + {})",
                slot, 1312 + slot * 16);
        }
    }

    out += "\n#elif defined(__air__)\n\n";

    println("#define XcConstant(INDEX) selectWrapper(uint(INDEX) < {0}u, (*(reinterpret_cast<device float4*>(g_PushConstants.{1}ShaderConstants + uint(clamp(int(INDEX), 0, {2})) * 16))), 0.0)",
        constantsCount, constantsName, constantsCount - 1);
    out += "#define XcUnpackLoop(V) int4(int((V) & 0xFFu), int(((V) >> 8) & 0xFFu), (int((V) << 8) >> 24), 0)\n";
    out += "#define XcLoopConstant(INDEX) XcUnpackLoop((*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 1056 + uint(INDEX) * 4))))\n";
    out += "#define XcBoolWord(WORD) (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + 720 + uint(WORD) * 4)))\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

    #ifdef UNLEASHED_RECOMP
        if (!isPixelShader)
        {
            if (strcmp(constantName, "g_MtxProjection") == 0)
                hasMtxProjection = true;
            else if (strcmp(constantName, "g_InstanceTypes") == 0)
                isMetaInstancer = true;
            else if (strcmp(constantName, "g_IndexCount") == 0)
                hasIndexCount = true;
        }
        else
        {
            if (strcmp(constantName, "g_MtxPrevInvViewProjection") == 0)
                hasMtxPrevInvViewProjection = true;
        }
    #endif

        switch (constantInfo->registerSet)
        {
        case RegisterSet::Float4:
        {
            const char* shaderName = isPixelShader ? "Pixel" : "Vertex";

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? kXcPixelConstantRegisters : 256) - constantInfo->registerIndex;

                println("#define {}(INDEX) selectWrapper((INDEX) < {}, (*(reinterpret_cast<device float4*>(g_PushConstants.{}ShaderConstants + ({} + min(INDEX, {})) * 16))), 0.0)",
                    constantName, tailCount, shaderName, constantInfo->registerIndex.get(), tailCount - 1);
            }
            else
            {
                println("#define {} (*(reinterpret_cast<device float4*>(g_PushConstants.{}ShaderConstants + {})))",
                    constantName, shaderName, constantInfo->registerIndex * 16);
            }

            for (uint16_t j = 0; j < constantInfo->registerCount; j++)
                float4Constants.emplace(constantInfo->registerIndex + j, constantInfo);

            break;
        }

        case RegisterSet::Sampler:
        {
            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("#define {}_Texture{}DescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + {})))",
                    constantName, TEXTURE_DIMENSIONS[j], j * 64 + constantInfo->registerIndex * 4);
            }

            println("#define {}_SamplerDescriptorIndex (*(reinterpret_cast<device uint*>(g_PushConstants.SharedConstants + {})))",
                constantName, std::size(TEXTURE_DIMENSIONS) * 64 + constantInfo->registerIndex * 4);

            samplers.emplace(constantInfo->registerIndex, constantName);
            break;
        }

        }
    }

    out += "\n#else\n\n";

    println("cbuffer {}ShaderConstants : register(b{}, space4)", isPixelShader ? "Pixel" : "Vertex", isPixelShader ? 1 : 0);
    out += "{\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Float4)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            print("\tfloat4 {}", constantName);

            if (constantInfo->registerCount > 1)
                print("[{}]", constantInfo->registerCount.get());

            println(" : packoffset(c{});", constantInfo->registerIndex.get());

            if (constantInfo->registerCount > 1)
            {
                uint32_t tailCount = (isPixelShader ? kXcPixelConstantRegisters : 256) - constantInfo->registerIndex;
                println("#define {0}(INDEX) selectWrapper((INDEX) < {1}, {0}[min(INDEX, {2})], 0.0)", constantName, tailCount, tailCount - 1);
            }
        }
    }

    // Xcompiler: the registers as an array when no table entry takes them (packoffsets may not
    // overlap); otherwise those reads give 0.
    if (float4Constants.empty())
        println("\tfloat4 XcConstants[{}] : packoffset(c0);", constantsCount);

    out += "};\n\n";

    if (float4Constants.empty())
        println("#define XcConstant(INDEX) selectWrapper(uint(INDEX) < {0}u, XcConstants[clamp(int(INDEX), 0, {1})], 0.0)",
            constantsCount, constantsCount - 1);
    else
        out += "#define XcConstant(INDEX) float4(0.0, 0.0, 0.0, 0.0)\n";
    out += "#define XcLoopConstant(INDEX) int4(0, 0, 1, 0)\n";
    out += "#define XcBoolWord(WORD) 0u\n";

    out += "cbuffer SharedConstants : register(b2, space4)\n";
    out += "{\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Sampler)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);

            for (size_t j = 0; j < std::size(TEXTURE_DIMENSIONS); j++)
            {
                println("\tuint {}_Texture{}DescriptorIndex : packoffset(c{}.{});",
                    constantName, TEXTURE_DIMENSIONS[j], j * 4 + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);
            }

            println("\tuint {}_SamplerDescriptorIndex : packoffset(c{}.{});",
                constantName, 4 * std::size(TEXTURE_DIMENSIONS) + constantInfo->registerIndex / 4, SWIZZLES[constantInfo->registerIndex % 4]);
        }
    }

    out += "\tDEFINE_SHARED_CONSTANTS();\n";
    out += "};\n\n";

    out += "#endif\n";

    for (uint32_t i = 0; i < constantTableContainer->constantTable.constants; i++)
    {
        const auto constantInfo = reinterpret_cast<const ConstantInfo*>(
            constantTableData + constantTableContainer->constantTable.constantInfo + i * sizeof(ConstantInfo));

        if (constantInfo->registerSet == RegisterSet::Bool)
        {
            const char* constantName = reinterpret_cast<const char*>(constantTableData + constantInfo->name);
            println("#define {} (1 << {})", constantName, constantInfo->registerIndex + (isPixelShader ? 16 : 0));
            boolConstants.emplace(constantInfo->registerIndex, constantName);
        }
    }

    out += '\n';

    const auto shader = reinterpret_cast<const Shader*>(shaderData + shaderContainer->shaderOffset);

    println("struct {}", isPixelShader ? "Interpolators" : "VertexShaderInput");
    out += "{\n";

    if (isPixelShader)
    {
        out += "#ifdef __air__\n";

        out += "\tfloat4 iPos [[position]];\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\tfloat4 i{0}{1} [[user({2}{1})]];", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);

        out += "#else\n";

        out += "\tfloat4 iPos : SV_Position;\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\tfloat4 i{0}{1} : {2}{1};", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);

        // Xcompiler (Halo 3): the distances to user clip planes 0-3 and 4-5 (SPEC_CONSTANT_CLIP_EMULATION).
        out += "#ifdef __spirv__\n\tfloat4 xcClipEmulation : XCCLIP0;\n\tfloat2 xcClipEmulation1 : XCCLIP1;\n#endif\n";

        out += "#endif\n";
    }
    else
    {
        auto vertexShader = reinterpret_cast<const VertexShader*>(shader);

        out += "#ifdef __air__\n";

        // Xcompiler: two vfetches of the same usage (a skinning shader's blend weights, Gears of
        // War) read one input, as D3D binds both to the same declaration element: declared once.
        auto firstOfUsage = [vertexShader](uint32_t i)
        {
            const auto* elements = vertexShader->vertexElementsAndInterpolators + vertexShader->field18;
            auto usageOf = [](uint32_t value)
            {
                union
                {
                    VertexElement vertexElement;
                    uint32_t raw;
                };
                raw = value;
                return std::make_pair(uint32_t(vertexElement.usage), uint32_t(vertexElement.usageIndex));
            };
            for (uint32_t j = 0; j < i; j++)
            {
                if (usageOf(elements[j]) == usageOf(elements[i]))
                    return false;
            }
            return true;
        };

        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement vertexElement;
                uint32_t value;
            };

            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];

            if (!firstOfUsage(i))
            {
                vertexElements.emplace(uint32_t(vertexElement.address), vertexElement);
                continue;
            }

            const char* usageType = USAGE_TYPES[uint32_t(vertexElement.usage)];

#ifdef UNLEASHED_RECOMP
            if ((vertexElement.usage == DeclUsage::TexCoord && vertexElement.usageIndex == 2 && isMetaInstancer) ||
                (vertexElement.usage == DeclUsage::Position && vertexElement.usageIndex == 1))
            {
                usageType = "uint4";
            }
#endif

            out += '\t';

            print("{0} i{1}{2}", usageType, USAGE_VARIABLES[uint32_t(vertexElement.usage)],
                uint32_t(vertexElement.usageIndex));

            bool foundUsage = false;
            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (usageLocation.usage == vertexElement.usage && usageLocation.usageIndex == vertexElement.usageIndex)
                {
                    println(" [[attribute({})]];", usageLocation.location);
                    foundUsage = true;
                    break;
                }
            }

            if (!foundUsage) {
                // Xcompiler: this shader fails alone (main.cpp leaves it out of the cache).
                throw std::runtime_error(fmt::format("no vertex input location for {} {}", USAGE_VARIABLES[uint32_t(vertexElement.usage)], uint32_t(vertexElement.usageIndex)));
            }

            vertexElements.emplace(uint32_t(vertexElement.address), vertexElement);
        }

        out += "#else\n";

        for (uint32_t i = 0; i < vertexShader->vertexElementCount; i++)
        {
            union
            {
                VertexElement vertexElement;
                uint32_t value;
            };

            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + i];

            if (!firstOfUsage(i))
                continue;

            const char* usageType = USAGE_TYPES[uint32_t(vertexElement.usage)];

#ifdef UNLEASHED_RECOMP
            if ((vertexElement.usage == DeclUsage::TexCoord && vertexElement.usageIndex == 2 && isMetaInstancer) ||
                (vertexElement.usage == DeclUsage::Position && vertexElement.usageIndex == 1))
            {
                usageType = "uint4";
            }
#endif

            out += '\t';

            for (auto& usageLocation : USAGE_LOCATIONS)
            {
                if (usageLocation.usage == vertexElement.usage && usageLocation.usageIndex == vertexElement.usageIndex)
                {
                    print("[[vk::location({})]] ", usageLocation.location);
                    break;
                }
            }

            println("{0} i{1}{2} : {3}{2};", usageType, USAGE_VARIABLES[uint32_t(vertexElement.usage)],
                uint32_t(vertexElement.usageIndex), USAGE_SEMANTICS[uint32_t(vertexElement.usage)]);
        }

        out += "#endif\n";
    }

    out += "};\n";

    println("struct {}", isPixelShader ? "PixelShaderOutput" : "Interpolators");
    out += "{\n";

    if (isPixelShader)
    {
        out += "#ifdef __air__\n";

        auto pixelShader = reinterpret_cast<const PixelShader*>(shader);
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR0)
            out += "\tfloat4 oC0 [[color(0)]];\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR1)
            out += "\tfloat4 oC1 [[color(1)]];\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR2)
            out += "\tfloat4 oC2 [[color(2)]];\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR3)
            out += "\tfloat4 oC3 [[color(3)]];\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_DEPTH)
            out += "\tfloat oDepth [[depth(any)]];\n";

        out += "#else\n";

        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR0)
            out += "\tfloat4 oC0 : SV_Target0;\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR1)
            out += "\tfloat4 oC1 : SV_Target1;\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR2)
            out += "\tfloat4 oC2 : SV_Target2;\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_COLOR3)
            out += "\tfloat4 oC3 : SV_Target3;\n";
        if (pixelShader->outputs & PIXEL_SHADER_OUTPUT_DEPTH)
            out += "\tfloat oDepth : SV_Depth;\n";

        out += "#endif\n";
    }
    else
    {
        out += "#ifdef __air__\n";

        out += "\tfloat4 oPos [[position]] [[invariant]];\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            print("\tfloat4 o{0}{1} [[user({2}{1})]];\n", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);

        out += "\tfloat clipDistance [[clip_distance]];\n";

        out += "#else\n";

        out += "\tprecise float4 oPos : SV_Position;\n";

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            print("\tfloat4 o{0}{1} : {2}{1};\n", USAGE_VARIABLES[uint32_t(usage)], usageIndex, USAGE_SEMANTICS[uint32_t(usage)]);

        // Xcompiler (Halo 3): the six Xenos user clip planes (PA_CL_UCP_0-5): Vulkan clip distances
        // 0-3 and 4-5, and the same distances as interpolators for a GPU without shaderClipDistance
        // (Mali), whose pixel shader discards against them (SPEC_CONSTANT_CLIP_EMULATION).
        out += "#ifdef __spirv__\n\tfloat4 clipDistance : SV_ClipDistance0;\n\tfloat2 clipDistance1 : SV_ClipDistance1;\n";
        out += "\tfloat4 xcClipEmulation : XCCLIP0;\n\tfloat2 xcClipEmulation1 : XCCLIP1;\n#else\n";
        out += "\tfloat clipDistance : SV_ClipDistance;\n#endif\n";
        out += "#ifdef __spirv__\n\t[[vk::builtin(\"PointSize\")]] float oPointSize : PSIZE;\n#endif\n";

        out += "#endif\n";
    }

    out += "};\n";

    out += "#ifdef __air__\n";

    if (isPixelShader)
        out += "[[fragment]]\n";
    else
        out += "[[vertex]]\n";

    out += "#elif !defined(__spirv__)\n";

    if (isPixelShader)
        out += "[shader(\"pixel\")]\n";
    else
        out += "[shader(\"vertex\")]\n";

    out += "#endif\n";

    println("{} shaderMain(", isPixelShader ? "PixelShaderOutput" : "Interpolators");

    if (isPixelShader)
    {
        out += "#ifdef __air__\n";

        out += "\tInterpolators input [[stage_in]],\n";
        out += "\tbool iFace [[front_facing]],\n";

        out += "\tconstant Texture2DDescriptorHeap* g_Texture2DDescriptorHeap [[buffer(0)]],\n";
        out += "\tconstant Texture2DArrayDescriptorHeap* g_Texture2DArrayDescriptorHeap [[buffer(1)]],\n";
        out += "\tconstant TextureCubeDescriptorHeap* g_TextureCubeDescriptorHeap [[buffer(2)]],\n";
        out += "\tconstant SamplerDescriptorHeap* g_SamplerDescriptorHeap [[buffer(3)]],\n";
#ifdef MARATHON_RECOMP
        out += "\tdevice AtomicUintBuffer* g_ConditionalSurveyBuffer [[buffer(4)]],\n";
#endif
        out += "\tconstant PushConstants& g_PushConstants [[buffer(8)]]\n";

        out += "#else\n";

        out += "\tInterpolators input,\n";

        out += "#ifdef __spirv__\n";
        out += "\tin bool iFace : SV_IsFrontFace\n";
        out += "#else\n";
        out += "\tin uint iFace : SV_IsFrontFace\n";
        out += "#endif\n";

        out += "\n#endif\n";
    }
    else
    {
        out += "#ifdef __air__\n";
        out += "\tconstant PushConstants& g_PushConstants [[buffer(8)]],\n";
        out += "\tVertexShaderInput input [[stage_in]]\n";
        out += "#else\n";
        // Xcompiler (Halo 3): a computed-fetch shader reads no attributes, only the vertex index.
        out += "#if XC_COMPUTED_FETCH\n";
        out += "\tin uint xcVertexId : SV_VertexID\n";
        out += "#else\n";
        out += "\tVertexShaderInput input\n";
        // Xcompiler (Halo 3): the attributes and the vertex index (see vertexIndexRead).
        out += "#if XC_VERTEX_INDEX\n";
        out += "\t, in uint xcVertexId : SV_VertexID\n";
        out += "#endif\n";
        out += "#endif\n";
        out += "#endif\n";

    #ifdef UNLEASHED_RECOMP
        if (hasIndexCount)
        {
            out += "\t,\n";
            out += "#ifdef __air__\n";
            out += "\tuint iVertexId [[vertex_id]],\n";
            out += "\tuint iInstanceId [[instance_id]]\n";
            out += "#else\n";
            out += "\tin uint iVertexId : SV_VertexID,\n";
            out += "\tin uint iInstanceId : SV_InstanceID\n";
            out += "#endif\n";
        }
    #endif
    }

    out += ")\n";
    out += "{\n";

    std::string outputName = isPixelShader ? "PixelShaderOutput" : "Interpolators";

    out += "#ifdef __air__\n";
    println("\t{0} output = {0}{{}};", outputName);
    out += "#else\n";
    println("\t{0} output = ({0})0;", outputName);
    out += "#endif\n";

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
    {
        specConstantsMask |= SPEC_CONSTANT_REVERSE_Z;

        out += "\toutput.oPos = 0.0;\n";

        out += "\tfloat4x4 mtxProjection = float4x4(g_MtxProjection(0), g_MtxProjection(1), g_MtxProjection(2), g_MtxProjection(3));\n";
        out += "\tfloat4x4 mtxProjectionReverseZ = mul(mtxProjection, float4x4(1, 0, 0, 0, 0, 1, 0, 0, 0, 0, -1, 0, 0, 0, 1, 1));\n";

        out += "\tUNROLL for (int iterationIndex = 0; iterationIndex < 2; iterationIndex++)\n";
        out += "\t{\n";
    }
#endif

    // Xcompiler (Halo 3): the loop constants the shader defines itself; the others its loops use are
    // read from what the game set (XcLoopConstant), declared once the control flow is known.
    std::set<uint32_t> literalLoopConstants;
    std::set<uint32_t> usedLoopConstants;

    if (shaderContainer->definitionTableOffset != NULL)
    {
        auto definitionTable = reinterpret_cast<const DefinitionTable*>(shaderData + shaderContainer->definitionTableOffset);
        auto definitions = definitionTable->definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Float4Definition*>(definitions);
            auto value = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + definition->physicalOffset);
            for (uint16_t i = 0; i < (definition->count + 3) / 4; i++)
            {
                println("#ifdef __air__");
                println("\tfloat4 c{} = as_type<float4>(uint4(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}));",
                    definition->registerIndex + i - (isPixelShader ? 256 : 0), value[0].get(), value[1].get(), value[2].get(), value[3].get());
                println("#else");
                println("\tfloat4 c{} = asfloat(uint4(0x{:X}, 0x{:X}, 0x{:X}, 0x{:X}));",
                    definition->registerIndex + i - (isPixelShader ? 256 : 0), value[0].get(), value[1].get(), value[2].get(), value[3].get());
                println("#endif");

                value += 4;
            }
            definitions += 2;
        }
        ++definitions;
        while (*definitions != 0)
        {
            auto definition = reinterpret_cast<const Int4Definition*>(definitions);
            for (uint16_t i = 0; i < definition->count; i++)
            {
                union
                {
                    uint32_t value;
                    struct
                    {
                        // Xcompiler (Halo 3): count and start are unsigned (a count over 127 came
                        // out negative), the step is signed.
                        uint8_t x;
                        uint8_t y;
                        int8_t z;
                        int8_t w;
                    };
                };

                value = definition->values[i].get();

                const uint32_t loopIndex = (definition->registerIndex - 8992) / 4 + i;
                literalLoopConstants.insert(loopIndex);
                println("\tint4 i{} = int4({}, {}, {}, {});",
                    loopIndex, uint32_t(x), uint32_t(y), int32_t(z), int32_t(w));
            }
            definitions += 2;
            definitions += definition->count;
        }

        out += "\n";
    }

    bool printedRegisters[32]{};

    uint32_t interpolatorCount = (shader->interpolatorInfo >> 5) & 0x1F;

    for (uint32_t i = 0; i < interpolatorCount; i++)
    {
        union
        {
            Interpolator interpolator;
            uint32_t value;
        };
    
        if (isPixelShader)
        {
            value = reinterpret_cast<const PixelShader*>(shader)->interpolators[i];
            println("\tfloat4 r{} = input.i{}{};", uint32_t(interpolator.reg), USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex));
            printedRegisters[interpolator.reg] = true;
        }
        else
        {
            auto vertexShader = reinterpret_cast<const VertexShader*>(shader);
            value = vertexShader->vertexElementsAndInterpolators[vertexShader->field18 + vertexShader->vertexElementCount + i];
            interpolators.emplace(i, fmt::format("output.o{}{}", USAGE_VARIABLES[uint32_t(interpolator.usage)], uint32_t(interpolator.usageIndex)));
        }
    }

    if (!isPixelShader)
    {
    #ifdef UNLEASHED_RECOMP
        if (!hasMtxProjection)
            out += "\toutput.oPos = 0.0;\n";
    #endif

        for (auto& [usage, usageIndex] : INTERPOLATORS)
            println("\toutput.o{}{} = 0.0;", USAGE_VARIABLES[uint32_t(usage)], usageIndex);

        out += "\n";
    }

    for (size_t i = 0; i < 32; i++)
    {
        if (!printedRegisters[i])
        {
            print("\tfloat4 r{} = ", i);
            if (isPixelShader && i == ((shader->fieldC >> 8) & 0xFF))
            {
                out += "\n#ifdef __spirv__\n\t\t((g_SpecConstants() & SPEC_CONSTANT_POINT_SPRITE) != 0) ? "
                       "float4(input.iPos.x - 0.5, -(input.iPos.y - 0.5), saturate(xcPointCoord)) :\n#endif\n"
                       "\t\tfloat4((input.iPos.xy - 0.5) * float2(iFace ? 1.0 : -1.0, 1.0), 0.0, 0.0);\n";
            }
            else if (!isPixelShader && i == 0)
            {
                // Xcompiler (Halo 3): Xenos starts r0.x at the vertex index.
                out += "\n#if XC_COMPUTED_FETCH || XC_VERTEX_INDEX\n\t\tfloat4(float(xcVertexId), 0.0, 0.0, 0.0);\n#else\n\t\t0.0;\n#endif\n";
            }
        #ifdef UNLEASHED_RECOMP
            else if (!isPixelShader && hasIndexCount && i == 0)
            {
                out += "float4(iVertexId + g_IndexCount.x * iInstanceId, 0.0, 0.0, 0.0);\n";
            }
        #endif
            else
            {
                out += "0.0;\n";
            }
        }
    }

    out += "\tint a0 = 0;\n";
    out += "\tint aL = 0;\n";
    out += "\tbool p0 = false;\n";
    out += "\tfloat ps = 0.0;\n";
    // Xcompiler: the setTexLOD register (see recompile(TextureFetchInstruction)).
    out += "\tfloat xcTexLod = 0.0;\n";
    if (isPixelShader)
    {
        // Xcompiler (Halo 3): the pixels a kill discarded (see trackKill), and the emulated user clip
        // planes' early exit. Without shaderClipDistance (Mali) the planes are a discard at the end:
        // a dynamic light re-draws the geometry around it clipped to the light's volume, and every
        // pixel outside it ran the whole light shader before being discarded. A quad wholly outside
        // now stops before shading; one with a pixel inside runs as before.
        out += "#ifdef __spirv__\n\tbool xcKill = false;\n"
               "\tBRANCH if ((g_SpecConstants() & SPEC_CONSTANT_CLIP_EMULATION) != 0)\n\t{\n"
               "\t\tBRANCH if (XcQuadAllTrue(any(input.xcClipEmulation < 0.0) || any(input.xcClipEmulation1 < 0.0)))\n\t\t{\n"
               "\t\t\tdiscard;\n\t\t\treturn output;\n\t\t}\n\t}\n#endif\n";
#ifdef UNLEASHED_RECOMP
        out += "\tfloat2 pixelCoord = 0.0;\n";
#endif
#ifdef MARATHON_RECOMP
        specConstantsMask |= SPEC_CONSTANT_CONDITIONAL_RENDERING;

        out += "\tBRANCH if ((g_SpecConstants() & SPEC_CONSTANT_CONDITIONAL_RENDERING))\n";
        out += "\t{\n";

        out += "\t\tuint sampleCount = atomicLoadUint(g_ConditionalSurveyBuffer, g_conditionalSurveyIndex);\n";
        out += "\t\tBRANCH if (sampleCount == 0)\n";
        out += "\t\t{\n";

        println("#ifdef __air__");
        println("\t\t\tdiscard_fragment();");
        println("#else");
        println("\t\t\tdiscard;");
        println("#endif");

        out += "\t\t}\n";

        out += "\t}\n";
#endif
    }

    const be<uint32_t>* code = reinterpret_cast<const be<uint32_t>*>(shaderData + shaderContainer->virtualSize + shader->physicalOffset);

    // Xcompiler: forward conditional jumps as flags. With braces, a jump over an else block
    // (cjmp to B if !p0; A; cjmp to C if p0; B; C) put B inside A's block, where it never ran,
    // and cond_exec_pred blocks ran whatever the predicate. On for Gears of War 3's build (and
    // with XENOSRECOMP_JUMP_FLAGS=1), off otherwise, so that other titles translate as before.
#if defined(XPORT_TITLE_ID) && (XPORT_TITLE_ID == 0x4D5308AB || XPORT_TITLE_ID == 0x4D5307E6)
    const bool useJumpFlags = true;
#else
    const bool useJumpFlags = [] {
        const char* value = std::getenv("XENOSRECOMP_JUMP_FLAGS");
        return value && value[0] == '1';
    }();
#endif
    struct ForwardJump
    {
        uint32_t pc;
        uint32_t target;
    };
    std::vector<ForwardJump> forwardJumps;
    uint32_t firstPassPc = 0;
    // Xcompiler (Halo 3): the loops (LoopStart and LoopEnd control flow indices), to tell which
    // forward jumps stay inside a loop body, leave one (a break) or enter one.
    struct LoopRange
    {
        uint32_t start;
        uint32_t end;
    };
    std::vector<LoopRange> loopRanges;
    std::vector<uint32_t> openLoops;
    bool loopsNested = true;
    std::set<uint32_t> breakJumps;

    union
    {
        ControlFlowInstruction controlFlow[2];
        struct
        {
            uint32_t code0;
            uint32_t code1;
            uint32_t code2;
            uint32_t code3;
        };
    };

    auto controlFlowCode = code;
    uint32_t instrAddress = 0;
    uint32_t instrSize = shader->size;
    bool simpleControlFlow = true;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            uint32_t address = 0;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                break;

            case ControlFlowOpcode::LoopStart:
                usedLoopConstants.insert(uint32_t(cfInstr.loopStart.loopId));
                openLoops.push_back(firstPassPc);
                break;

            case ControlFlowOpcode::LoopEnd:
                usedLoopConstants.insert(uint32_t(cfInstr.loopEnd.loopId));
                if (!openLoops.empty())
                {
                    loopRanges.push_back({openLoops.back(), firstPassPc});
                    openLoops.pop_back();
                }
                else
                    loopsNested = false;
                break;

            case ControlFlowOpcode::CondJmp:
            {
                // Xcompiler (Halo 3): with the jump flags, an unconditional forward jump (the jump
                // over an else block) is a flag that is always set, so the shader stays structured.
                // Before, it made the shader a `while (true) switch (pc)` state machine, which the
                // Mali compiler took minutes on (the intro and menus drew nothing meanwhile).
                const bool backward = cfInstr.condJmp.direction ||
                    (useJumpFlags && cfInstr.condJmp.address <= firstPassPc);
                if ((cfInstr.condJmp.isUnconditional && !useJumpFlags) || backward)
                    simpleControlFlow = false;
                else
                {
                    ++ifEndLabels[cfInstr.condJmp.address];
                    forwardJumps.push_back({firstPassPc, uint32_t(cfInstr.condJmp.address)});
                }

                break;
            }
            }

            if (address != 0)
                instrSize = std::min<uint32_t>(instrSize, address * 12);
            ++firstPassPc;
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    // Xcompiler (Halo 3): a forward jump out of one loop is a flag plus a break; one into a loop,
    // or out of two, keeps the state machine.
    if (useJumpFlags && simpleControlFlow)
    {
        if (!loopsNested || !openLoops.empty())
            simpleControlFlow = false;
        for (const ForwardJump& jump : forwardJumps)
        {
            uint32_t exits = 0;
            for (const LoopRange& loop : loopRanges)
            {
                const bool fromInside = loop.start < jump.pc && jump.pc < loop.end;
                const bool toInside = loop.start < jump.target && jump.target <= loop.end;
                if (toInside && !fromInside)
                    simpleControlFlow = false;
                else if (fromInside && !toInside)
                    ++exits;
            }
            if (exits > 1)
                simpleControlFlow = false;
            else if (exits == 1)
                breakJumps.insert(jump.pc);
        }
    }

    const bool jumpFlags = useJumpFlags && simpleControlFlow;
    // Xcompiler (Halo 3): structured shaders only (the state machine has no quad-uniform scope).
    quadKillChecks = isPixelShader && simpleControlFlow;

    // Xcompiler (Halo 3): the loop constants the shader reads but does not define (before, an
    // undeclared i<N> and the whole shader failed to compile), and an iteration counter per loop
    // constant: aL starts at the constant's start and moves by its step, the count is separate.
    for (uint32_t loopIndex : usedLoopConstants)
    {
        if (!literalLoopConstants.count(loopIndex))
            out += fmt::format("\tint4 i{0} = XcLoopConstant({0});\n", loopIndex);
        out += fmt::format("\tint xcLoop{} = 0;\n", loopIndex);
    }
    // Xcompiler: where memory exports (eA, eM0-eM4) are written: nowhere.
    if (!isPixelShader)
        out += "\tfloat4 xcMemExport = 0.0;\n\tfloat4 xcPointSize = float4(-1.0, 0.0, 0.0, 0.0);\n"
               "\tfloat4 xcEA = 0.0;\n\tfloat4 xcEM0 = 0.0;\n\tfloat4 xcEM1 = 0.0;\n\tfloat4 xcEM2 = 0.0;\n"
               "\tfloat4 xcEM3 = 0.0;\n\tfloat4 xcEM4 = 0.0;\n\tuint xcEMWritten = 0u;\n";
    // Xcompiler (Halo 3): a tessellated patch draw (XcTessellatedPatchInputs).
    if (!isPixelShader)
        out += "#if XC_COMPUTED_FETCH\n"
               "\tif ((g_SpecConstants() & SPEC_CONSTANT_TESSELLATION) != 0)\n"
               "\t\tXcTessellatedPatchInputs(xcVertexId, r0, r1);\n"
               "#endif\n";
    // Xcompiler (2026-11-10): the patch plan starts here, with r1.x the patch index. Structured
    // shaders only (the others run their blocks through a program counter, in any order).
    if (!isPixelShader && simpleControlFlow)
    {
        patchPlanOpen = true;
        patchSymbols[1][0] = { 1, 0, 0 };
    }

    // Xcompiler: a boolean constant test, true when the constant equals `condition`. With the jump
    // flags (Gears of War 3), the constants the stripped constant tables don't name are read from
    // g_Booleans too: the native renderer puts vertex b0-b15 in its bits 0-15 and pixel b0-b15
    // (microcode b128-b143) in bits 16-31. Before, they read 0 whatever the game set.
    auto boolTest = [&](uint32_t boolAddress, bool condition) -> std::string
    {
        auto findResult = boolConstants.find(boolAddress);
        if (findResult != boolConstants.end())
            return fmt::format("((g_Booleans & {}) {} 0)", findResult->second, condition ? "!=" : "==");
        uint32_t bit = UINT32_MAX;
        if (isPixelShader && boolAddress >= 128 && boolAddress < 144)
            bit = boolAddress - 128 + 16;
        else if (!isPixelShader && boolAddress < 16)
            bit = boolAddress;
        if (bit != UINT32_MAX)
            return fmt::format("((g_Booleans & {}u) {} 0)", 1u << bit, condition ? "!=" : "==");
        // Xcompiler (Halo 3): the words at SharedConstants + 720 (its water tests vertex b101-b104,
        // other shaders vertex b20/b21 and pixel b228-b231).
        uint32_t word = UINT32_MAX;
        if (!isPixelShader && boolAddress < 32)
            word = 0, bit = boolAddress;
        else if (!isPixelShader && boolAddress >= 96 && boolAddress < 128)
            word = 1, bit = boolAddress - 96;
        else if (isPixelShader && boolAddress >= 128 && boolAddress < 160)
            word = 2, bit = boolAddress - 128;
        else if (isPixelShader && boolAddress >= 224 && boolAddress < 256)
            word = 3, bit = boolAddress - 224;
        if (word != UINT32_MAX)
            return fmt::format("((XcBoolWord({}) & {}u) {} 0)", word, 1u << bit, condition ? "!=" : "==");
        // Not passed by the renderer: reads 0. The comment names it in the HLSL dumps.
        return fmt::format("/*b{}*/ {}", boolAddress, condition ? "false" : "true");
    };
    if (simpleControlFlow)
    {
        out += '\n';
        indentation = 1;
        if (jumpFlags)
        {
            ifEndLabels.clear();
            for (const ForwardJump& jump : forwardJumps)
                out += fmt::format("\tbool jmp{} = false;\n", jump.pc);
        }
    }
    else
    {
        out += "\n\tuint pc = 0;\n";
        out += "\twhile (true)\n";
        out += "\t{\n";
        out += "\t\tswitch (pc)\n";
        out += "\t\t{\n";
    }

    controlFlowCode = code;
    instrAddress = 0;
    uint32_t pc = 0;

    while (instrAddress < instrSize)
    {
        code0 = controlFlowCode[0];
        code1 = controlFlowCode[1] & 0xFFFF;
        code2 = (controlFlowCode[1] >> 16) | (controlFlowCode[2] << 16);
        code3 = controlFlowCode[2] >> 16;

        for (auto& cfInstr : controlFlow)
        {
            if (!simpleControlFlow)
            {
                indentation = 3;
                println("\t\tcase {}:", pc);
            }
            else
            {
                auto findResult = ifEndLabels.find(pc);
                if (findResult != ifEndLabels.end())
                {
                    for (uint32_t i = 0; i < findResult->second; i++)
                    {
                        --indentation;
                        indent();
                        out += "}\n";
                    }
                }
            }

            ++pc;

            // Xcompiler: the jumps that skip this control flow instruction when taken.
            const uint32_t cfIndex = pc - 1;
            std::string jumpGuard;
            if (jumpFlags)
            {
                for (const ForwardJump& jump : forwardJumps)
                {
                    if (jump.pc < cfIndex && cfIndex < jump.target)
                        jumpGuard += fmt::format("{}jmp{}", jumpGuard.empty() ? "" : " || ", jump.pc);
                }
            }

            uint32_t address = 0;
            uint32_t count = 0;
            uint32_t sequence = 0;
            bool shouldReturn = false;

            // Xcompiler (2026-11-10): the patch plan covers only the shader's leading straight
            // run (unconditional blocks nothing jumps over). A jump back into it would run its
            // fetches again with other registers: such a shader gets no plan.
            if (cfInstr.opcode != ControlFlowOpcode::Nop && cfInstr.opcode != ControlFlowOpcode::Exec &&
                cfInstr.opcode != ControlFlowOpcode::ExecEnd && cfInstr.opcode != ControlFlowOpcode::Alloc &&
                cfInstr.opcode != ControlFlowOpcode::MarkVsFetchDone)
                closePatchPlan();
            if (!jumpGuard.empty())
                closePatchPlan();
            if (patchPlanOpen)
                patchPlanLastCf = cfIndex;
            else if (cfInstr.opcode == ControlFlowOpcode::CondJmp && cfInstr.condJmp.address <= patchPlanLastCf)
                patchPlanDropped = true;
            if (cfInstr.opcode == ControlFlowOpcode::LoopEnd && cfInstr.loopEnd.address <= patchPlanLastCf)
                patchPlanDropped = true;
            if (cfInstr.opcode == ControlFlowOpcode::CondCall || cfInstr.opcode == ControlFlowOpcode::Return)
                patchPlanDropped = true;

            switch (cfInstr.opcode)
            {
            case ControlFlowOpcode::Exec:
            case ControlFlowOpcode::ExecEnd:
                address = cfInstr.exec.address;
                count = cfInstr.exec.count;
                sequence = cfInstr.exec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::ExecEnd);
                break;

            case ControlFlowOpcode::CondExec:
            case ControlFlowOpcode::CondExecEnd:
            case ControlFlowOpcode::CondExecPredClean:
            case ControlFlowOpcode::CondExecPredCleanEnd:
                address = cfInstr.condExec.address;
                count = cfInstr.condExec.count;
                sequence = cfInstr.condExec.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecEnd || cfInstr.opcode == ControlFlowOpcode::CondExecEnd) ||
                    (useJumpFlags && cfInstr.opcode == ControlFlowOpcode::CondExecPredCleanEnd);
                break;

            case ControlFlowOpcode::CondExecPred:
            case ControlFlowOpcode::CondExecPredEnd:
                address = cfInstr.condExecPred.address;
                count = cfInstr.condExecPred.count;
                sequence = cfInstr.condExecPred.sequence;
                shouldReturn = (cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd);
                break;

            case ControlFlowOpcode::LoopStart:
                if (simpleControlFlow)
                {
                    indent();
                #ifdef UNLEASHED_RECOMP
                    print("UNROLL ");
                #endif
                    // Xcompiler (Halo 3): count .x times, aL from .y in steps of .z (before: aL from
                    // 0 to the count, so loops indexing constants with aL read the wrong ones).
                    println("aL = i{}.y;", uint32_t(cfInstr.loopStart.loopId));
                    indent();
                    // Xcompiler (Halo 3): a loop that a taken jump skips runs no iteration.
                    if (jumpGuard.empty())
                        println("for (xcLoop{0} = 0; xcLoop{0} < i{0}.x; xcLoop{0}++)", uint32_t(cfInstr.loopStart.loopId));
                    else
                        println("for (xcLoop{0} = 0; xcLoop{0} < i{0}.x && !({1}); xcLoop{0}++)", uint32_t(cfInstr.loopStart.loopId), jumpGuard);
                    indent();
                    out += "{\n";
                    ++indentation;
                }
                else
                {
                    println("\t\t\taL = i{0}.y;", uint32_t(cfInstr.loopStart.loopId));
                    println("\t\t\txcLoop{} = 0;", uint32_t(cfInstr.loopStart.loopId));
                }
                break;

            case ControlFlowOpcode::LoopEnd:
                if (simpleControlFlow)
                {
                    indent();
                    println("aL += i{}.z;", uint32_t(cfInstr.loopEnd.loopId));
                    --indentation;
                    indent();
                    out += "}\n";
                }
                else
                {
                    println("\t\t\taL += i{}.z;", uint32_t(cfInstr.loopEnd.loopId));
                    println("\t\t\t++xcLoop{};", uint32_t(cfInstr.loopEnd.loopId));
                    println("\t\t\tif (xcLoop{0} < i{0}.x)", uint32_t(cfInstr.loopEnd.loopId));
                    out += "\t\t\t{\n";
                    println("\t\t\t\tpc = {};", uint32_t(cfInstr.loopEnd.address));
                    out += "\t\t\t\tcontinue;\n";
                    out += "\t\t\t}\n";
                }
                break;

            case ControlFlowOpcode::CondJmp:
            {
                if (cfInstr.condJmp.isUnconditional && !jumpFlags)
                {
                    assert(!simpleControlFlow);
                    println("\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                    out += "\t\t\tcontinue;\n";
                }
                else if (jumpFlags)
                {
                    // Taken when the predicate or boolean equals the condition (unknown booleans read 0).
                    std::string taken;
                    if (cfInstr.condJmp.isUnconditional)
                    {
                        taken = "true";
                    }
                    else if (cfInstr.condJmp.isPredicated)
                    {
                        taken = cfInstr.condJmp.condition ? "p0" : "!p0";
                    }
                    else
                    {
                        taken = boolTest(cfInstr.condJmp.boolAddress, cfInstr.condJmp.condition);
                    }
                    indent();
                    if (jumpGuard.empty())
                        println("jmp{} = {};", cfIndex, taken);
                    else
                        println("jmp{} = !({}) && ({});", cfIndex, jumpGuard, taken);
                    // Xcompiler (Halo 3): a jump out of the loop ends it; the flag skips the rest.
                    if (breakJumps.count(cfIndex))
                    {
                        indent();
                        println("if (jmp{}) break;", cfIndex);
                    }
                }
                else
                {
                    indent();
                    if (cfInstr.condJmp.isPredicated)
                    {
                        println("if ({}p0)", cfInstr.condJmp.condition ^ simpleControlFlow ? "" : "!");
                    }
                    else if (useJumpFlags)
                    {
                        println("if ({})", boolTest(cfInstr.condJmp.boolAddress, cfInstr.condJmp.condition ^ simpleControlFlow));
                    }
                    else
                    {
                        auto findResult = boolConstants.find(cfInstr.condJmp.boolAddress);
                        if (findResult != boolConstants.end())
                            println("if ((g_Booleans & {}) {}= 0)", findResult->second, cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
                        else
                            println("if ({})", cfInstr.condJmp.condition ^ simpleControlFlow ? "false" : "true"); 
                        // println("if (b{} {}= 0)", uint32_t(cfInstr.condJmp.boolAddress), cfInstr.condJmp.condition ^ simpleControlFlow ? "!" : "=");
                    }

                    if (simpleControlFlow)
                    {
                        indent();
                        out += "{\n";
                        ++indentation;
                    }
                    else
                    {
                        out += "\t\t\t{\n";
                        println("\t\t\t\tpc = {};", uint32_t(cfInstr.condJmp.address));
                        out += "\t\t\t\tcontinue;\n";
                        out += "\t\t\t}\n";
                    }
                }
                break;
            }
            }

            auto instructionCode = code + address * 3;

            bool jumpGuarded = false;
            if (useJumpFlags && count != 0)
            {
                std::string condition = jumpGuard.empty() ? std::string() : "!(" + jumpGuard + ")";
                if (cfInstr.opcode == ControlFlowOpcode::CondExecPred ||
                    cfInstr.opcode == ControlFlowOpcode::CondExecPredEnd)
                {
                    condition += condition.empty() ? "" : " && ";
                    condition += cfInstr.condExecPred.condition ? "p0" : "!p0";
                }
                else if (cfInstr.opcode == ControlFlowOpcode::CondExec ||
                    cfInstr.opcode == ControlFlowOpcode::CondExecEnd ||
                    cfInstr.opcode == ControlFlowOpcode::CondExecPredClean ||
                    cfInstr.opcode == ControlFlowOpcode::CondExecPredCleanEnd)
                {
                    condition += condition.empty() ? "" : " && ";
                    condition += boolTest(cfInstr.condExec.boolAddress, cfInstr.condExec.condition);
                }
                if (!condition.empty())
                {
                    indent();
                    println("if ({})", condition);
                    indent();
                    out += "{\n";
                    ++indentation;
                    jumpGuarded = true;
                }
            }
            
            for (uint32_t i = 0; i < count; i++)
            {
                union
                {
                    VertexFetchInstruction vertexFetch;
                    TextureFetchInstruction textureFetch;
                    AluInstruction alu;
                    struct
                    {
                        uint32_t code0;
                        uint32_t code1;
                        uint32_t code2;
                    };
                };
            
                code0 = instructionCode[0];
                code1 = instructionCode[1];
                code2 = instructionCode[2];
            
                if ((sequence & 0x1) != 0)
                {
                    if (vertexFetch.opcode == FetchOpcode::VertexFetch)
                    {
                        recompile(vertexFetch, address + i);
                    }
                    else
                    {
                    #ifdef UNLEASHED_RECOMP
                        if (textureFetch.constIndex == 10) // g_GISampler
                        {
                            specConstantsMask |= SPEC_CONSTANT_BICUBIC_GI_FILTER;

                            indent();
                            out += "if (g_SpecConstants() & SPEC_CONSTANT_BICUBIC_GI_FILTER)\n";
                            indent();
                            out += "{\n";

                            ++indentation;
                            recompile(textureFetch, true);
                            --indentation;

                            indent();
                            out += "}\n";
                            indent();
                            out += "else\n";
                            indent();
                            out += "{\n";

                            ++indentation;
                            recompile(textureFetch, false);
                            --indentation;

                            indent();
                            out += "}\n";
                        }
                        else
                    #endif
                        {
                            recompile(textureFetch, false);
                        }
                        closePatchPlan();
                    }
                }
                else
                {
                    const size_t aluBegin = out.size();
                    recompile(alu);
                    patchPlanAfterAlu(aluBegin);
                }
            
                sequence >>= 2;
                instructionCode += 3;
            }

            if (jumpGuarded && shouldReturn)
            {
                --indentation;
                indent();
                out += "}\n";
                jumpGuarded = false;
            }

            if (shouldReturn)
            {
                if (isPixelShader)
                {
                    // Xcompiler: the native renderer enables alpha test only for shaders that take
                    // its comparison too (SPEC_CONSTANT_ALPHA_TEST_CAPABILITY_MASK, 0x702), the
                    // Xenos CompareFunction in bits 8-10; the fragment is kept when the test passes.
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TEST | 0x700;

                    out += "#ifdef __spirv__\n";
                    indent();
                    out += "BRANCH if ((g_SpecConstants() & SPEC_CONSTANT_CLIP_EMULATION) != 0 && (any(input.xcClipEmulation < 0.0) || any(input.xcClipEmulation1 < 0.0))) discard;\n";
                    out += "#endif\n";

                    indent();
                    out += "BRANCH if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TEST)\n";
                    indent();
                    out += "{\n";

                    indent();
                    out += "\tuint alphaFunction = (g_SpecConstants() >> 8) & 7;\n";
                    indent();
                    out += "\tfloat alphaValue = output.oC0.w;\n";
                    indent();
                    out += "\tbool alphaPass = ((alphaFunction & 1) != 0 && alphaValue < g_AlphaThreshold) ||\n";
                    indent();
                    out += "\t\t((alphaFunction & 2) != 0 && alphaValue == g_AlphaThreshold) ||\n";
                    indent();
                    out += "\t\t((alphaFunction & 4) != 0 && alphaValue > g_AlphaThreshold);\n";
                    indent();
                    out += "\tif (!alphaPass)\n";
                    indent();
                    out += "\t\tdiscard;\n";

                    indent();
                    out += "}\n";

                #ifdef UNLEASHED_RECOMP
                    specConstantsMask |= SPEC_CONSTANT_ALPHA_TO_COVERAGE;

                    indent();
                    out += "else if (g_SpecConstants() & SPEC_CONSTANT_ALPHA_TO_COVERAGE)\n";
                    indent();
                    out += "{\n";

                    indent();
                    out += "\toutput.oC0.w *= 1.0 + computeMipLevel(pixelCoord) * 0.25;\n";
                    indent();
                    out += "\toutput.oC0.w = 0.5 + (output.oC0.w - g_AlphaThreshold) / max(fwidth(output.oC0.w), 1e-6);\n";

                    indent();
                    out += "}\n";
                #endif
                }
                else
                {
                    // Xcompiler (Gears of War 3): a shader that fetches by a computed index got the
                    // vertex's own attributes, so its primitives join unrelated records: particle
                    // sprites and instanced meshes drawn as spikes and big flat shapes across the
                    // screen. Until computed fetches are translated, they are left out (outside the
                    // clip volume) rather than drawn wrong.
                    if (useJumpFlags && computedVertexFetch)
                        out += "#if !XC_COMPUTED_FETCH\n\toutput.oPos = float4(4.0, 4.0, 0.0, 1.0);\n#endif\n";
                    out += "#ifdef __spirv__\n\toutput.oPointSize = ((g_SpecConstants() & SPEC_CONSTANT_POINT_SPRITE) != 0) ? XcPointDiameter(xcPointSize.x) : 1.0;\n#endif\n";
                    out += "#ifdef __spirv__\n\tif (xcEMWritten != 0u) { XcMemExport(xcEA, xcEM0, xcEM1, xcEM2, xcEM3, xcEM4, xcEMWritten); xcEMWritten = 0u; }\n#endif\n";
                    // Xcompiler (Halo 3): every enabled user clip plane (mask at SharedConstants + 1192,
                    // plane 0 g_ClipPlane, planes 1-5 at + 1232 + 16 * (n - 1)); a disabled one gets
                    // a positive distance. Halo's dynamic lights clip to their volume with all six.
                    out += "#ifdef __spirv__\n\t{\n\t\tuint xcClipMask = vk::RawBufferLoad<uint>(g_PushConstants.SharedConstants + 1192) & 0x3Fu;\n";
                    out += "\t\tif (xcClipMask != 0u)\n\t\t{\n";
                    out += "\t\t\tfloat4 xcClipD0 = 1.0;\n\t\t\tfloat2 xcClipD1 = 1.0;\n";
                    out += "\t\t\tif ((xcClipMask & 1u) != 0u) xcClipD0.x = dot(output.oPos, g_ClipPlane);\n";
                    out += "\t\t\tif ((xcClipMask & 2u) != 0u) xcClipD0.y = dot(output.oPos, vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1232));\n";
                    out += "\t\t\tif ((xcClipMask & 4u) != 0u) xcClipD0.z = dot(output.oPos, vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1248));\n";
                    out += "\t\t\tif ((xcClipMask & 8u) != 0u) xcClipD0.w = dot(output.oPos, vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1264));\n";
                    out += "\t\t\tif ((xcClipMask & 16u) != 0u) xcClipD1.x = dot(output.oPos, vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1280));\n";
                    out += "\t\t\tif ((xcClipMask & 32u) != 0u) xcClipD1.y = dot(output.oPos, vk::RawBufferLoad<float4>(g_PushConstants.SharedConstants + 1296));\n";
                    out += "\t\t\toutput.xcClipEmulation = xcClipD0;\n\t\t\toutput.xcClipEmulation1 = xcClipD1;\n";
                    out += "\t\t\tif ((g_SpecConstants() & SPEC_CONSTANT_CLIP_EMULATION) == 0) { output.clipDistance = xcClipD0; output.clipDistance1 = xcClipD1; }\n";
                    out += "\t\t}\n\t}\n#else\n";
                    out += "\tif (g_ClipPlaneEnabled) output.clipDistance = dot(output.oPos, g_ClipPlane);\n";
                    out += "#endif\n";
                    out += "\toutput.oPos.xy += g_HalfPixelOffset * output.oPos.w;\n";
                }

                if (simpleControlFlow)
                {
                    indent();
                #ifdef UNLEASHED_RECOMP
                    if (hasMtxProjection)
                    {
                        out += "continue;\n";
                    }
                    else
                #endif
                    {
                        out += "return output;\n";
                    }
                }
                else
                {
                    out += "\t\t\tbreak;\n";
                }
            }

            if (jumpGuarded)
            {
                --indentation;
                indent();
                out += "}\n";
            }
        }

        controlFlowCode += 3;
        instrAddress += 12;
    }

    if (!simpleControlFlow)
    {
        out += "\t\t\tbreak;\n";
        out += "\t\t}\n";
        out += "\t\tbreak;\n";
        out += "\t}\n";
    }

#ifdef UNLEASHED_RECOMP
    if (hasMtxProjection)
        out += "\t}\n";
#endif

    if (!simpleControlFlow)
        out += "\treturn output;\n";
    else if (jumpFlags && !forwardJumps.empty())
        out += "\treturn output;\n";
#ifdef UNLEASHED_RECOMP
    else if (hasMtxProjection)
        out += "\treturn output;\n";
#endif

    out += "}";

    // Xcompiler (Halo 3): SPEC_CONSTANT_MEMEXPORT in the mask tells the renderer to give the shader
    // its export regions (SharedConstants + 1216). Its fetches read through the declaration
    // (XcFetchVertexDecl), so they see what earlier exports wrote to the regions.
    if (memExportUsed && !isPixelShader)
    {
        specConstantsMask |= 1u << 13;
        computedVertexFetch = true;
    }

    // Xcompiler (Halo 3): and whether the ALU reads the vertex index r0.x starts with.
    out.insert(computedFetchDefinePos, !isPixelShader && vertexIndexRead
        ? "#ifdef __air__\n#define XC_VERTEX_INDEX 0\n#else\n#define XC_VERTEX_INDEX 1\n#endif\n"
        : "#define XC_VERTEX_INDEX 0\n");
    // Xcompiler (Halo 3): only now is it known whether a vfetch computed its index.
    if (isPixelShader || !computedVertexFetch)
    {
        vertexFetchSlots[0] = vertexFetchSlots[1] = vertexFetchSlots[2] = 0;
        patchPlanCount = 0;
        out.insert(computedFetchDefinePos, "#define XC_COMPUTED_FETCH 0\n");
    }
    else
    {
        // The specialization constants of the declaration locations read (shader_recompiler.h).
        std::string declared = "#ifdef __spirv__\n#define XC_COMPUTED_FETCH 1\n";
        for (uint32_t location = 0; location < 64; location++)
        {
            if (declLocationsUsed & (uint64_t(1) << location))
                declared += fmt::format("[[vk::constant_id({})]] const uint xcDecl{} = 0;\n", 100 + location, location);
        }
        declared += "#else\n#define XC_COMPUTED_FETCH 0\n#endif\n";
        out.insert(computedFetchDefinePos, declared);
    }
    // The patch plan is all or nothing for the renderer: without it the shader's XcPatchFetch
    // calls read as plain declaration fetches.
    if (patchPlanDropped)
        patchPlanCount = 0;
    for (uint32_t i = patchPlanCount; i < kPatchPlanCapacity; i++)
        patchPlan[i] = 0;
}
