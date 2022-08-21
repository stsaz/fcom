/** fcom: SHA-512
2022, Simon Zolin */

#include <fcom.h>
#include <../3pt/sha/sha512.h>

struct sha512 {
	sha512_ctx h;
};

static fcom_hash_obj* sha512_create()
{
	struct sha512 *s = ffmem_new(struct sha512);
	sha512_init(&s->h);
	return s;
}

static void _sha512_update(fcom_hash_obj *obj, const void *data, ffsize size)
{
	struct sha512 *s = obj;
	sha512_update(&s->h, data, size);
}

static void sha512_finish(fcom_hash_obj *obj, byte *result, ffsize result_cap)
{
	if (result_cap != 64)
		return;
	struct sha512 *s = obj;
	sha512_fin(&s->h, result);
}

static void sha512_close(fcom_hash_obj *obj)
{
	if (obj == NULL)
		return;
	ffmem_free(obj);
}

FF_EXP const fcom_hash sha512 = {
	sha512_create,
	_sha512_update,
	sha512_finish,
	sha512_close,
};
