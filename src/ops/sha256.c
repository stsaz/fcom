/** fcom: SHA-256
2022, Simon Zolin */

#include <fcom.h>
#include <../3pt/sha/sha256.h>

struct sha256 {
	sha256_ctx h;
};

static fcom_hash_obj* sha256_create()
{
	struct sha256 *s = ffmem_new(struct sha256);
	sha256_init(&s->h);
	return s;
}

static void _sha256_update(fcom_hash_obj *obj, const void *data, ffsize size)
{
	struct sha256 *s = obj;
	sha256_update(&s->h, data, size);
}

static void sha256_finish(fcom_hash_obj *obj, byte *result, ffsize result_cap)
{
	if (result_cap != 32)
		return;
	struct sha256 *s = obj;
	sha256_fin(&s->h, result);
}

static void sha256_close(fcom_hash_obj *obj)
{
	if (obj == NULL)
		return;
	ffmem_free(obj);
}

FF_EXPORT const fcom_hash sha256 = {
	sha256_create,
	_sha256_update,
	sha256_finish,
	sha256_close,
};
