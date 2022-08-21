/** SHA-512 hash wrapper
2022, Simon Zolin */

#include <stdlib.h>

typedef struct sha512_ctx {
	char data[8*8 + 8*2 + 8 + 256];
} sha512_ctx;

#define SHA512_LENGTH 64

#ifdef __cplusplus
extern "C" {
#endif

void __sha512_init_ctx (void *ctx);
void __sha512_process_bytes (const void *buffer, size_t len, void *ctx);
void *__sha512_finish_ctx (void *ctx, void *resbuf);

#ifdef __cplusplus
}
#endif

static inline void sha512_init(sha512_ctx *ctx)
{
	__sha512_init_ctx(ctx);
}

static inline void sha512_update(sha512_ctx *ctx, const void *data, size_t size)
{
	__sha512_process_bytes(data, size, ctx);
}

static inline void sha512_fin(sha512_ctx *ctx, unsigned char result[SHA512_LENGTH])
{
	__sha512_finish_ctx(ctx, result);
}

static inline void sha512_hash(const void *data, size_t size, unsigned char result[SHA512_LENGTH])
{
	sha512_ctx c;
	sha512_init(&c);
	sha512_update(&c, data, size);
	sha512_fin(&c, result);
}
