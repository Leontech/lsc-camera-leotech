/* Compact MD5 (RFC 1321), public-domain style implementation. LEO 2026-11-24 */
#include <string.h>
#include "md5.h"
#define F(x,y,z) ((z) ^ ((x) & ((y) ^ (z))))
#define G(x,y,z) ((y) ^ ((z) & ((x) ^ (y))))
#define H(x,y,z) ((x) ^ (y) ^ (z))
#define I(x,y,z) ((y) ^ ((x) | ~(z)))
#define STEP(f,a,b,c,d,x,t,s) \
    (a) += f((b),(c),(d)) + (x) + (t); (a) = (((a) << (s)) | (((a) & 0xffffffff) >> (32 - (s)))); (a) += (b);
static const uint8_t *body(md5_ctx *ctx, const uint8_t *p, size_t size)
{
    uint32_t a, b, c, d, sa, sb, sc, sd, X[16];
    a = ctx->a; b = ctx->b; c = ctx->c; d = ctx->d;
    do {
        for (int i = 0; i < 16; i++)
            X[i] = (uint32_t)p[i*4] | ((uint32_t)p[i*4+1] << 8) | ((uint32_t)p[i*4+2] << 16) | ((uint32_t)p[i*4+3] << 24);
        sa = a; sb = b; sc = c; sd = d;
        STEP(F,a,b,c,d,X[0],0xd76aa478,7)  STEP(F,d,a,b,c,X[1],0xe8c7b756,12) STEP(F,c,d,a,b,X[2],0x242070db,17) STEP(F,b,c,d,a,X[3],0xc1bdceee,22)
        STEP(F,a,b,c,d,X[4],0xf57c0faf,7)  STEP(F,d,a,b,c,X[5],0x4787c62a,12) STEP(F,c,d,a,b,X[6],0xa8304613,17) STEP(F,b,c,d,a,X[7],0xfd469501,22)
        STEP(F,a,b,c,d,X[8],0x698098d8,7)  STEP(F,d,a,b,c,X[9],0x8b44f7af,12) STEP(F,c,d,a,b,X[10],0xffff5bb1,17) STEP(F,b,c,d,a,X[11],0x895cd7be,22)
        STEP(F,a,b,c,d,X[12],0x6b901122,7) STEP(F,d,a,b,c,X[13],0xfd987193,12) STEP(F,c,d,a,b,X[14],0xa679438e,17) STEP(F,b,c,d,a,X[15],0x49b40821,22)
        STEP(G,a,b,c,d,X[1],0xf61e2562,5)  STEP(G,d,a,b,c,X[6],0xc040b340,9)  STEP(G,c,d,a,b,X[11],0x265e5a51,14) STEP(G,b,c,d,a,X[0],0xe9b6c7aa,20)
        STEP(G,a,b,c,d,X[5],0xd62f105d,5)  STEP(G,d,a,b,c,X[10],0x02441453,9) STEP(G,c,d,a,b,X[15],0xd8a1e681,14) STEP(G,b,c,d,a,X[4],0xe7d3fbc8,20)
        STEP(G,a,b,c,d,X[9],0x21e1cde6,5)  STEP(G,d,a,b,c,X[14],0xc33707d6,9) STEP(G,c,d,a,b,X[3],0xf4d50d87,14) STEP(G,b,c,d,a,X[8],0x455a14ed,20)
        STEP(G,a,b,c,d,X[13],0xa9e3e905,5) STEP(G,d,a,b,c,X[2],0xfcefa3f8,9)  STEP(G,c,d,a,b,X[7],0x676f02d9,14) STEP(G,b,c,d,a,X[12],0x8d2a4c8a,20)
        STEP(H,a,b,c,d,X[5],0xfffa3942,4)  STEP(H,d,a,b,c,X[8],0x8771f681,11) STEP(H,c,d,a,b,X[11],0x6d9d6122,16) STEP(H,b,c,d,a,X[14],0xfde5380c,23)
        STEP(H,a,b,c,d,X[1],0xa4beea44,4)  STEP(H,d,a,b,c,X[4],0x4bdecfa9,11) STEP(H,c,d,a,b,X[7],0xf6bb4b60,16) STEP(H,b,c,d,a,X[10],0xbebfbc70,23)
        STEP(H,a,b,c,d,X[13],0x289b7ec6,4) STEP(H,d,a,b,c,X[0],0xeaa127fa,11) STEP(H,c,d,a,b,X[3],0xd4ef3085,16) STEP(H,b,c,d,a,X[6],0x04881d05,23)
        STEP(H,a,b,c,d,X[9],0xd9d4d039,4)  STEP(H,d,a,b,c,X[12],0xe6db99e5,11) STEP(H,c,d,a,b,X[15],0x1fa27cf8,16) STEP(H,b,c,d,a,X[2],0xc4ac5665,23)
        STEP(I,a,b,c,d,X[0],0xf4292244,6)  STEP(I,d,a,b,c,X[7],0x432aff97,10) STEP(I,c,d,a,b,X[14],0xab9423a7,15) STEP(I,b,c,d,a,X[5],0xfc93a039,21)
        STEP(I,a,b,c,d,X[12],0x655b59c3,6) STEP(I,d,a,b,c,X[3],0x8f0ccc92,10) STEP(I,c,d,a,b,X[10],0xffeff47d,15) STEP(I,b,c,d,a,X[1],0x85845dd1,21)
        STEP(I,a,b,c,d,X[8],0x6fa87e4f,6)  STEP(I,d,a,b,c,X[15],0xfe2ce6e0,10) STEP(I,c,d,a,b,X[6],0xa3014314,15) STEP(I,b,c,d,a,X[13],0x4e0811a1,21)
        STEP(I,a,b,c,d,X[4],0xf7537e82,6)  STEP(I,d,a,b,c,X[11],0xbd3af235,10) STEP(I,c,d,a,b,X[2],0x2ad7d2bb,15) STEP(I,b,c,d,a,X[9],0xeb86d391,21)
        a += sa; b += sb; c += sc; d += sd;
        p += 64;
    } while (size -= 64);
    ctx->a = a; ctx->b = b; ctx->c = c; ctx->d = d;
    return p;
}
void md5_init(md5_ctx *c) { c->a=0x67452301; c->b=0xefcdab89; c->c=0x98badcfe; c->d=0x10325476; c->lo=0; c->hi=0; }
void md5_update(md5_ctx *ctx, const void *data, size_t size)
{
    const uint8_t *p = data;
    uint32_t saved_lo = ctx->lo, used, avail;
    if ((ctx->lo = (saved_lo + size) & 0x1fffffff) < saved_lo) ctx->hi++;
    ctx->hi += (uint32_t)(size >> 29);
    used = saved_lo & 0x3f;
    if (used) {
        avail = 64 - used;
        if (size < avail) { memcpy(&ctx->buf[used], p, size); return; }
        memcpy(&ctx->buf[used], p, avail); p += avail; size -= avail;
        body(ctx, ctx->buf, 64);
    }
    if (size >= 64) { p = body(ctx, p, size & ~(size_t)0x3f); size &= 0x3f; }
    memcpy(ctx->buf, p, size);
}
void md5_final(md5_ctx *ctx, uint8_t out[16])
{
    uint32_t used = ctx->lo & 0x3f, avail;
    ctx->buf[used++] = 0x80;
    avail = 64 - used;
    if (avail < 8) { memset(&ctx->buf[used], 0, avail); body(ctx, ctx->buf, 64); used = 0; avail = 64; }
    memset(&ctx->buf[used], 0, avail - 8);
    ctx->lo <<= 3;
    uint32_t v[2] = { ctx->lo, ctx->hi };
    for (int k = 0; k < 2; k++) for (int i = 0; i < 4; i++) ctx->buf[56 + k*4 + i] = (uint8_t)(v[k] >> (8*i));
    body(ctx, ctx->buf, 64);
    uint32_t r[4] = { ctx->a, ctx->b, ctx->c, ctx->d };
    for (int k = 0; k < 4; k++) for (int i = 0; i < 4; i++) out[k*4 + i] = (uint8_t)(r[k] >> (8*i));
}
