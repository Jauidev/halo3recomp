// Kernel imports Halo 3 and its Waves audio DLLs call that the runtime only had as generated
// stubs (tools/gen_stubs.py): those return 0 without writing their outputs, which leaves the
// caller with garbage (a sprintf buffer never written, a spinlock never taken, an AES key never
// expanded). Layouts and conventions follow Xenia's xboxkrnl (strings, crypt, threading).
#include <stdafx.h>
#include <kernel/function.h>
#include <kernel/memory.h>
#include <cpu/guest_thread.h>
#include <random>
#include "../../third_party/XenonRecomp/thirdparty/tiny-AES-c/aes.hpp"

namespace
{
    uint8_t* Host(uint32_t address)
    {
        return reinterpret_cast<uint8_t*>(g_memory.Translate(address));
    }

    uint64_t Load64(uint32_t address)
    {
        uint64_t value;
        memcpy(&value, Host(address), 8);
        return ByteSwap(value);
    }

    void Store32(uint32_t address, uint32_t value)
    {
        value = ByteSwap(value);
        memcpy(Host(address), &value, 4);
    }

    // The variable arguments of a guest printf. Called directly, the first eight 8-byte slots are
    // r3-r10 and the rest sit in the caller's parameter save area (r1 + 0x10 + 8 * slot: Xenia
    // reads the ninth argument's low word at r1 + 0x54). A va_list points at consecutive 8-byte
    // slots (the callee spilled its registers there). A double takes a whole slot, as raw bits.
    struct GuestArgs
    {
        const PPCContext* ctx = nullptr;
        uint32_t slot = 0;
        uint32_t list = 0;

        uint64_t Next()
        {
            if (!ctx)
            {
                const uint64_t value = Load64(list);
                list += 8;
                return value;
            }
            const uint32_t i = slot++;
            switch (i)
            {
            case 0: return ctx->r3.u64;
            case 1: return ctx->r4.u64;
            case 2: return ctx->r5.u64;
            case 3: return ctx->r6.u64;
            case 4: return ctx->r7.u64;
            case 5: return ctx->r8.u64;
            case 6: return ctx->r9.u64;
            case 7: return ctx->r10.u64;
            default: return Load64(ctx->r1.u32 + 0x10 + 8 * i);
            }
        }
    };

    template<typename T>
    void AppendFormatted(std::string& out, const std::string& spec, T value)
    {
        const int n = snprintf(nullptr, 0, spec.c_str(), value);
        if (n <= 0)
            return;
        const size_t at = out.size();
        out.resize(at + size_t(n) + 1);
        snprintf(out.data() + at, size_t(n) + 1, spec.c_str(), value);
        out.resize(at + size_t(n));
    }

