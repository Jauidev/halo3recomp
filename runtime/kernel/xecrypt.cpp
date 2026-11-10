// XeCrypt SHA-1, which the title uses on data it loads (the stubs returned garbage digests
// and the game crashed right after loading The Lost and Damned). The guest state layout follows
// Xenia's XECRYPT_SHA_STATE: a byte count, the five chaining words, a 64-byte block buffer.
#include <stdafx.h>
#include <kernel/function.h>

namespace
{
    struct ShaState
    {
        be<uint32_t> count;
        be<uint32_t> state[5];
        uint8_t buffer[64];
    };
    static_assert(sizeof(ShaState) == 0x58);

    uint32_t Rotate(uint32_t value, int bits)
    {
        return (value << bits) | (value >> (32 - bits));
    }

    void Transform(uint32_t state[5], const uint8_t block[64])
    {
        uint32_t w[80];
        for (int i = 0; i < 16; i++)
            w[i] = (block[i * 4] << 24) | (block[i * 4 + 1] << 16) | (block[i * 4 + 2] << 8) | block[i * 4 + 3];
        for (int i = 16; i < 80; i++)
            w[i] = Rotate(w[i - 3] ^ w[i - 8] ^ w[i - 14] ^ w[i - 16], 1);

        uint32_t a = state[0], b = state[1], c = state[2], d = state[3], e = state[4];
        for (int i = 0; i < 80; i++)
        {
            uint32_t f, k;
            if (i < 20)      { f = (b & c) | (~b & d);          k = 0x5A827999; }
            else if (i < 40) { f = b ^ c ^ d;                   k = 0x6ED9EBA1; }
            else if (i < 60) { f = (b & c) | (b & d) | (c & d); k = 0x8F1BBCDC; }
            else             { f = b ^ c ^ d;                   k = 0xCA62C1D6; }

            const uint32_t temp = Rotate(a, 5) + f + e + k + w[i];
            e = d;
            d = c;
            c = Rotate(b, 30);
            b = a;
            a = temp;
        }

        state[0] += a;
        state[1] += b;
        state[2] += c;
        state[3] += d;
        state[4] += e;
    }

    // Host-side context; the guest state is converted in and out on every call.
    struct Sha1
    {
        uint32_t state[5];
        uint32_t count;
        uint8_t buffer[64];

        void Init()
        {
            state[0] = 0x67452301;
            state[1] = 0xEFCDAB89;
            state[2] = 0x98BADCFE;
            state[3] = 0x10325476;
            state[4] = 0xC3D2E1F0;
            count = 0;
        }

        void Load(const ShaState& guest)
        {
            count = guest.count;
            for (int i = 0; i < 5; i++)
                state[i] = guest.state[i];
            memcpy(buffer, guest.buffer, sizeof(buffer));
        }

        void Store(ShaState& guest) const
        {
            guest.count = count;
            for (int i = 0; i < 5; i++)
                guest.state[i] = state[i];
            memcpy(guest.buffer, buffer, sizeof(buffer));
        }

        void Update(const uint8_t* data, uint32_t size)
        {
            while (size > 0)
            {
                const uint32_t used = count % 64;
                const uint32_t chunk = std::min(size, 64 - used);
                memcpy(buffer + used, data, chunk);
                count += chunk;
                data += chunk;
                size -= chunk;
                if (count % 64 == 0)
                    Transform(state, buffer);
            }
        }

        void Final(uint8_t digest[20])
        {
            const uint64_t bits = uint64_t(count) * 8;
            const uint8_t pad = 0x80;
            Update(&pad, 1);
            const uint8_t zero = 0;
            while (count % 64 != 56)
                Update(&zero, 1);
            uint8_t length[8];
            for (int i = 0; i < 8; i++)
                length[i] = uint8_t(bits >> (56 - i * 8));
            Update(length, 8);

            for (int i = 0; i < 5; i++)
            {
                digest[i * 4] = uint8_t(state[i] >> 24);
                digest[i * 4 + 1] = uint8_t(state[i] >> 16);
                digest[i * 4 + 2] = uint8_t(state[i] >> 8);
                digest[i * 4 + 3] = uint8_t(state[i]);
            }
        }
    };
}

void XeCryptShaInit(ShaState* guest)
{
    Sha1 sha;
    sha.Init();
    memset(guest, 0, sizeof(*guest));
    sha.Store(*guest);
}

void XeCryptShaUpdate(ShaState* guest, const uint8_t* data, uint32_t size)
{
    Sha1 sha;
    sha.Load(*guest);
    sha.Update(data, size);
    sha.Store(*guest);
}

void XeCryptShaFinal(ShaState* guest, uint8_t* digest, uint32_t digestSize)
{
    Sha1 sha;
    sha.Load(*guest);
    uint8_t full[20];
    sha.Final(full);
    memcpy(digest, full, std::min<uint32_t>(digestSize, 20));
    sha.Store(*guest);
}

// One-shot hash of up to three buffers.
void XeCryptSha(const uint8_t* input1, uint32_t size1, const uint8_t* input2, uint32_t size2,
    const uint8_t* input3, uint32_t size3, uint8_t* digest, uint32_t digestSize)
{
    Sha1 sha;
    sha.Init();
    if (input1)
        sha.Update(input1, size1);
    if (input2)
        sha.Update(input2, size2);
    if (input3)
        sha.Update(input3, size3);
    uint8_t full[20];
    sha.Final(full);
    memcpy(digest, full, std::min<uint32_t>(digestSize, 20));
}

GUEST_FUNCTION_HOOK(__imp__XeCryptShaInit, XeCryptShaInit);
GUEST_FUNCTION_HOOK(__imp__XeCryptShaUpdate, XeCryptShaUpdate);
GUEST_FUNCTION_HOOK(__imp__XeCryptShaFinal, XeCryptShaFinal);
GUEST_FUNCTION_HOOK(__imp__XeCryptSha, XeCryptSha);
