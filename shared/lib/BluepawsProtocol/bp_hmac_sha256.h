/*
  Bluepaws V4 — small SHA-256 / HMAC-SHA256 helper for embedded TLV packets.

  Used by bench firmware to generate the TLV v1.1 8-byte truncated
  HMAC-SHA256 authentication tag:

    first 8 bytes of HMAC-SHA256(device_key, header + TLVs)
*/

#ifndef BP_HMAC_SHA256_H
#define BP_HMAC_SHA256_H

#include <stdint.h>
#include <string.h>

#define BP_SHA256_BLOCK_SIZE 64
#define BP_SHA256_DIGEST_SIZE 32

typedef struct {
    uint32_t state[8];
    uint64_t bitlen;
    uint8_t data[64];
    uint32_t datalen;
} bp_sha256_ctx_t;

static inline uint32_t bp_rotr32(uint32_t x, uint8_t n) {
    return (x >> n) | (x << (32U - n));
}

static inline uint32_t bp_load_be32(const uint8_t *p) {
    return ((uint32_t)p[0] << 24) | ((uint32_t)p[1] << 16) | ((uint32_t)p[2] << 8) | p[3];
}

static inline void bp_store_be32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)(v >> 24);
    p[1] = (uint8_t)(v >> 16);
    p[2] = (uint8_t)(v >> 8);
    p[3] = (uint8_t)v;
}

static const uint32_t BP_SHA256_K[64] = {
    0x428a2f98UL, 0x71374491UL, 0xb5c0fbcfUL, 0xe9b5dba5UL,
    0x3956c25bUL, 0x59f111f1UL, 0x923f82a4UL, 0xab1c5ed5UL,
    0xd807aa98UL, 0x12835b01UL, 0x243185beUL, 0x550c7dc3UL,
    0x72be5d74UL, 0x80deb1feUL, 0x9bdc06a7UL, 0xc19bf174UL,
    0xe49b69c1UL, 0xefbe4786UL, 0x0fc19dc6UL, 0x240ca1ccUL,
    0x2de92c6fUL, 0x4a7484aaUL, 0x5cb0a9dcUL, 0x76f988daUL,
    0x983e5152UL, 0xa831c66dUL, 0xb00327c8UL, 0xbf597fc7UL,
    0xc6e00bf3UL, 0xd5a79147UL, 0x06ca6351UL, 0x14292967UL,
    0x27b70a85UL, 0x2e1b2138UL, 0x4d2c6dfcUL, 0x53380d13UL,
    0x650a7354UL, 0x766a0abbUL, 0x81c2c92eUL, 0x92722c85UL,
    0xa2bfe8a1UL, 0xa81a664bUL, 0xc24b8b70UL, 0xc76c51a3UL,
    0xd192e819UL, 0xd6990624UL, 0xf40e3585UL, 0x106aa070UL,
    0x19a4c116UL, 0x1e376c08UL, 0x2748774cUL, 0x34b0bcb5UL,
    0x391c0cb3UL, 0x4ed8aa4aUL, 0x5b9cca4fUL, 0x682e6ff3UL,
    0x748f82eeUL, 0x78a5636fUL, 0x84c87814UL, 0x8cc70208UL,
    0x90befffaUL, 0xa4506cebUL, 0xbef9a3f7UL, 0xc67178f2UL
};

static inline void bp_sha256_transform(bp_sha256_ctx_t *ctx, const uint8_t data[64]) {
    uint32_t a, b, c, d, e, f, g, h;
    uint32_t m[64];

    for (uint8_t i = 0; i < 16; i++) {
        m[i] = bp_load_be32(&data[i * 4]);
    }
    for (uint8_t i = 16; i < 64; i++) {
        uint32_t s0 = bp_rotr32(m[i - 15], 7) ^ bp_rotr32(m[i - 15], 18) ^ (m[i - 15] >> 3);
        uint32_t s1 = bp_rotr32(m[i - 2], 17) ^ bp_rotr32(m[i - 2], 19) ^ (m[i - 2] >> 10);
        m[i] = m[i - 16] + s0 + m[i - 7] + s1;
    }

    a = ctx->state[0];
    b = ctx->state[1];
    c = ctx->state[2];
    d = ctx->state[3];
    e = ctx->state[4];
    f = ctx->state[5];
    g = ctx->state[6];
    h = ctx->state[7];

    for (uint8_t i = 0; i < 64; i++) {
        uint32_t s1 = bp_rotr32(e, 6) ^ bp_rotr32(e, 11) ^ bp_rotr32(e, 25);
        uint32_t ch = (e & f) ^ ((~e) & g);
        uint32_t temp1 = h + s1 + ch + BP_SHA256_K[i] + m[i];
        uint32_t s0 = bp_rotr32(a, 2) ^ bp_rotr32(a, 13) ^ bp_rotr32(a, 22);
        uint32_t maj = (a & b) ^ (a & c) ^ (b & c);
        uint32_t temp2 = s0 + maj;

        h = g;
        g = f;
        f = e;
        e = d + temp1;
        d = c;
        c = b;
        b = a;
        a = temp1 + temp2;
    }

    ctx->state[0] += a;
    ctx->state[1] += b;
    ctx->state[2] += c;
    ctx->state[3] += d;
    ctx->state[4] += e;
    ctx->state[5] += f;
    ctx->state[6] += g;
    ctx->state[7] += h;
}

