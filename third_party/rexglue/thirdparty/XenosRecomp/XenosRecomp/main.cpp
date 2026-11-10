#include <deque>
#include <mutex>
#include <thread>

#include "shader.h"
#include "shader_recompiler.h"
#include "dxc_compiler.h"

#ifdef XENOS_RECOMP_AIR
#include "air_compiler.h"
#endif

static std::unique_ptr<uint8_t[]> readAllBytes(const char* filePath, size_t& fileSize)
{
    FILE* file = fopen(filePath, "rb");
    fseek(file, 0, SEEK_END);
    fileSize = ftell(file);
    fseek(file, 0, SEEK_SET);
    auto data = std::make_unique<uint8_t[]>(fileSize);
    fread(data.get(), 1, fileSize, file);
    fclose(file);
    return data;
}

static void writeAllBytes(const char* filePath, const void* data, size_t dataSize)
{
    FILE* file = fopen(filePath, "wb");
    // Xcompiler: a path that can't be opened is reported, not passed to fwrite (the CRT aborts).
    if (file == nullptr)
    {
        fmt::println("error: cannot write {}", filePath);
        return;
    }
    fwrite(data, 1, dataSize, file);
    fclose(file);
}

struct RecompiledShader
{
    uint8_t* data = nullptr;
    IDxcBlob* dxil = nullptr;
    std::vector<uint8_t> spirv;
    std::vector<uint8_t> air;
    uint32_t specConstantsMask = 0;
    uint32_t usedTextureMask = 0;
    uint32_t vertexFetchSlots[3] = {};
    uint32_t patchPlan[ShaderRecompiler::kPatchPlanCapacity] = {};
    bool isPixelShader = false;
    bool failed = false;
    uint64_t hash = 0;
};

void recompileShader(RecompiledShader& shader, const std::string_view include, std::atomic<uint32_t>& progress, uint32_t numShaders)
{
    thread_local ShaderRecompiler recompiler;
    recompiler = {};
    try
    {
        recompiler.recompile(shader.data, include);
    }
    catch (const std::exception& error)
    {
        fmt::println("error: {}", error.what());
        shader.failed = true;
        ++progress;
        return;
    }

    // Xcompiler: XENOS_RECOMP_DUMP_HLSL=<dir> writes each shader's HLSL there, by hash, to read it.
    if (const char* hlslDir = getenv("XENOS_RECOMP_DUMP_HLSL"))
    {
        const std::string hlslPath = fmt::format("{}/{:016X}.hlsl", hlslDir, shader.hash);
        writeAllBytes(hlslPath.c_str(), recompiler.out.data(), recompiler.out.size());
    }

    shader.specConstantsMask = recompiler.specConstantsMask;
    shader.usedTextureMask = recompiler.usedTextureMask;
    for (int i = 0; i < 3; i++)
        shader.vertexFetchSlots[i] = recompiler.vertexFetchSlots[i];
    for (uint32_t i = 0; i < ShaderRecompiler::kPatchPlanCapacity; i++)
        shader.patchPlan[i] = recompiler.patchPlan[i];
    shader.isPixelShader = recompiler.isPixelShader;

    thread_local DxcCompiler dxcCompiler;

#ifdef XENOS_RECOMP_DXIL
    shader.dxil = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, recompiler.specConstantsMask != 0, false);
    assert(shader.dxil != nullptr);
    assert(*(reinterpret_cast<uint32_t *>(shader.dxil->GetBufferPointer()) + 1) != 0 && "DXIL was not signed properly!");
#endif

#ifdef XENOS_RECOMP_AIR
    shader.air = AirCompiler::compile(recompiler.out);
#endif

    IDxcBlob* spirv = dxcCompiler.compile(recompiler.out, recompiler.isPixelShader, false, true);
    // Xcompiler: a shader DXC rejects is left out of the cache (the renderer reports a cache miss).
    if (spirv == nullptr || !smolv::Encode(spirv->GetBufferPointer(), spirv->GetBufferSize(), shader.spirv, smolv::kEncodeFlagStripDebugInfo))
    {
        shader.failed = true;
        shader.spirv.clear();
    }

    if (spirv != nullptr)
        spirv->Release();

    size_t currentProgress = ++progress;
    if ((currentProgress % 10) == 0 || (currentProgress == numShaders - 1))
        fmt::println("Recompiling shaders... {}%", currentProgress / float(numShaders) * 100.0f);
}

