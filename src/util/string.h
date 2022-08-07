#pragma once
#include "ffos-compat/types.h"
#include <FFOS/string.h>
#include <FFOS/error.h>
#include <ffbase/string.h>
#include <ffbase/stringz.h>

#define FFSLEN(s)  FFS_LEN(s)

/** Protect against division by zero. */
#define FFINT_DIVSAFE(val, by) \
	((by) != 0 ? (val) / (by) : 0)

#define ffchar_lower(ch)  ((ch) | 0x20)

static inline ffbool ffchar_isdigit(int ch) {
	return (ch >= '0' && ch <= '9');
}

static inline ffbool ffchar_isup(int ch) {
	return (ch >= 'A' && ch <= 'Z');
}

static inline ffbool ffchar_islow(int ch) {
	return (ch >= 'a' && ch <= 'z');
}

#define ffchar_isletter(ch)  ffchar_islow(ffchar_lower(ch))

static inline ffbool ffbit_testarr(const uint *ar, uint bit)
{
	return ffbit_test32(&ar[bit / 32], bit % 32);
}

static inline ffbool ffchar_isname(int ch)
{
	/** a-zA-Z0-9_ */
	static const uint ffcharmask_name[] = {
		0,
		            // ?>=< ;:98 7654 3210  /.-, +*)( '&%$ #"!
		0x03ff0000, // 0000 0011 1111 1111  0000 0000 0000 0000
		            // _^]\ [ZYX WVUT SRQP  ONML KJIH GFED CBA@
		0x87fffffe, // 1000 0111 1111 1111  1111 1111 1111 1110
		            //  ~}| {zyx wvut srqp  onml kjih gfed cba`
		0x07fffffe, // 0000 0111 1111 1111  1111 1111 1111 1110
		0,
		0,
		0,
		0
	};
	return (0 != ffbit_testarr(ffcharmask_name, (byte)ch));
}

enum {
	FF_TEXT_LINE_MAX = 64 * 1024, //max line size
};

/** Compare two buffers. */
#define ffmemcmp(s1, s2, n)  memcmp(s1, s2, n)

#define ffs_cmp(s1, s2, n)  memcmp(s1, s2, n)

/** Compare buffer and NULL-terminated string.
Return 0 if equal.
Return the mismatch byte position:
 . n+1 if s1 > sz2
 . -n-1 if s1 < sz2. */
static inline ssize_t ffs_cmpn(const char *s1, const char *s2, size_t len)
{
	for (size_t i = 0;  i != len;  i++) {
		int c1 = s1[i], c2 = s2[i];
		if (c1 != c2)
			return (c1 < c2) ? -(ssize_t)i - 1 : (ssize_t)i + 1;
	}

	return 0; //s1 == s2
}

/** Return TRUE if a buffer and a constant NULL-terminated string are equal. */
#define ffs_eqcz(s1, len, csz2) \
	((len) == FFSLEN(csz2) && 0 == ffs_cmp(s1, csz2, len))

#ifndef FF_MSVC
#define ffsz_icmp(sz1, sz2)  strcasecmp(sz1, sz2)
#else
#define ffsz_icmp(sz1, sz2)  _stricmp(sz1, sz2)
#endif

/** Return NULL if not found. */
#define ffsz_findc(sz, ch)  strchr(sz, ch)

/** Search byte in a buffer.
Return END if not found. */
static inline char * ffs_find(const char *buf, size_t len, int ch) {
	char *pos = (char*)memchr(buf, ch, len);
	return (pos != NULL ? pos : (char*)buf + len);
}

/** Search byte in a buffer.
Return NULL if not found. */
#define ffs_findc(buf, len, ch)  memchr(buf, ch, len)

static inline char * ffs_findof(const char *buf, size_t len, const char *anyof, size_t cnt)
{
	ffssize r = ffs_findany(buf, len, anyof, cnt);
	if (r < 0)
		return (char*)buf + len;
	return (char*)buf + r;
}

/** Perform reverse search of byte in a buffer. */
static inline char* ffs_rfind(const char *buf, size_t len, int ch)
{
	ffssize i = ffs_rfindchar(buf, len, ch);
	if (i < 0)
		return (char*)buf + len;
	return (char*)buf + i;
}

static inline char* ffs_rfindof(const char *buf, size_t len, const char *anyof, size_t cnt)
{
	ffssize r = ffs_rfindany(buf, len, anyof, cnt);
	if (r < 0)
		return (char*)buf + len;
	return (char*)buf + r;
}

/** Split string by a character.
If split-character isn't found, the second string will be empty.
@first, @second: optional
@at: pointer within the range [s..s+len] or NULL.
Return @at or NULL. */
static inline const char* ffs_split2(const char *s, size_t len, const char *at, ffstr *first, ffstr *second)
{
	if (at == s + len)
		at = NULL;
	ffssize i = (at != NULL) ? at - s : -1;
	ffs_split(s, len, i, first, second);
	return at;
}

#define ffs_split2by(s, len, by, first, second) \
	ffs_split2(s, len, ffs_find(s, len, by), first, second)

#define ffs_rsplit2by(s, len, by, first, second) \
	ffs_split2(s, len, ffs_rfind(s, len, by), first, second)

enum FFSTR_NEXTVAL {
	FFS_NV_KEEPWHITE = 0x200, // don't trim whitespace
};