    // The MSVC CRT's printf for narrow strings: %s narrow, %S/%ls wide (big-endian UTF-16 in
    // guest memory), %hs narrow, I64/ll 64-bit, h/hh short/char.
    std::string FormatGuest(const char* format, GuestArgs& args)
    {
        enum Length { kInt, kChar, kShort, kLong, kLongLong };
        std::string out;
        const char* p = format;
        while (*p)
        {
            if (*p != '%')
            {
                out += *p++;
                continue;
            }
            const char* start = p++;
            if (*p == '%')
            {
                out += '%';
                p++;
                continue;
            }
            std::string spec = "%";
            while (*p && strchr("-+ #0", *p))
                spec += *p++;
            if (*p == '*')
            {
                spec += std::to_string(int32_t(args.Next()));
                p++;
            }
            else
            {
                while (isdigit((unsigned char)*p))
                    spec += *p++;
            }
            if (*p == '.')
            {
                spec += *p++;
                if (*p == '*')
                {
                    spec += std::to_string(std::max(int32_t(args.Next()), 0));
                    p++;
                }
                else
                {
                    while (isdigit((unsigned char)*p))
                        spec += *p++;
                }
            }
            Length length = kInt;
            if (*p == 'h')
            {
                p++;
                length = kShort;
                if (*p == 'h')
                {
                    p++;
                    length = kChar;
                }
            }
            else if (*p == 'l')
            {
                p++;
                length = kLong;
                if (*p == 'l')
                {
                    p++;
                    length = kLongLong;
                }
            }
            else if (p[0] == 'I' && p[1] == '6' && p[2] == '4')
            {
                p += 3;
                length = kLongLong;
            }
            else if (p[0] == 'I' && p[1] == '3' && p[2] == '2')
            {
                p += 3;
                length = kLong;
            }
            else if (*p == 'L' || *p == 'q' || *p == 'j')
            {
                p++;
                length = kLongLong;
            }
            else if (*p == 'I' || *p == 'z' || *p == 't')
            {
                p++;
                length = kLong; // 32-bit on the console
            }
            const char conversion = *p;
            if (!conversion)
            {
                out.append(start, p);
                break;
            }
            p++;
            switch (conversion)
            {
            case 'd':
            case 'i':
            {
                const uint64_t raw = args.Next();
                const int64_t value = length == kLongLong ? int64_t(raw) : length == kShort ? int64_t(int16_t(raw))
                    : length == kChar ? int64_t(int8_t(raw)) : int64_t(int32_t(raw));
                AppendFormatted(out, spec + "lld", (long long)value);
                break;
            }
            case 'u':
            case 'x':
            case 'X':
            case 'o':
            {
                const uint64_t raw = args.Next();
                const uint64_t value = length == kLongLong ? raw : length == kShort ? uint64_t(uint16_t(raw))
                    : length == kChar ? uint64_t(uint8_t(raw)) : uint64_t(uint32_t(raw));
                AppendFormatted(out, spec + "ll" + conversion, (unsigned long long)value);
                break;
            }
            case 'p':
                AppendFormatted(out, "%08X", uint32_t(args.Next()));
                break;
            case 'c':
            case 'C':
            {
                const uint32_t raw = uint32_t(args.Next());
                const bool wide = conversion == 'C' ? length != kShort : length == kLong;
                const uint32_t ch = wide ? (raw & 0xFFFF) : (raw & 0xFF);
                AppendFormatted(out, spec + "c", int(ch < 0x100 ? ch : '?'));
                break;
            }
            case 's':
            case 'S':
            case 'Z':
            {
                const uint32_t address = uint32_t(args.Next());
                const bool wide = conversion == 'S' ? length != kShort : length == kLong;
                std::string text;
                if (!address)
                {
                    text = "(null)";
                }
                else if (wide)
                {
                    for (uint32_t a = address;; a += 2)
                    {
                        uint16_t ch;
                        memcpy(&ch, Host(a), 2);
                        ch = ByteSwap(ch);
                        if (!ch)
                            break;
                        text += ch < 0x80 ? char(ch) : '?';
                    }
                }
                else
                {
                    text = reinterpret_cast<const char*>(Host(address));
                }
                AppendFormatted(out, spec + "s", text.c_str());
                break;
            }
            case 'f':
            case 'F':
            case 'e':
            case 'E':
            case 'g':
            case 'G':
            case 'a':
            case 'A':
            {
                const uint64_t raw = args.Next();
                double value;
                memcpy(&value, &raw, 8);
                AppendFormatted(out, spec + conversion, value);
                break;
            }
            case 'n':
            {
                const uint32_t address = uint32_t(args.Next());
                if (address)
                    Store32(address, uint32_t(out.size()));
                break;
            }
            default:
                out.append(start, p);
                break;
            }
        }
        return out;
    }
}

// int sprintf(char* buffer, const char* format, ...)
static uint32_t Kernel_sprintf(uint32_t buffer, uint32_t format)
{
    GuestArgs args;
    args.ctx = GetPPCContext();
    args.slot = 2;
    const std::string text = FormatGuest(reinterpret_cast<const char*>(Host(format)), args);
    memcpy(Host(buffer), text.c_str(), text.size() + 1);
    return uint32_t(text.size());
}

// int _vsnprintf(char* buffer, size_t count, const char* format, va_list list): the MSVC
// semantics, no terminator when the text fills the buffer, -1 when it does not fit.
static uint32_t Kernel_vsnprintf(uint32_t buffer, uint32_t count, uint32_t format, uint32_t list)
{
    GuestArgs args;
    args.list = list;
    const std::string text = FormatGuest(reinterpret_cast<const char*>(Host(format)), args);
    if (text.size() < count)
    {
        memcpy(Host(buffer), text.c_str(), text.size() + 1);
        return uint32_t(text.size());
    }
    if (count)
        memcpy(Host(buffer), text.data(), count);
    return text.size() == count ? count : uint32_t(-1);
}

// Same word as KeAcquireSpinLockAtRaisedIrql (imports.cpp): 0 free, the owner's r13 held.
static uint32_t KeTryToAcquireSpinLockAtRaisedIrql(uint32_t spinLock)
{
    auto& word = AtomicRef(*reinterpret_cast<uint32_t*>(Host(spinLock)));
    uint32_t expected = 0;
    return word.compare_exchange_strong(expected, GetPPCContext()->r13.u32, std::memory_order_acquire) ? 1 : 0;
}

static uint32_t NtYieldExecution()
{
    std::this_thread::yield();
    return 0;
}

