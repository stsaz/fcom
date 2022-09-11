/** fcom: SHA-1
2022, Simon Zolin */

#include <fcom.h>
#include <../3pt/sha/sha1.h>

struct sha1 {
	sha1_ctx h;
};

static fcom_hash_obj* sha1_create()
{
	struct sha1 *s = ffmem_new(struct sha1);
	sha1_init(&s->h);
	return s;
}

static void _sha1_update(fcom_hash_obj *obj, const void *data, ffsize size)
{
	struct sha1 *s = obj;
	sha1_update(&s->h, data, size);
}

static void sha1_finish(fcom_hash_obj *obj, byte *result, ffsize result_cap)
{
	if (result_cap != SHA1_LENGTH)
		return;
	struct sha1 *s = obj;
	sha1_fin(&s->h, result);
}

static void sha1_close(fcom_hash_obj *obj)
{
	if (obj == NULL)
		return;
	ffmem_free(obj);
}

FF_EXP const fcom_hash sha1 = {
	sha1_create,
	_sha1_update,
	sha1_finish,
	sha1_close,
};
