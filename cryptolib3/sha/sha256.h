/** SHA-256 generator
2019, Simon Zolin */

#include <stdlib.h>

#ifdef WIN32
	#define _EXPORT  __declspec(dllexport)
#else
	#define _EXPORT  __attribute__((visibility("default")))
#endif

typedef struct {
	char data[8*4 + 8 + 4+4 + 128];
} sha256_ctx;

#define SHA256_LENGTH 32

#ifdef __cplusplus
extern "C" {
#endif

_EXPORT void sha256_init(sha256_ctx *ctx);

_EXPORT void sha256_fin(sha256_ctx *ctx, unsigned char result[SHA256_LENGTH]);

_EXPORT void sha256_update(sha256_ctx *ctx, const void *data, size_t size);

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

#undef _EXPORT