int main(int argc, char** argv)
{
#ifndef XENOS_RECOMP_INPUT
    if (argc < 4)
    {
        printf("Usage: XenosRecomp [input path] [output path] [shader common header file path]");
        return 0;
    }
#endif

    const char* input =
#ifdef XENOS_RECOMP_INPUT 
        XENOS_RECOMP_INPUT
#else
        argv[1]
#endif
    ;

    const char* output =
#ifdef XENOS_RECOMP_OUTPUT 
        XENOS_RECOMP_OUTPUT
#else
        argv[2]
#endif
        ;
    
    const char* includeInput =
#ifdef XENOS_RECOMP_INCLUDE_INPUT
        XENOS_RECOMP_INCLUDE_INPUT
#else
        argv[3]
#endif
        ;

    size_t includeSize = 0;
    auto includeData = readAllBytes(includeInput, includeSize);
    std::string_view include(reinterpret_cast<const char*>(includeData.get()), includeSize);

    if (std::filesystem::is_directory(input))
    {
        std::vector<std::unique_ptr<uint8_t[]>> files;
        std::map<XXH64_hash_t, RecompiledShader> shaders;
        std::map<XXH64_hash_t, std::string> shaderFilenames;

        for (auto& file : std::filesystem::recursive_directory_iterator(input))
        {
            if (std::filesystem::is_directory(file))
            {
                continue;
            }
            
            size_t fileSize = 0;
            auto fileData = readAllBytes(file.path().string().c_str(), fileSize);
            bool foundAny = false;

            for (size_t i = 0; fileSize > sizeof(ShaderContainer) && i < fileSize - sizeof(ShaderContainer) - 1;)
            {
                auto shaderContainer = reinterpret_cast<const ShaderContainer*>(fileData.get() + i);
                size_t dataSize = shaderContainer->virtualSize + shaderContainer->physicalSize;

                if ((shaderContainer->flags & 0xFFFFFF00) == 0x102A1100 &&
                    dataSize <= (fileSize - i) &&
                    shaderContainer->field1C == 0 &&
                    shaderContainer->field20 == 0)
                {
                    XXH64_hash_t hash = XXH3_64bits(shaderContainer, dataSize);
                    auto shader = shaders.try_emplace(hash);
                    if (shader.second)
                    {
                        shader.first->second.data = fileData.get() + i;
                        foundAny = true;
                        shaderFilenames[hash] = file.path().string();
                    }

                    i += dataSize;
                }
                else
                {
                    i += sizeof(uint32_t);
                }
            }

            if (foundAny)
                files.emplace_back(std::move(fileData));
        }

        std::mutex shaderQueueMutex;
        std::deque<XXH64_hash_t> shaderQueue;
        for (const auto& [hash, _] : shaders)
        {
            shaderQueue.emplace_back(hash);
        }

        const uint32_t numThreads = std::max(std::thread::hardware_concurrency(), 1u);
        fmt::println("Recompiling shaders with {} threads", numThreads);

        std::atomic<uint32_t> progress = 0;
        std::vector<std::thread> threads;
        threads.reserve(numThreads);
        for (uint32_t i = 0; i < numThreads; i++)
        {
            threads.emplace_back([&]
            {
                while (true)
                {
                    XXH64_hash_t shaderHash;
                    {
                        std::lock_guard lock(shaderQueueMutex);
                        if (shaderQueue.empty()) {
                            return;
                        }
                        shaderHash = shaderQueue.front();
                        shaderQueue.pop_front();
                    }
                    shaders[shaderHash].hash = shaderHash;
                    recompileShader(shaders[shaderHash], include, progress, shaders.size());
                }
            });
        }
        for (auto& thread : threads)
        {
            thread.join();
        }

        fmt::println("Creating shader cache...");

        StringBuffer f;
        f.println("#include \"shader_cache.h\"");
        f.println("ShaderCacheEntry g_shaderCacheEntries[] = {{");

        std::vector<uint8_t> dxil;
        std::vector<uint8_t> spirv;
        std::vector<uint8_t> air;

        size_t entryCount = 0;
        for (auto& [hash, shader] : shaders)
        {
            if (shader.failed)
            {
                fmt::println("error: shader {:016X} from {} did not compile", hash, shaderFilenames[hash]);
                continue;
            }
            ++entryCount;
            const std::string& fullFilename = shaderFilenames[hash];
            std::string filename = fullFilename;
            size_t shaderPos = filename.find("shader");
            if (shaderPos != std::string::npos) {
                filename = filename.substr(shaderPos);
                // Prevent bad escape sequences in Windows shader path.
                std::replace(filename.begin(), filename.end(), '\\', '/');
            }
            // Xcompiler: the native renderer's ShaderCacheEntry (games/<title>/data/shader/shader_cache.h):
            // "_vs"/"_ps" in the name for its stage check, and the fetch constants. Alpha-tested pixel
            // shaders need a late-Z module (no EarlyFragmentTests) to be drawn with alpha test; this
            // XenosRecomp never asks for early tests, so the one module serves as both.
            if (filename.find("_vs") == std::string::npos && filename.find("_ps") == std::string::npos)
                filename += shader.isPixelShader ? "_ps" : "_vs";
            const bool late = shader.isPixelShader && (shader.specConstantsMask & 0x702) == 0x702;
            // The patch fetch plan (shader_recompiler.h), only for the shaders that have one.
            std::string patchPlanText;
            if (shader.patchPlan[0] != 0)
            {
                patchPlanText = ", {";
                for (uint32_t i = 0; i < ShaderRecompiler::kPatchPlanCapacity && shader.patchPlan[i] != 0; i++)
                    patchPlanText += fmt::format("{}0x{:X}u", i ? ", " : " ", shader.patchPlan[i]);
                patchPlanText += " }";
            }
            f.println("\t{{ 0x{:X}, {}, {}, {}, {}, {}, {}, {}, {}, {}, \"{}\", nullptr, {}, {{ {}, {}, {} }}{} }},",
                hash, dxil.size(), (shader.dxil != nullptr) ? shader.dxil->GetBufferSize() : 0,
                spirv.size(), shader.spirv.size(), late ? spirv.size() : 0, late ? shader.spirv.size() : 0,
                air.size(), shader.air.size(), shader.specConstantsMask, filename, shader.usedTextureMask,
                shader.vertexFetchSlots[0], shader.vertexFetchSlots[1], shader.vertexFetchSlots[2],
                patchPlanText);

            if (shader.dxil != nullptr)
            {
                dxil.insert(dxil.end(), reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()),
                    reinterpret_cast<uint8_t *>(shader.dxil->GetBufferPointer()) + shader.dxil->GetBufferSize());
            }

#ifdef XENOS_RECOMP_AIR
            air.insert(air.end(), shader.air.begin(), shader.air.end());
#endif

            spirv.insert(spirv.end(), shader.spirv.begin(), shader.spirv.end());
        }

        f.println("}};");

        fmt::println("Compressing DXIL cache...");

        int level = ZSTD_maxCLevel();

#ifdef XENOS_RECOMP_DXIL
        std::vector<uint8_t> dxilCompressed(ZSTD_compressBound(dxil.size()));
        dxilCompressed.resize(ZSTD_compress(dxilCompressed.data(), dxilCompressed.size(), dxil.data(), dxil.size(), level));

        f.print("const uint8_t g_compressedDxilCache[] = {{");

        for (auto data : dxilCompressed)
            f.print("{},", data);

        f.println("}};");
        f.println("const size_t g_dxilCacheCompressedSize = {};", dxilCompressed.size());
        f.println("const size_t g_dxilCacheDecompressedSize = {};", dxil.size());
#endif

#ifdef XENOS_RECOMP_AIR
        fmt::println("Compressing AIR cache...");

        std::vector<uint8_t> airCompressed(ZSTD_compressBound(air.size()));
        airCompressed.resize(ZSTD_compress(airCompressed.data(), airCompressed.size(), air.data(), air.size(), level));

        f.print("const uint8_t g_compressedAirCache[] = {{");

        for (auto data : airCompressed)
            f.print("{},", data);

        f.println("}};");
        f.println("const size_t g_airCacheCompressedSize = {};", airCompressed.size());
        f.println("const size_t g_airCacheDecompressedSize = {};", air.size());
#endif

        fmt::println("Compressing SPIRV cache...");

        std::vector<uint8_t> spirvCompressed(ZSTD_compressBound(spirv.size()));
        spirvCompressed.resize(ZSTD_compress(spirvCompressed.data(), spirvCompressed.size(), spirv.data(), spirv.size(), level));

        f.print("const uint8_t g_compressedSpirvCache[] = {{");

        for (auto data : spirvCompressed)
            f.print("{},", data);

        f.println("}};");

        f.println("const size_t g_spirvCacheCompressedSize = {};", spirvCompressed.size());
        f.println("const size_t g_spirvCacheDecompressedSize = {};", spirv.size());
        f.println("const size_t g_shaderCacheEntryCount = {};", entryCount);

        writeAllBytes(output, f.out.data(), f.out.size());
    }
    else
    {
        ShaderRecompiler recompiler;
        size_t fileSize;
        recompiler.recompile(readAllBytes(input, fileSize).get(), include);
        writeAllBytes(output, recompiler.out.data(), recompiler.out.size());
    }

    return 0;
}
