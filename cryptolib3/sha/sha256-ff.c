/** SHA-256 generator
2019, Simon Zolin */

#include "sha256.h"

struct sha256_ctx;
extern void __sha256_init_ctx (struct sha256_ctx *ctx);
extern void __sha256_process_bytes (const void *buffer, size_t len, struct sha256_ctx *ctx);
extern void *__sha256_finish_ctx (struct sha256_ctx *ctx, void *resbuf);


void sha256_init(sha256_ctx *ctx)
{
	__sha256_init_ctx((struct sha256_ctx*)ctx);
}

void sha256_fin(sha256_ctx *ctx, unsigned char result[SHA256_LENGTH])
{
	__sha256_finish_ctx((struct sha256_ctx*)ctx, result);
}

void sha256_update(sha256_ctx *ctx, const void *data, size_t size)
{
	__sha256_process_bytes(data, size, (struct sha256_ctx*)ctx);
}
