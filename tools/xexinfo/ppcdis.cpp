// ppcdis: disassembles ranges of an XEX with XenonUtils' disassembler (the one XenonRecomp uses).
// Usage: ppcdis <default.xex> <addr> <count> [<addr> <count> ...]
#include <cstdio>
#include <cstdlib>
#include <file.h>
#include <image.h>
#include <disasm.h>
#include <byteswap.h>

int main(int argc, char** argv)
{
    if (argc < 4)
    {
        fprintf(stderr, "usage: ppcdis <xex> <addr> <count> ...\n");
        return 1;
    }
    const auto file = LoadFile(argv[1]);
    auto image = Image::ParseImage(file.data(), file.size());
    for (int a = 2; a + 1 < argc; a += 2)
    {
        uint32_t addr = strtoul(argv[a], nullptr, 16);
        uint32_t count = strtoul(argv[a + 1], nullptr, 0);
        for (uint32_t i = 0; i < count; i++, addr += 4)
        {
            auto* p = (const uint32_t*)image.Find(addr);
            ppc_insn insn;
            ppc::Disassemble(p, addr, insn);
            uint32_t raw = ByteSwap(*p);
            printf("%08X  %08X  %s %s\n", addr, raw, insn.opcode ? insn.opcode->name : "???", insn.op_str);
        }
        printf("\n");
    }
    return 0;
}
