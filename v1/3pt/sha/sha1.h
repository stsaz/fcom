/** SHA-1 hash wrapper
2018, Simon Zolin */

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
typedef unsigned char u_char;

typedef struct sha1_ctx {
	uint64_t bytes;
	uint32_t a, b, c, d, e, f;
	u_char buffer[64];
} sha1_ctx;

#define SHA1_LENGTH 20

#ifdef __cplusplus
extern "C" {
#endif

void sha1_init(sha1_ctx *ctx);

void sha1_fin(sha1_ctx *ctx, u_char result[SHA1_LENGTH]);

void sha1_update(sha1_ctx *ctx, const void *data, size_t size);

#ifdef __cplusplus
}
#endif
