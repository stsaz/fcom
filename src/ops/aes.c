/** fcom: AES
2022, Simon Zolin */

#include <fcom.h>
#include <../3pt/aes/aes-ff.h>

static const fcom_core *core;

struct aes {
	aes_ctx aes;
};

static fcom_aes_obj* aes_e_create(const byte *key, ffsize key_len, uint flags)
{
	struct aes *a = ffmem_new(struct aes);
	if (0 != aes_encrypt_init(&a->aes, key, key_len, flags)) {
		fcom_errlog("aes_encrypt_init");
		return NULL;
	}
	return a;
}

static int aes_e_process(fcom_aes_obj *obj, const void *in, void *out, ffsize len, byte *iv)
{
	struct aes *a = obj;
	int r = aes_encrypt_chunk(&a->aes, in, out, len, iv);
	if (r != 0) {
		fcom_errlog("aes_encrypt_chunk");
		return -1;
	}
	return 0;
}

static void aes_e_close(fcom_aes_obj *obj)
{
	if (obj == NULL)
		return;
	struct aes *a = obj;
	aes_encrypt_destroy(&a->aes);
	ffmem_free(a);
}

FF_EXPORT const fcom_aes fcom_aes_encrypt = {
	aes_e_create,
	aes_e_process,
	aes_e_close,
};


static fcom_aes_obj* aes_d_create(const byte *key, ffsize key_len, uint flags)
{
	struct aes *a = ffmem_new(struct aes);
	if (0 != aes_decrypt_init(&a->aes, key, key_len, flags)) {
		fcom_errlog("aes_decrypt_init");
		return NULL;
	}
	return a;
}

static int aes_d_process(fcom_aes_obj *obj, const void *in, void *out, ffsize len, byte *iv)
{
	struct aes *a = obj;
	int r = aes_decrypt_chunk(&a->aes, in, out, len, iv);
	if (r != 0) {
		fcom_errlog("aes_decrypt_chunk");
		return -1;
	}
	return 0;
}

static void aes_d_close(fcom_aes_obj *obj)
{
	if (obj == NULL)
		return;
	struct aes *a = obj;
	aes_decrypt_destroy(&a->aes);
	ffmem_free(a);
}

FF_EXPORT const fcom_aes fcom_aes_decrypt = {
	aes_d_create,
	aes_d_process,
	aes_d_close,
};


static void crypto_init(const fcom_core *_core) { core = _core; }
static void crypto_destroy(){}
static const fcom_operation* crypto_provide_op(const char *name) { return NULL; }
FF_EXPORT struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	crypto_init, crypto_destroy, crypto_provide_op,
};
