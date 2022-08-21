/** AES wrapper
2019, Simon Zolin */

#include <stdlib.h>

enum AES_MODE {
	AES_CBC,
	AES_CFB,
	AES_OFB,
};

typedef struct aes_ctx {
	void *ctx;
	unsigned int mode;
} aes_ctx;

#ifdef __cplusplus
extern "C" {
#endif

int aes_decrypt_init(aes_ctx *a, const unsigned char *key, size_t key_len, unsigned int flags);
int aes_decrypt_chunk(aes_ctx *a, const unsigned char *in, unsigned char *out, size_t len, unsigned char *iv);
int aes_encrypt_init(aes_ctx *a, const unsigned char *key, size_t key_len, unsigned int flags);
int aes_encrypt_chunk(aes_ctx *a, const unsigned char *in, unsigned char *out, size_t len, unsigned char *iv);

#ifdef __cplusplus
}
#endif

static inline void aes_decrypt_destroy(aes_ctx *a)
{
	free(a->ctx);  a->ctx = NULL;
}

static inline void aes_encrypt_destroy(aes_ctx *a)
{
	free(a->ctx);  a->ctx = NULL;
}
