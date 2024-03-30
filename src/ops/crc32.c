/** fcom: CRC32
2022, Simon Zolin */

#include <fcom.h>

/** Fast CRC32 implementation using 8k table */
extern ffuint crc32(const void *buf, ffsize size, ffuint crc);

struct crc32 {
	uint crc;
};

static fcom_hash_obj* crc32_create()
{
	return ffmem_new(struct crc32);
}

static void crc32_close(fcom_hash_obj *obj)
{
	if (obj == NULL) return;
	ffmem_free(obj);
}

static void crc32_update(fcom_hash_obj *obj, const void *data, ffsize size)
{
	struct crc32 *c = obj;
	c->crc = crc32(data, size, c->crc);
}

static void crc32_fin(fcom_hash_obj *obj, byte *result, ffsize result_cap)
{
	struct crc32 *c = obj;
	if (result_cap == 4)
		*(uint*)result = c->crc;
}

FF_EXPORT const fcom_hash fcom_crc32 = {
	crc32_create,
	crc32_update,
	crc32_fin,
	crc32_close,
};
