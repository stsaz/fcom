/** fcom: C utility functions
2024, Simon Zolin */

static inline char* ffstr_dup_str0(ffstr *dst, ffstr src)
{
	if (src.len == 0) {
		FF_ASSERT(dst->ptr == NULL);
		dst->ptr = NULL;
		dst->len = 0;
		return NULL;
	}

	return ffstr_dup(dst, src.ptr, src.len);
}

static inline char* ffstrz_dup_str0(ffstr *dst, ffstr src)
{
	if (src.len == 0) {
		FF_ASSERT(dst->ptr == NULL);
		dst->ptr = NULL;
		dst->len = 0;
		return NULL;
	}

	if (NULL == ffstr_alloc(dst, src.len + 1))
		return NULL;
	*(char*)ffmem_copy(dst->ptr, src.ptr, src.len) = '\0';
	dst->len = src.len;
	return dst->ptr;
}
