/** fcom: C utility functions
2024, Simon Zolin */

#include <ffsys/path.h>

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


#ifdef FF_WIN
#define ffpath_rfindslash(path, len)  ffs_rfindany(path, len, "/\\", 2)

#else // UNIX:
/** Find the last slash in path. */
#define ffpath_rfindslash(path, len)  ffs_rfindchar(path, len, '/')
#endif

/** Compare strings.
Return
	* 0 if equal
	* position +1 where the strings differ */
static inline ffssize ffs_cmp_n(const char *a, const char *b, ffsize n)
{
	for (ffsize i = 0;  i < n;  i++) {
		if (a[i] != b[i])
			return ((ffbyte)a[i] < (ffbyte)b[i]) ? -(ffssize)(i+1) : (ffssize)(i+1);
	}
	return 0;
}

/** Get max. shared parent directory (without the last slash).
p1,p2: Normalized paths
dir: [output] points to segment inside `p1`
Return !=0 if the paths don't have a shared parent. */
static inline int ffpath_parent(ffstr p1, ffstr p2, ffstr *dir)
{
	size_t n = ffmin(p1.len, p2.len);
	if (n == 0)
		return -1; // empty path
	ssize_t i = ffs_cmp_n(p1.ptr, p2.ptr, n);
	if (i == 0) {
		if (p1.len == p2.len)
			goto done; // "/a" & "/a"

		const char *s = (n == p1.len) ? p2.ptr : p1.ptr;
		if (ffpath_slash(s[n]))
			goto done; // "/a" & "/a/b"

		// "/a" & "/ab" => "/a"
	} else if (i < 0) {
		n = -i - 1; // "/ab" & "/ac" => "/a"
	} else {
		n = i - 1; // "/ac" & "/ab" => "/a"
	}

	if ((i = ffpath_rfindslash(p1.ptr, n)) < 0)
		return -1;
	n = i;

done:
	ffstr_set(dir, p1.ptr, n);
	return 0;
}

enum FILE_CMP_F {
	FILE_CMP_FAST = 1, // Compare just 3 chunks of data (beginning, middle, end)
};

/** Compare files content.
window: buffer size
flags: enum FILE_CMP_F'
Return 0 if equal;
 1 if not equal;
 <0 on error */
static inline int file_cmp(const char *fn1, const char *fn2, ffsize window, ffuint flags)
{
	int rc = -1;
	char *buf = NULL;
	fffd f1 = FFFILE_NULL, f2 = FFFILE_NULL;
	ffssize r1, r2;
	ffuint64 sz, off = 0, offsets[3];
	ffuint i = 0;

	if (FFFILE_NULL == (f1 = fffile_open(fn1, FFFILE_READONLY))
		|| FFFILE_NULL == (f2 = fffile_open(fn2, FFFILE_READONLY)))
		goto end;

	sz = fffile_size(f1);
	if (sz != (ffuint64)fffile_size(f2)) {
		rc = 1;
		goto end;
	}
	offsets[0] = 0;
	offsets[1] = (sz > window) ? sz / 2 : ~0ULL;
	offsets[2] = sz - window;

	if (!(buf = (char*)ffmem_align(2 * window, 4096)))
		goto end;

	for (;;) {
		if (flags & FILE_CMP_FAST) {
			if (i == 3)
				break;
			off = offsets[i++];
			if ((ffint64)off < 0)
				break;
		}

		r1 = fffile_readat(f1, buf, window, off);
		r2 = fffile_readat(f2, buf + window, window, off);
		off += window;
		if (r1 < 0 || r2 < 0)
			goto end;
		if (r1 != r2 || ffmem_cmp(buf, buf + window, r1)) {
			rc = 1;
			goto end;
		}
		if ((ffsize)r1 < window)
			break;
	}

	rc = 0;

end:
	fffile_close(f1);
	fffile_close(f2);
	ffmem_free(buf);
	return rc;
}
