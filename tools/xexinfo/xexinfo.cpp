// xexinfo: loads an XEX2 with XenonUtils (AES + LZX), dumps the in-memory PE image and
// prints the header facts the port needs (imports, static libraries, sections, TLS, etc).
// Usage: xexinfo <default.xex> <out.pe> > info.txt
#include <cstdio>
#include <cstring>
#include <unordered_map>
#include <file.h>
#include <image.h>
#include <xex.h>

extern std::unordered_map<size_t, const char*> XamExports;
extern std::unordered_map<size_t, const char*> XboxKernelExports;

static uint32_t be32(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (b[0] << 24) | (b[1] << 16) | (b[2] << 8) | b[3];
}

static uint16_t be16(const void* p)
{
    const uint8_t* b = (const uint8_t*)p;
    return (b[0] << 8) | b[1];
}

static const uint8_t* optHeader(const uint8_t* data, uint32_t key)
{
    auto* header = (const Xex2Header*)data;
    auto* opts = (const Xex2OptHeader*)(header + 1);
    for (uint32_t i = 0; i < header->headerCount; i++)
    {
        if (opts[i].key != key)
            continue;
        if ((key & 0xFF) == 0)
            return (const uint8_t*)&opts[i].value; // value stored inline
        if ((key & 0xFF) == 1)
            return (const uint8_t*)&opts[i].value;
        return data + opts[i].offset;
    }
    return nullptr;
}

