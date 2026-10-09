#pragma once

#include <cstddef>
#include <cstdint>
#include <cstring>

// OpenSSL SHA256() replacement so the Windows host thunkgen does not need libcrypto.
// Output matches SHA256() from OpenSSL for the thunk id hashes.

inline constexpr int SHA256_DIGEST_LENGTH = 32;

inline void SHA256(const unsigned char* data, size_t len, unsigned char* out) {
    static const uint32_t k[64] = {
        0x428a2f98, 0x71374491, 0xb5c0fbcf, 0xe9b5dba5, 0x3956c25b, 0x59f111f1, 0x923f82a4, 0xab1c5ed5,
        0xd807aa98, 0x12835b01, 0x243185be, 0x550c7dc3, 0x72be5d74, 0x80deb1fe, 0x9bdc06a7, 0xc19bf174,
        0xe49b69c1, 0xefbe4786, 0x0fc19dc6, 0x240ca1cc, 0x2de92c6f, 0x4a7484aa, 0x5cb0a9dc, 0x76f988da,
        0x983e5152, 0xa831c66d, 0xb00327c8, 0xbf597fc7, 0xc6e00bf3, 0xd5a79147, 0x06ca6351, 0x14292967,
        0x27b70a85, 0x2e1b2138, 0x4d2c6dfc, 0x53380d13, 0x650a7354, 0x766a0abb, 0x81c2c92e, 0x92722c85,
        0xa2bfe8a1, 0xa81a664b, 0xc24b8b70, 0xc76c51a3, 0xd192e819, 0xd6990624, 0xf40e3585, 0x106aa070,
        0x19a4c116, 0x1e376c08, 0x2748774c, 0x34b0bcb5, 0x391c0cb3, 0x4ed8aa4a, 0x5b9cca4f, 0x682e6ff3,
        0x748f82ee, 0x78a5636f, 0x84c87814, 0x8cc70208, 0x90befffa, 0xa4506ceb, 0xbef9a3f7, 0xc67178f2};

    auto rotr = [](uint32_t x, uint32_t n) { return (x >> n) | (x << (32 - n)); };

    uint32_t h0 = 0x6a09e667, h1 = 0xbb67ae85, h2 = 0x3c6ef372, h3 = 0xa54ff53a;
    uint32_t h4 = 0x510e527f, h5 = 0x9b05688c, h6 = 0x1f83d9ab, h7 = 0x5be0cd19;

    const uint64_t bit_len = static_cast<uint64_t>(len) * 8;
    const size_t padded = ((len + 9 + 63) / 64) * 64;
    // Thunk names are short. A fixed buffer covers every hash this generator makes.
    unsigned char buf[512];
    if (padded > sizeof(buf)) {
        std::memset(out, 0, SHA256_DIGEST_LENGTH);
        return;
    }
    std::memset(buf, 0, padded);
    if (len) {
        std::memcpy(buf, data, len);
    }
    buf[len] = 0x80;
    for (int i = 0; i < 8; ++i) {
        buf[padded - 1 - i] = static_cast<unsigned char>(bit_len >> (8 * i));
    }

    for (size_t off = 0; off < padded; off += 64) {
        uint32_t w[64];
        for (int i = 0; i < 16; ++i) {
            w[i] = (uint32_t)buf[off + i * 4] << 24 | (uint32_t)buf[off + i * 4 + 1] << 16 |
                   (uint32_t)buf[off + i * 4 + 2] << 8 | (uint32_t)buf[off + i * 4 + 3];
        }
        for (int i = 16; i < 64; ++i) {
            const uint32_t s0 = rotr(w[i - 15], 7) ^ rotr(w[i - 15], 18) ^ (w[i - 15] >> 3);
            const uint32_t s1 = rotr(w[i - 2], 17) ^ rotr(w[i - 2], 19) ^ (w[i - 2] >> 10);
            w[i] = w[i - 16] + s0 + w[i - 7] + s1;
        }
        uint32_t a = h0, b = h1, c = h2, d = h3, e = h4, f = h5, g = h6, h = h7;
        for (int i = 0; i < 64; ++i) {
            const uint32_t S1 = rotr(e, 6) ^ rotr(e, 11) ^ rotr(e, 25);
            const uint32_t ch = (e & f) ^ (~e & g);
            const uint32_t t1 = h + S1 + ch + k[i] + w[i];
            const uint32_t S0 = rotr(a, 2) ^ rotr(a, 13) ^ rotr(a, 22);
            const uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
            const uint32_t t2 = S0 + maj;
            h = g;
            g = f;
            f = e;
            e = d + t1;
            d = c;
            c = b;
            b = a;
            a = t1 + t2;
        }
        h0 += a;
        h1 += b;
        h2 += c;
        h3 += d;
        h4 += e;
        h5 += f;
        h6 += g;
        h7 += h;
    }

    const uint32_t hs[8] = {h0, h1, h2, h3, h4, h5, h6, h7};
    for (int i = 0; i < 8; ++i) {
        out[i * 4] = static_cast<unsigned char>(hs[i] >> 24);
        out[i * 4 + 1] = static_cast<unsigned char>(hs[i] >> 16);
        out[i * 4 + 2] = static_cast<unsigned char>(hs[i] >> 8);
        out[i * 4 + 3] = static_cast<unsigned char>(hs[i]);
    }
}
