/** AES wrapper
2019, Simon Zolin */

#include "aes-ff.h"
#include <aes.h>

int aes_decrypt_init(aes_ctx *a, const unsigned char *key, size_t key_len, unsigned int flags)
{
	a->ctx = calloc(1, sizeof(aes_decrypt_ctx));
	a->mode = flags;
	if (flags == AES_CBC)
		return aes_decrypt_key(key, key_len, a->ctx);
	return aes_encrypt_key(key, key_len, a->ctx);
}

int aes_decrypt_chunk(aes_ctx *a, const unsigned char *in, unsigned char *out,
	size_t len, unsigned char *iv)
{
	switch (a->mode) {
	case AES_CBC:
		return aes_cbc_decrypt(in, out, len, iv, a->ctx);
	case AES_CFB:
		return aes_cfb_decrypt(in, out, len, iv, a->ctx);
	case AES_OFB:
		return aes_ofb_decrypt(in, out, len, iv, a->ctx);
	}
	return -1;
}


int aes_encrypt_init(aes_ctx *a, const unsigned char *key, size_t key_len, unsigned int flags)
{
	a->ctx = calloc(1, sizeof(aes_encrypt_ctx));
	a->mode = flags;
	return aes_encrypt_key(key, key_len, a->ctx);
}

int aes_encrypt_chunk(aes_ctx *a, const unsigned char *in, unsigned char *out,
	size_t len, unsigned char *iv)
{
	switch (a->mode) {
	case AES_CBC:
		return aes_cbc_encrypt(in, out, len, iv, a->ctx);
	case AES_CFB:
		return aes_cfb_encrypt(in, out, len, iv, a->ctx);
	case AES_OFB:
		return aes_ofb_encrypt(in, out, len, iv, a->ctx);
	}
	return -1;
}
