#pragma once

#include "shader.h"
#include "shader_code.h"

struct StringBuffer
{
    std::string out;

    template<class... Args>
    void print(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
    }

    template<class... Args>
    void println(fmt::format_string<Args...> fmt, Args&&... args)
    {
        fmt::vformat_to(std::back_inserter(out), fmt.get(), fmt::make_format_args(args...));
        out += '\n';
    }
};

struct ShaderRecompiler : StringBuffer
{
    uint32_t indentation = 0;
    bool isPixelShader = false;
    const uint8_t* constantTableData = nullptr;
    std::unordered_map<uint32_t, VertexElement> vertexElements;
    std::unordered_map<uint32_t, std::string> interpolators;
    std::unordered_map<uint32_t, const ConstantInfo*> float4Constants;
    // Xcompiler: the float constants the shader defines itself (c<N> literals). One that is neither
    // these nor in the constant table (an engine that strips the table, as Gears of War 3's Unreal
    // Engine 3 does) is read by its register: XcConstant(N).
    std::unordered_map<uint32_t, bool> literalFloat4Constants;
    std::unordered_map<uint32_t, const char*> boolConstants;
    std::unordered_map<uint32_t, const char*> samplers;
    std::unordered_map<uint32_t, uint32_t> ifEndLabels;
    uint32_t specConstantsMask = 0;
    // Xcompiler: the texture fetch constants the microcode reads (the renderer's usedTextureMask).
    uint32_t usedTextureMask = 0;
    // Xcompiler: Xenos starts a vertex shader with the vertex index in r0.x. A vertex fetch whose
    // index is another register, or r0.x after the shader wrote it, reads another vertex's record
    // (instancing, particle sprites); the translation reads the vertex's own attributes instead.
    bool vertexIndexWritten = false;
    // Xcompiler (Halo 3): an ALU instruction reads r0 before the shader wrote r0.x: it uses the
    // vertex index Xenos starts r0.x with (screen quads built from the index, such as the water
    // ripple slope pass 371E75ED73F5ED40), so the translation takes SV_VertexID in every variant.
    bool vertexIndexRead = false;
    bool computedVertexFetch = false;
    // Xcompiler (Halo 3): a vertex shader with computed fetch indices reads every vfetch itself
    // from guest memory (XcFetchVertex, SPIR-V only): the slots (0-95) it reads, for the renderer,
    // and where the XC_COMPUTED_FETCH define goes once the whole shader is known.
    uint32_t vertexFetchSlots[3] = {};
    size_t computedFetchDefinePos = 0;
    // Xcompiler (Halo 3): the shader writes eA/eM (memory export).
    bool memExportUsed = false;
    // The last full vfetch: a mini fetch reuses its slot, index and stride.
    uint32_t lastFetchSlot = 0;
    uint32_t lastFetchStride = 0;
    // Xcompiler (Halo 3): a pixel shader kill ran since the last quad check (xcKill), and whether
    // the shader is structured (indentation 1 is shaderMain's own scope, quad-uniform), where the
    // check that returns early from a quad whose four pixels are discarded goes.
    bool quadKillPending = false;
    bool quadKillChecks = false;
    bool lastFetchRounded = false;
    std::string lastFetchIndex = "0.0";
    // Xcompiler (Halo 3, 2026-11-10, performance): the patch fetch plan. A tessellated patch draw
    // starts the vertex shader with the patch index in r1.x, and the water's shader opens with 25
    // vertex fetches whose indices depend only on it (the patch's control point indices, then the
    // control points). Those are the same for every vertex of a patch, so the renderer can run
    // them once per patch on the CPU and hand the shader the results (XcPatchFetch). The plan is
    // the list of those fetches, in order: per entry the declaration location, where the index
    // comes from and the rounding. Only the leading straight run of the shader is planned: it
    // closes at the first instruction whose effect on the registers is not followed here.
    //   bits 0-5 location, 6-7 index kind (1 patch index, 2 patch index + float constant, 3 a
    //   component of an earlier entry's result), 8-15 the constant register or the entry, 16-17
    //   the component, 18 rounded index, 31 valid.
    static constexpr uint32_t kPatchPlanCapacity = 32;
    struct PatchSymbol
    {
        uint8_t kind = 0; // 0 unknown, else as the plan's index kind
        uint8_t argument = 0;
        uint8_t component = 0;
    };
    // Xcompiler (Halo 3, 2026-11-10, performance): the declaration locations the shader's unbound
    // fetches read. Each gets a specialization constant (id 100 + location, xcDecl<location>):
    // the element's format word as the fetch table holds it, which the renderer gives the
    // pipeline from its vertex declaration, so the driver compiles the fetch for that one format
    // instead of the decode of every format (XcFetchVertexDecl); 0 leaves the decode as it was.
    uint64_t declLocationsUsed = 0;
    bool patchPlanOpen = false;
    PatchSymbol patchSymbols[64][4] = {};
    uint32_t patchPlan[kPatchPlanCapacity] = {};
    uint32_t patchPlanCount = 0;
    // The last control flow instruction the open plan covered, and a later jump back into it.
    uint32_t patchPlanLastCf = 0;
    bool patchPlanDropped = false;
    // The index register and component of the last full vfetch (a mini fetch reuses them).
    uint32_t lastFetchIndexRegister = 0;
    uint32_t lastFetchIndexComponent = 0;
    void closePatchPlan() { patchPlanOpen = false; }
    // After an ALU instruction was written (its text starts at outBegin): follows the one form
    // the plan knows, "rD.c = patch index + constant", and closes the plan at any other.
    void patchPlanAfterAlu(size_t outBegin);

#ifdef UNLEASHED_RECOMP
    bool hasMtxProjection = false;
    bool hasMtxPrevInvViewProjection = false;
#endif

    void indent()
    {
        for (uint32_t i = 0; i < indentation; i++)
            out += '\t';
    }

    uint32_t printDstSwizzle(uint32_t dstSwizzle, bool operand);
    void printDstSwizzle01(uint32_t dstRegister, uint32_t dstSwizzle);

    void recompile(const VertexFetchInstruction& instr, uint32_t address);
    void recompile(const TextureFetchInstruction& instr, bool bicubic);
    void recompile(const AluInstruction& instr);

    void recompile(const uint8_t* shaderData, const std::string_view& include);
};
