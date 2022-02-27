/** SHA-1 generator.
2018, Simon Zolin */

#include <inttypes.h>
#include <stdint.h>
#include <stdlib.h>
typedef unsigned char u_char;

#ifdef WIN32
	#define _EXPORT  __declspec(dllexport)
#else
	#define _EXPORT  __attribute__((visibility("default")))
#endif

typedef struct sha1_ctx {
	uint64_t bytes;
	uint32_t a, b, c, d, e, f;
	u_char buffer[64];
} sha1_ctx;
#define SHA1_LENGTH 20

#ifdef __cplusplus
extern "C" {
#endif

_EXPORT void sha1_init(sha1_ctx *ctx);

_EXPORT void sha1_fin(sha1_ctx *ctx, u_char result[SHA1_LENGTH]);

_EXPORT void sha1_update(sha1_ctx *ctx, const void *data, size_t size);

#ifdef __cplusplus
}
#endif

#undef _EXPORT
