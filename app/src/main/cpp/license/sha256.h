

#pragma once
#include <cstdint>
#include <cstring>
#include <string>

namespace facegate_crypto {

static const uint32_t SHA256_K[64] = {
    0x428a2f98u, 0x71374491u, 0xb5c0fbcfu, 0xe9b5dba5u,
    0x3956c25bu, 0x59f111f1u, 0x923f82a4u, 0xab1c5ed5u,
    0xd807aa98u, 0x12835b01u, 0x243185beu, 0x550c7dc3u,
    0x72be5d74u, 0x80deb1feu, 0x9bdc06a7u, 0xc19bf174u,
    0xe49b69c1u, 0xefbe4786u, 0x0fc19dc6u, 0x240ca1ccu,
    0x2de92c6fu, 0x4a7484aau, 0x5cb0a9dcu, 0x76f988dau,
    0x983e5152u, 0xa831c66du, 0xb00327c8u, 0xbf597fc7u,
    0xc6e00bf3u, 0xd5a79147u, 0x06ca6351u, 0x14292967u,
    0x27b70a85u, 0x2e1b2138u, 0x4d2c6dfcu, 0x53380d13u,
    0x650a7354u, 0x766a0abbu, 0x81c2c92eu, 0x92722c85u,
    0xa2bfe8a1u, 0xa81a664bu, 0xc24b8b70u, 0xc76c51a3u,
    0xd192e819u, 0xd6990624u, 0xf40e3585u, 0x106aa070u,
    0x19a4c116u, 0x1e376c08u, 0x2748774cu, 0x34b0bcb5u,
    0x391c0cb3u, 0x4ed8aa4au, 0x5b9cca4fu, 0x682e6ff3u,
    0x748f82eeu, 0x78a5636fu, 0x84c87814u, 0x8cc70208u,
    0x90befffau, 0xa4506cebu, 0xbef9a3f7u, 0xc67178f2u,
};

static inline uint32_t rotr32(uint32_t x, int n) {
    return (x >> n) | (x << (32 - n));
}

struct Sha256Ctx {
    uint32_t state[8];
    uint64_t count;
    uint8_t  buf[64];
    uint32_t buf_len;
};

static void sha256_init(Sha256Ctx *ctx) {
    ctx->state[0] = 0x6a09e667u;
    ctx->state[1] = 0xbb67ae85u;
    ctx->state[2] = 0x3c6ef372u;
    ctx->state[3] = 0xa54ff53au;
    ctx->state[4] = 0x510e527fu;
    ctx->state[5] = 0x9b05688cu;
    ctx->state[6] = 0x1f83d9abu;
    ctx->state[7] = 0x5be0cd19u;
    ctx->count   = 0;
    ctx->buf_len = 0;
}

static void sha256_process_block(Sha256Ctx *ctx, const uint8_t *block) {
    uint32_t w[64];
    for (int i = 0; i < 16; i++) {
        w[i] = ((uint32_t)block[i*4    ] << 24)
             | ((uint32_t)block[i*4 + 1] << 16)
             | ((uint32_t)block[i*4 + 2] <<  8)
             | ((uint32_t)block[i*4 + 3]);
    }
    for (int i = 16; i < 64; i++) {
        uint32_t s0 = rotr32(w[i-15], 7) ^ rotr32(w[i-15], 18) ^ (w[i-15] >> 3);
        uint32_t s1 = rotr32(w[i-2],  17) ^ rotr32(w[i-2],  19) ^ (w[i-2]  >> 10);
        w[i] = w[i-16] + s0 + w[i-7] + s1;
    }

    uint32_t a = ctx->state[0], b = ctx->state[1],
             c = ctx->state[2], d = ctx->state[3],
             e = ctx->state[4], f = ctx->state[5],
             g = ctx->state[6], h = ctx->state[7];

    for (int i = 0; i < 64; i++) {
        uint32_t S1    = rotr32(e, 6) ^ rotr32(e, 11) ^ rotr32(e, 25);
        uint32_t ch    = (e & f) ^ (~e & g);
        uint32_t temp1 = h + S1 + ch + SHA256_K[i] + w[i];
        uint32_t S0    = rotr32(a, 2) ^ rotr32(a, 13) ^ rotr32(a, 22);
        uint32_t maj   = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = S0 + maj;

        h = g; g = f; f = e; e = d + temp1;
        d = c; c = b; b = a; a = temp1 + temp2;
    }

    ctx->state[0] += a; ctx->state[1] += b;
    ctx->state[2] += c; ctx->state[3] += d;
    ctx->state[4] += e; ctx->state[5] += f;
    ctx->state[6] += g; ctx->state[7] += h;
}

static void sha256_update(Sha256Ctx *ctx, const uint8_t *data, size_t len) {
    ctx->count += len;
    size_t i = 0;
    if (ctx->buf_len > 0) {
        size_t need = 64 - ctx->buf_len;
        size_t take = (len < need) ? len : need;
        memcpy(ctx->buf + ctx->buf_len, data, take);
        ctx->buf_len += (uint32_t)take;
        i += take;
        if (ctx->buf_len == 64) {
            sha256_process_block(ctx, ctx->buf);
            ctx->buf_len = 0;
        }
    }
    while (i + 64 <= len) {
        sha256_process_block(ctx, data + i);
        i += 64;
    }
    if (i < len) {
        memcpy(ctx->buf, data + i, len - i);
        ctx->buf_len = (uint32_t)(len - i);
    }
}

static void sha256_final(Sha256Ctx *ctx, uint8_t out[32]) {
    uint8_t pad[64] = {};
    uint64_t bit_count = ctx->count * 8;
    uint32_t pad_start = ctx->buf_len;


    pad[0] = 0x80;
    size_t pad_len = (pad_start < 56) ? (56 - pad_start) : (120 - pad_start);
    sha256_update(ctx, pad, pad_len);


    uint8_t len_bytes[8];
    for (int i = 7; i >= 0; i--) {
        len_bytes[i] = (uint8_t)(bit_count & 0xFF);
        bit_count >>= 8;
    }
    sha256_update(ctx, len_bytes, 8);


    for (int i = 0; i < 8; i++) {
        out[i*4    ] = (uint8_t)(ctx->state[i] >> 24);
        out[i*4 + 1] = (uint8_t)(ctx->state[i] >> 16);
        out[i*4 + 2] = (uint8_t)(ctx->state[i] >>  8);
        out[i*4 + 3] = (uint8_t)(ctx->state[i]);
    }
}

static const char HEX_CHARS[] = "0123456789abcdef";

static inline std::string bytes_to_hex(const uint8_t *b, size_t n) {
    std::string out;
    out.reserve(n * 2);
    for (size_t i = 0; i < n; i++) {
        out += HEX_CHARS[(b[i] >> 4) & 0xF];
        out += HEX_CHARS[ b[i]       & 0xF];
    }
    return out;
}

static inline std::string sha256_hex(const void *data, size_t len) {
    Sha256Ctx ctx;
    sha256_init(&ctx);
    sha256_update(&ctx, reinterpret_cast<const uint8_t *>(data),  len);
    uint8_t digest[32];
    sha256_final(&ctx, digest);
    return bytes_to_hex(digest, 32);
}

static inline std::string sha256_hex(const std::string &s) {
    return sha256_hex(s.data(), s.size());
}

static inline std::string hmac_sha256_hex(const std::string &key,
                                           const std::string &msg) {

    uint8_t k[64] = {};
    if (key.size() > 64) {

        Sha256Ctx kctx;
        sha256_init(&kctx);
        sha256_update(&kctx,
            reinterpret_cast<const uint8_t *>(key.data()), key.size());
        sha256_final(&kctx, k);
    } else {
        memcpy(k, key.data(), key.size());
    }

    uint8_t k_ipad[64], k_opad[64];
    for (int i = 0; i < 64; i++) {
        k_ipad[i] = k[i] ^ 0x36u;
        k_opad[i] = k[i] ^ 0x5Cu;
    }


    Sha256Ctx inner;
    sha256_init(&inner);
    sha256_update(&inner, k_ipad, 64);
    sha256_update(&inner,
        reinterpret_cast<const uint8_t *>(msg.data()), msg.size());
    uint8_t inner_hash[32];
    sha256_final(&inner, inner_hash);


    Sha256Ctx outer;
    sha256_init(&outer);
    sha256_update(&outer, k_opad, 64);
    sha256_update(&outer, inner_hash, 32);
    uint8_t result[32];
    sha256_final(&outer, result);

    return bytes_to_hex(result, 32);
}

static inline bool hex_eq(const std::string &a, const std::string &b) {
    if (a.size() != b.size()) return false;
    uint8_t diff = 0;
    for (size_t i = 0; i < a.size(); i++) {
        diff |= (uint8_t)(a[i] ^ b[i]);
    }
    return diff == 0;
}

}
