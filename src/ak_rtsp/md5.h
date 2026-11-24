#pragma once
#include <stdint.h>
#include <stddef.h>
typedef struct { uint32_t a, b, c, d; uint32_t lo, hi; uint8_t buf[64]; } md5_ctx;
void md5_init(md5_ctx *c);
void md5_update(md5_ctx *c, const void *data, size_t len);
void md5_final(md5_ctx *c, uint8_t out[16]);