/** Get the next value from input string like "val1, val2, ...".
Spaces on the edges are trimmed.
@spl: split-character OR-ed with enum FFSTR_NEXTVAL.
Return the number of processed bytes. */
static inline size_t ffstr_nextval(const char *buf, size_t len, ffstr *dst, int spl)
{
	const char *end = buf + len;
	const char *pos;
	ffstr spc, sspl = {};
	uint f = spl & ~0xff;
	spl &= 0xff;

	ffstr_setcz(&spc, " ");

	if (!(f & FFS_NV_KEEPWHITE))
		buf += ffs_skipany(buf, end - buf, spc.ptr, spc.len);

	if (buf == end) {
		dst->len = 0;
		return len;
	}

	if (sspl.ptr != NULL)
		pos = ffs_findof(buf, end - buf, sspl.ptr, sspl.len);
	else
		pos = ffs_find(buf, end - buf, spl);

	if (pos != end) {
		len = pos - (end - len) + 1;
		if (pos + 1 == end && pos != buf)
			len--; // don't remove the last split char, e.g. "val,"
	}

	if (!(f & FFS_NV_KEEPWHITE))
		pos -= ffs_rskipany(buf, pos - buf, spc.ptr, spc.len);

	ffstr_set(dst, buf, pos - buf);
	return len;
}

static inline size_t ffstr_nextval3(ffstr *src, ffstr *dst, int spl)
{
	size_t n = ffstr_nextval(src->ptr, src->len, dst, spl);
	ffstr_shift(src, n);
	return n;
}


#define ffs_findarrz(ar, n, search, search_len) \
	ffszarr_find(ar, n, search, search_len)

#define _ffmemcpy  memcpy
#ifdef FFMEM_DBG
#include <FFOS/atomic.h>
FF_EXTERN ffatomic ffmemcpy_total;
FF_EXTERN void* ffmemcpy(void *dst, const void *src, size_t len);
#else
#define ffmemcpy  _ffmemcpy
#endif

#define ffmem_copyT(dst, src, T)  ffmemcpy(dst, src, sizeof(T))

#define ffmem_copycz(dst, s)  ((char*)ffmemcpy(dst, s, FFSLEN(s)) + FFSLEN(s))

/** Copy 1 character.
Return the tail. */
static inline char * ffs_copyc(char *dst, const char *bufend, int ch) {
	if (dst != bufend)
		*dst++ = (char)ch;
	return dst;
}

/** Copy zero-terminated string. */
static inline char * ffs_copyz(char *dst, const char *bufend, const char *sz) {
	while (dst != bufend && *sz != '\0') {
		*dst++ = *sz++;
	}
	return dst;
}

/** Copy buffer. */
static inline char * ffs_copy(char *dst, const char *bufend, const char *s, size_t len) {
	len = ffmin(bufend - dst, len);
	ffmemcpy(dst, s, len);
	return dst + len;
}

/** Copy the contents of ffstr* into char* buffer. */
#define ffs_copystr(dst, bufend, pstr)  ffs_copy(dst, bufend, (pstr)->ptr, (pstr)->len)

#define ffs_copycz(dst, bufend, csz)  ffs_copy(dst, bufend, csz, FFSLEN(csz))

/** Copy buffer and append zero byte.
Return the pointer to the trailing zero. */
static inline char * ffsz_copy(char *dst, size_t cap, const char *src, size_t len) {
	char *end = dst + cap;
	if (cap != 0) {
		dst = ffs_copy(dst, end - 1, src, len);
		*dst = '\0';
	}
	return dst;
}

static inline char * ffsz_fcopy(char *dst, const char *src, size_t len) {
	ffmem_copy(dst, src, len);
	dst += len;
	*dst = '\0';
	return dst;
}

#define ffsz_fcopyz(dst, src)  strcpy(dst, src)

#define ffsz_copycz(dst, csz)  ffmemcpy(dst, csz, sizeof(csz))

/** Allocate memory and copy string. */
static inline char* ffsz_alcopy(const char *src, size_t len)
{
	char *s = (char*)ffmem_alloc(len + 1);
	if (s != NULL)
		ffsz_fcopy(s, src, len);
	return s;
}

#define ffsz_alcopyz(src)  ffsz_alcopy(src, ffsz_len(src))
#define ffsz_alcopystr(src)  ffsz_alcopy((src)->ptr, (src)->len)

/** Fill buffer with copies of 1 byte. */
static inline size_t ffs_fill(char *s, const char *end, uint ch, size_t len) {
	len = ffmin(len, end - s);
	memset(s, ch, len);
	return len;
}

/**
Return the number of chars written.
Return 0 on error. */
static inline size_t ffs_fmtv(char *buf, const char *end, const char *fmt, va_list args)
{
	va_list va;
	va_copy(va, args);
	ssize_t r = ffs_formatv(buf, end - buf, fmt, va);
	va_end(va);
	return (r >= 0) ? r : 0;
}

static inline size_t ffs_fmt(char *buf, const char *end, const char *fmt, ...) {
	ssize_t r;
	va_list args;
	va_start(args, fmt);
	r = ffs_formatv(buf, end - buf, fmt, args);
	va_end(args);
	return (r >= 0) ? r : 0;
}
