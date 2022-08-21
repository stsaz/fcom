/** fcom: MD5
2022, Simon Zolin */

#include <fcom.h>
#include <../3pt/md5/md5.h>

struct md5 {
	md5_ctx h;
};

static fcom_hash_obj* md5_create()
{
	struct md5 *m = ffmem_new(struct md5);
	md5_init(&m->h);
	return m;
}

static void _md5_update(fcom_hash_obj *obj, const void *data, ffsize size)
{
	struct md5 *m = obj;
	md5_update(&m->h, data, size);
}

static void md5_fin(fcom_hash_obj *obj, byte *result, ffsize result_cap)
{
	if (result_cap != 16)
		return;
	struct md5 *m = obj;
	md5_final(&m->h, result);
}

static void md5_close(fcom_hash_obj *obj)
{
	if (obj == NULL)
		return;
	ffmem_free(obj);
}

FF_EXP const fcom_hash md5 = {
	md5_create,
	_md5_update,
	md5_fin,
	md5_close,
};