static inline void bp_sha256_init(bp_sha256_ctx_t *ctx) {
    ctx->datalen = 0;
    ctx->bitlen = 0;
    ctx->state[0] = 0x6a09e667UL;
    ctx->state[1] = 0xbb67ae85UL;
    ctx->state[2] = 0x3c6ef372UL;
    ctx->state[3] = 0xa54ff53aUL;
    ctx->state[4] = 0x510e527fUL;
    ctx->state[5] = 0x9b05688cUL;
    ctx->state[6] = 0x1f83d9abUL;
    ctx->state[7] = 0x5be0cd19UL;
}

static inline void bp_sha256_update(bp_sha256_ctx_t *ctx, const uint8_t *data, uint32_t len) {
    for (uint32_t i = 0; i < len; i++) {
        ctx->data[ctx->datalen++] = data[i];
        if (ctx->datalen == 64) {
            bp_sha256_transform(ctx, ctx->data);
            ctx->bitlen += 512;
            ctx->datalen = 0;
        }
    }
}

static inline void bp_sha256_final(bp_sha256_ctx_t *ctx, uint8_t hash[32]) {
    uint32_t i = ctx->datalen;

    ctx->data[i++] = 0x80;
    if (i > 56) {
        while (i < 64) ctx->data[i++] = 0x00;
        bp_sha256_transform(ctx, ctx->data);
        memset(ctx->data, 0, 56);
    } else {
        while (i < 56) ctx->data[i++] = 0x00;
    }

    ctx->bitlen += (uint64_t)ctx->datalen * 8ULL;
    for (uint8_t j = 0; j < 8; j++) {
        ctx->data[63 - j] = (uint8_t)(ctx->bitlen >> (j * 8));
    }
    bp_sha256_transform(ctx, ctx->data);

    for (uint8_t j = 0; j < 8; j++) {
        bp_store_be32(&hash[j * 4], ctx->state[j]);
    }
}

static inline void bp_hmac_sha256(const uint8_t *key, uint32_t key_len,
                                  const uint8_t *message, uint32_t message_len,
                                  uint8_t out[32]) {
    uint8_t key_block[BP_SHA256_BLOCK_SIZE];
    uint8_t inner_hash[BP_SHA256_DIGEST_SIZE];
    uint8_t ipad[BP_SHA256_BLOCK_SIZE];
    uint8_t opad[BP_SHA256_BLOCK_SIZE];
    bp_sha256_ctx_t ctx;

    memset(key_block, 0, sizeof(key_block));
    if (key_len > BP_SHA256_BLOCK_SIZE) {
        bp_sha256_init(&ctx);
        bp_sha256_update(&ctx, key, key_len);
        bp_sha256_final(&ctx, key_block);
    } else if (key_len > 0) {
        memcpy(key_block, key, key_len);
    }

    for (uint8_t i = 0; i < BP_SHA256_BLOCK_SIZE; i++) {
        ipad[i] = key_block[i] ^ 0x36;
        opad[i] = key_block[i] ^ 0x5c;
    }

    bp_sha256_init(&ctx);
    bp_sha256_update(&ctx, ipad, BP_SHA256_BLOCK_SIZE);
    bp_sha256_update(&ctx, message, message_len);
    bp_sha256_final(&ctx, inner_hash);

    bp_sha256_init(&ctx);
    bp_sha256_update(&ctx, opad, BP_SHA256_BLOCK_SIZE);
    bp_sha256_update(&ctx, inner_hash, BP_SHA256_DIGEST_SIZE);
    bp_sha256_final(&ctx, out);

    memset(key_block, 0, sizeof(key_block));
    memset(inner_hash, 0, sizeof(inner_hash));
}

#endif
