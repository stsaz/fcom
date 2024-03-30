/** fcom: MD5 hash
2022, Simon Zolin */

#include <../3pt/md5/md5.h>

struct md5h {
	md5_ctx h;
};

static fcom_hash_obj* md5h_create()
{
	struct md5h *m = ffmem_new(struct md5h);
	md5_init(&m->h);
	return m;
}

static void md5h_update(fcom_hash_obj *obj, const void *data, ffsize size)
{
	struct md5h *m = obj;
	md5_update(&m->h, data, size);
}

static void md5h_fin(fcom_hash_obj *obj, byte *result, ffsize result_cap)
{
	if (result_cap != 16)
		return;
	struct md5h *m = obj;
	md5_final(&m->h, result);
}

static void md5h_close(fcom_hash_obj *obj)
{
	if (obj == NULL)
		return;
	ffmem_free(obj);
}

FF_EXPORT const fcom_hash fcom_md5 = {
	md5h_create,
	md5h_update,
	md5h_fin,
	md5h_close,
};