// SIZE_T RtlCompareMemory(const void* a, const void* b, SIZE_T length): the matching prefix.
static uint32_t Kernel_RtlCompareMemory(uint32_t a, uint32_t b, uint32_t length)
{
    const uint8_t* x = Host(a);
    const uint8_t* y = Host(b);
    uint32_t i = 0;
    while (i < length && x[i] == y[i])
        i++;
    return i;
}

// The whole guest space is mapped here; the first 64 KB stay unmapped on the console.
static uint32_t MmIsAddressValid(uint32_t address)
{
    return address >= 0x10000 ? 1 : 0;
}

// The PE headers sit at the image base in memory (the loader copies the whole image).
static uint32_t RtlImageNtHeader(uint32_t imageBase)
{
    if (imageBase < 0x10000)
        return 0;
    const uint8_t* image = Host(imageBase);
    if (image[0] != 'M' || image[1] != 'Z')
        return 0;
    uint32_t lfanew;
    memcpy(&lfanew, image + 0x3C, 4); // little-endian, as PE headers are
    if (lfanew == 0 || lfanew > 0x1000)
        return 0;
    const uint8_t* nt = image + lfanew;
    if (nt[0] != 'P' || nt[1] != 'E' || nt[2] != 0 || nt[3] != 0)
        return 0;
    return imageBase + lfanew;
}

// XAUDIO speaker configuration: Xenia's answer, stereo/digital.
static uint32_t XAudioGetSpeakerConfig(uint32_t config)
{
    if (config)
        Store32(config, 0x00010001);
    return 0;
}

static uint32_t XamUserCheckPrivilege(uint32_t userIndex, uint32_t privilege, uint32_t result)
{
    if (result)
        Store32(result, 1);
    return 0;
}

// Thread objects by id: only the calling thread is known here (OpenThread(GetCurrentThreadId())
// in the Waves DLLs, then QueueUserAPC/SetThreadPriority on it). The object is the current-thread
// pseudo handle, which the runtime's thread functions accept; ObOpenObjectByPointer hands it back
// as the handle (ObReferenceObjectByHandle does the reverse).
static uint32_t ObLookupThreadByThreadId(uint32_t threadId, uint32_t object)
{
    if (threadId == GuestThread::GetCurrentThreadId())
    {
        Store32(object, CURRENT_THREAD_HANDLE);
        return 0;
    }
    LOGF_WARNING("thread id 0x{:X}: not the calling thread, not found", threadId);
    Store32(object, 0);
    return 0xC000000B; // STATUS_INVALID_CID
}

static uint32_t ObLookupAnyThreadByThreadId(uint32_t threadId, uint32_t object)
{
    return ObLookupThreadByThreadId(threadId, object);
}

static uint32_t ObOpenObjectByPointer(uint32_t object, uint32_t handle)
{
    Store32(handle, object);
    return 0;
}

static uint32_t KeSetDisableBoostThread(uint32_t thread, uint32_t disable)
{
    return 0; // the previous setting: boost was on
}

// XECRYPT_AES_STATE is 0x160 bytes of the guest's; it holds tiny-AES's expanded key here (the
// title only hands the state back to XeCryptAes*).
static_assert(sizeof(AES_ctx) <= 0x160);

static void XeCryptAesKey(uint32_t state, uint32_t key)
{
    AES_ctx ctx;
    AES_init_ctx(&ctx, Host(key));
    memcpy(Host(state), &ctx, sizeof(ctx));
}

static void XeCryptAesEcb(uint32_t state, uint32_t input, uint32_t output, uint32_t encrypt)
{
    AES_ctx ctx;
    memcpy(&ctx, Host(state), sizeof(ctx));
    uint8_t block[16];
    memcpy(block, Host(input), 16);
    if (encrypt)
        AES_ECB_encrypt(&ctx, block);
    else
        AES_ECB_decrypt(&ctx, block);
    memcpy(Host(output), block, 16);
}

// feed: the 16-byte chaining value, in and out.
static void XeCryptAesCbc(uint32_t state, uint32_t input, uint32_t size, uint32_t output, uint32_t feed, uint32_t encrypt)
{
    const size_t length = size & ~15u;
    if (!length)
        return;
    AES_ctx ctx;
    memcpy(&ctx, Host(state), sizeof(ctx));
    AES_ctx_set_iv(&ctx, Host(feed));
    uint8_t* out = Host(output);
    if (output != input)
        memmove(out, Host(input), length);
    if (encrypt)
    {
        AES_CBC_encrypt_buffer(&ctx, out, length);
        memcpy(Host(feed), out + length - 16, 16);
    }
    else
    {
        uint8_t last[16];
        memcpy(last, out + length - 16, 16);
        AES_CBC_decrypt_buffer(&ctx, out, length);
        memcpy(Host(feed), last, 16);
    }
}