int main(int argc, char** argv)
{
    if (argc < 3)
    {
        fprintf(stderr, "usage: xexinfo <xex> <out.pe>\n");
        return 1;
    }

    const auto file = LoadFile(argv[1]);
    const uint8_t* data = file.data();
    auto* header = (const Xex2Header*)data;
    auto* security = (const Xex2SecurityInfo*)(data + header->securityOffset);
    auto* ffi = (const Xex2OptFileFormatInfo*)optHeader(data, XEX_HEADER_FILE_FORMAT_INFO);

    printf("[xex]\nmodule_flags = 0x%08X\nheader_size = 0x%X\nimage_size = 0x%X\nload_address = 0x%08X\nimage_flags = 0x%08X\n",
        (uint32_t)header->moduleFlags, (uint32_t)header->headerSize, (uint32_t)security->imageSize,
        (uint32_t)security->loadAddress, (uint32_t)security->imageFlags);
    if (ffi)
        printf("encryption = %u\ncompression = %u\n", (uint32_t)ffi->encryptionType, (uint32_t)ffi->compressionType);

    auto* opts = (const Xex2OptHeader*)(header + 1);
    printf("\n[opt_headers]\n");
    for (uint32_t i = 0; i < header->headerCount; i++)
        printf("0x%08X = 0x%08X\n", (uint32_t)opts[i].key, (uint32_t)opts[i].value);

    if (auto* p = optHeader(data, XEX_HEADER_ORIGINAL_PE_NAME))
        printf("\noriginal_pe_name = %s\n", (const char*)p + 4);
    if (auto* p = optHeader(data, XEX_HEADER_EXECUTION_INFO))
        printf("media_id = 0x%08X\nversion = 0x%08X\nbase_version = 0x%08X\ntitle_id = 0x%08X\nplatform/type/disc = %u %u %u/%u\n",
            be32(p), be32(p + 4), be32(p + 8), be32(p + 12), p[16], p[17], p[18], p[19]);
    if (auto* p = optHeader(data, XEX_HEADER_TLS_INFO))
        printf("tls_slots = %u\ntls_raw_data = 0x%08X\ntls_data_size = 0x%X\ntls_raw_size = 0x%X\n",
            be32(p), be32(p + 4), be32(p + 8), be32(p + 12));
    if (auto* p = optHeader(data, XEX_HEADER_DEFAULT_STACK_SIZE))
        printf("default_stack_size = 0x%X\n", be32(p));
    if (auto* p = optHeader(data, XEX_HEADER_DEFAULT_HEAP_SIZE))
        printf("default_heap_size = 0x%X\n", be32(p));
    if (auto* p = optHeader(data, XEX_HEADER_TITLE_WORKSPACE_SIZE))
        printf("title_workspace_size = 0x%X\n", be32(p));
    if (auto* p = optHeader(data, XEX_HEADER_SYSTEM_FLAGS))
        printf("system_flags = 0x%08X\n", be32(p));
    if (auto* p = optHeader(data, XEX_HEADER_ADDITIONAL_TITLE_MEMORY))
        printf("additional_title_memory = 0x%X\n", be32(p));

    if (auto* p = optHeader(data, XEX_HEADER_STATIC_LIBRARIES))
    {
        uint32_t size = be32(p);
        printf("\n[static_libraries]\n");
        for (const uint8_t* e = p + 4; e + 16 <= p + size; e += 16)
        {
            char name[9]{};
            memcpy(name, e, 8);
            printf("%-8s %u.%u.%u.%u\n", name, be16(e + 8), be16(e + 10), be16(e + 12), be16(e + 14) & 0x7FFF);
        }
    }

    auto image = Image::ParseImage(data, file.size());
    printf("\n[image]\nbase = 0x%08zX\nsize = 0x%X\nentry = 0x%08zX\n", image.base, image.size, image.entry_point);
    for (const auto& s : image.sections)
        printf("section %-8.8s base=0x%08zX size=0x%08X %s\n", s.name.c_str(), s.base, s.size,
            (s.flags & SectionFlags_Code) ? "code" : "data");

    if (FILE* f = fopen(argv[2], "wb"))
    {
        // With basic compression the buffer only spans the data+zero blocks, which can be
        // shorter than security->imageSize; stop at the end of the last section.
        size_t end = 0;
        for (const auto& s : image.sections)
            end = std::max(end, s.base - image.base + s.size);
        // The title's .reloc can claim more than the image holds (Gears of War: 0xCE4768 of 0xCE0000),
        // and fwrite past the buffer writes nothing: stop at the image, or at the blocks with basic compression.
        end = std::min(end, size_t(image.size));
        if (ffi && ffi->compressionType == XEX_COMPRESSION_BASIC)
        {
            auto* blocks = (const Xex2FileBasicCompressionBlock*)(ffi + 1);
            size_t held = 0;
            for (size_t i = 0; i < ffi->infoSize / sizeof(Xex2FileBasicCompressionInfo) - 1; i++)
                held += blocks[i].dataSize + blocks[i].zeroSize;
            end = std::min(end, held);
        }
        size_t n = fwrite(image.data.get(), 1, end, f);
        fclose(f);
        fprintf(stderr, "wrote %zu bytes (image_size 0x%X)\n", n, image.size);
    }
    else
        fprintf(stderr, "cannot open %s\n", argv[2]);

    // Imports: Xex2LoadImage byteswapped every record in place and replaced function thunks
    // with a 16-byte stub, so a variable record (type 0) carries the ordinal and a stub marks
    // the function thunk that follows it.
    auto* imports = (const Xex2ImportHeader*)optHeader(data, XEX_HEADER_IMPORT_LIBRARIES);
    if (imports)
    {
        std::vector<std::string> names;
        const char* str = (const char*)(imports + 1);
        size_t off = 0;
        for (uint32_t i = 0; i < imports->numImports; i++)
        {
            names.emplace_back(str + off);
            off += ((names.back().size() + 1) + 3) & ~3;
        }

        printf("\n[imports]\n");
        auto* lib = (const Xex2ImportLibrary*)((const char*)imports + sizeof(Xex2ImportHeader) + imports->sizeOfStringTable);
        for (size_t i = 0; i < names.size(); i++)
        {
            const std::string& lname = names[lib->name];
            const auto* table = lname == "xam.xex" ? &XamExports : lname == "xboxkrnl.exe" ? &XboxKernelExports : nullptr;
            printf("# library %s version=0x%08X min=0x%08X records=%u\n", lname.c_str(),
                (uint32_t)lib->version, (uint32_t)lib->minVersion, (uint32_t)lib->numberOfImports);

            auto* desc = (const Xex2ImportDescriptor*)(lib + 1);
            for (uint32_t k = 0; k < lib->numberOfImports; k++)
            {
                uint32_t addr = desc[k].firstThunk;
                auto* rec = (const uint8_t*)image.Find(addr);
                uint32_t v;
                memcpy(&v, rec, 4);
                if (v == 0x00000060)
                    continue; // function thunk stub, reported with its variable record
                uint32_t ordinal = v & 0xFFFF;
                bool isFunc = k + 1 < lib->numberOfImports && [&] {
                    uint32_t n;
                    memcpy(&n, image.Find(desc[k + 1].firstThunk), 4);
                    return n == 0x00000060;
                }();
                const char* nm = "?";
                if (table)
                    if (auto it = table->find(ordinal); it != table->end())
                        nm = it->second;
                printf("%s %s 0x%03X %s var=0x%08X%s", lname.c_str(), isFunc ? "func" : "var ", ordinal, nm, addr,
                    isFunc ? "" : "\n");
                if (isFunc)
                    printf(" thunk=0x%08X\n", (uint32_t)desc[k + 1].firstThunk);
            }
            lib = (const Xex2ImportLibrary*)((const char*)(lib + 1) + lib->numberOfImports * sizeof(Xex2ImportDescriptor));
        }
    }
    return 0;
}
