/** SHA-256 hash wrapper
2019, Simon Zolin */

#include <stdlib.h>

typedef struct sha256_ctx {
	char data[8*4 + 8 + 4+4 + 128];
} sha256_ctx;

#define SHA256_LENGTH 32

#ifdef __cplusplus
extern "C" {
#endif

void __sha256_init_ctx (struct sha256_ctx *ctx);
void __sha256_process_bytes (const void *buffer, size_t len, struct sha256_ctx *ctx);
void *__sha256_finish_ctx (struct sha256_ctx *ctx, void *resbuf);

static inline void sha256_init(sha256_ctx *ctx)
{
	__sha256_init_ctx((struct sha256_ctx*)ctx);
}

static inline void sha256_fin(sha256_ctx *ctx, unsigned char result[SHA256_LENGTH])
{
	__sha256_finish_ctx((struct sha256_ctx*)ctx, result);
}

static inline void sha256_update(sha256_ctx *ctx, const void *data, size_t size)
{
	__sha256_process_bytes(data, size, (struct sha256_ctx*)ctx);
}

static inline void sha256_hash(const void *data, size_t size, unsigned char result[SHA256_LENGTH])
{
	sha256_ctx c;
	sha256_init(&c);
	sha256_update(&c, data, size);
	sha256_fin(&c, result);
}

#ifdef __cplusplus
}
#endif