static void XeCryptRandom(uint32_t buffer, uint32_t size)
{
    static std::mutex s_mutex;
    static std::mt19937 s_engine{ std::random_device{}() };
    std::lock_guard lock(s_mutex);
    uint8_t* out = Host(buffer);
    for (uint32_t i = 0; i < size; i++)
        out[i] = uint8_t(s_engine());
}

// Signatures (the title's own content, saves): there are no console keys here, so every
// signature checks out, as on Xenia with its signature checks off.
static uint32_t XeCryptBnQwNeRsaPubCrypt(uint32_t input, uint32_t output, uint32_t key)
{
    return 1;
}

static uint32_t XeCryptBnDwLePkcs1Verify(uint32_t hash, uint32_t signature, uint32_t size)
{
    return 1;
}

static void XeCryptBnDwLePkcs1Format(uint32_t hash, uint32_t format, uint32_t signature, uint32_t size)
{
    if (signature && size)
        memset(Host(signature), 0, size);
}

// Big numbers between the two orders: each 64-bit word byte-reversed (Xenia: copy_and_swap).
static void XeCryptBnQw_SwapDwQwLeBe(uint32_t input, uint32_t output, uint32_t count)
{
    std::vector<uint64_t> words(count);
    memcpy(words.data(), Host(input), size_t(count) * 8);
    for (uint64_t& w : words)
        w = ByteSwap(w);
    memcpy(Host(output), words.data(), size_t(count) * 8);
}

// No voice chat: no voice object (Xenia answers ERROR_ACCESS_DENIED the same way). The stub
// returned success without writing the handle.
static uint32_t XamVoiceCreate(uint32_t unknown1, uint32_t unknown2, uint32_t voice)
{
    if (voice)
        Store32(voice, 0);
    return 5; // ERROR_ACCESS_DENIED
}

GUEST_FUNCTION_HOOK(__imp__sprintf, Kernel_sprintf);
GUEST_FUNCTION_HOOK(__imp__XamVoiceCreate, XamVoiceCreate);
GUEST_FUNCTION_HOOK(__imp___vsnprintf, Kernel_vsnprintf);
GUEST_FUNCTION_HOOK(__imp__KeTryToAcquireSpinLockAtRaisedIrql, KeTryToAcquireSpinLockAtRaisedIrql);
GUEST_FUNCTION_HOOK(__imp__NtYieldExecution, NtYieldExecution);
GUEST_FUNCTION_HOOK(__imp__RtlCompareMemory, Kernel_RtlCompareMemory);
GUEST_FUNCTION_HOOK(__imp__MmIsAddressValid, MmIsAddressValid);
GUEST_FUNCTION_HOOK(__imp__RtlImageNtHeader, RtlImageNtHeader);
GUEST_FUNCTION_HOOK(__imp__XAudioGetSpeakerConfig, XAudioGetSpeakerConfig);
GUEST_FUNCTION_HOOK(__imp__XamUserCheckPrivilege, XamUserCheckPrivilege);
GUEST_FUNCTION_HOOK(__imp__ObLookupThreadByThreadId, ObLookupThreadByThreadId);
GUEST_FUNCTION_HOOK(__imp__ObLookupAnyThreadByThreadId, ObLookupAnyThreadByThreadId);
GUEST_FUNCTION_HOOK(__imp__ObOpenObjectByPointer, ObOpenObjectByPointer);
GUEST_FUNCTION_HOOK(__imp__KeSetDisableBoostThread, KeSetDisableBoostThread);
GUEST_FUNCTION_HOOK(__imp__XeCryptAesKey, XeCryptAesKey);
GUEST_FUNCTION_HOOK(__imp__XeCryptAesEcb, XeCryptAesEcb);
GUEST_FUNCTION_HOOK(__imp__XeCryptAesCbc, XeCryptAesCbc);
GUEST_FUNCTION_HOOK(__imp__XeCryptRandom, XeCryptRandom);
GUEST_FUNCTION_HOOK(__imp__XeCryptBnQwNeRsaPubCrypt, XeCryptBnQwNeRsaPubCrypt);
GUEST_FUNCTION_HOOK(__imp__XeCryptBnDwLePkcs1Verify, XeCryptBnDwLePkcs1Verify);
GUEST_FUNCTION_HOOK(__imp__XeCryptBnDwLePkcs1Format, XeCryptBnDwLePkcs1Format);
GUEST_FUNCTION_HOOK(__imp__XeCryptBnQw_SwapDwQwLeBe, XeCryptBnQw_SwapDwQwLeBe);
