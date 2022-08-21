/** MD5 hash wrapper
2022, Simon Zolin */

#include <stddef.h>

typedef struct md5_ctx {
	char data[8 + 4*4 + 64];
} md5_ctx;

#ifdef __cplusplus
extern "C" {
#endif

void ngx_md5_init(void *ctx);
void ngx_md5_update(void *ctx, const void *data, size_t size);
void ngx_md5_final(unsigned char result[16], void *ctx);

#ifdef __cplusplus
}
#endif

static inline void md5_init(md5_ctx *ctx)
{
	ngx_md5_init(ctx);
}

static inline void md5_update(md5_ctx *ctx, const void *data, size_t size)
{
	ngx_md5_update(ctx, data, size);
}

static inline void md5_final(md5_ctx *ctx, unsigned char result[16])
{
	ngx_md5_final(result, ctx);
}
