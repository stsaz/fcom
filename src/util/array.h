/** Array, buffer range.
Copyright (c) 2013 Simon Zolin
*/

#pragma once

#include "string.h"
#include "chain.h"
#include <FFOS/file.h>
#include <ffbase/vector.h>

#define FFARR_WALKN(ar, n, it, elsz) \
	for (it = (void*)(ar) \
		;  it != (void*)((char*)(ar) + (n) * elsz) \
		;  it = (void*)((char*)it + elsz))

#define FFARR_WALKNT(ar, n, it, T) \
	FFARR_WALKN(ar, n, it, sizeof(T))

/** FOREACH() for static array, e.g. int ar[4] */
#define FFARRS_FOREACH(ar, it) \
	for (it = (ar);  it != (ar) + FF_COUNT(ar);  it++)


#define FFARR_WALKT  FFSLICE_WALK_T

/** Default char-array.
Can be safely casted to 'ffstr'. */
typedef ffvec ffarr;

/** Set a buffer. */
#define ffarr_set(ar, data, n) \
do { \
	(ar)->ptr = data; \
	(ar)->len = n; \
} while(0)

#define ffarr_set3(ar, d, _len, _cap) \
do { \
	(ar)->ptr = d; \
	(ar)->len = _len; \
	(ar)->cap = _cap; \
} while(0)

#define FFARR_SHIFT(ptr, len, by) \
do { \
	ssize_t _by = (by); \
	(ptr) += (_by); \
	(len) -= (_by); \
} while (0)

/** Shift buffer pointers. */
#define ffarr_shift(ar, by)  FFARR_SHIFT((ar)->ptr, (ar)->len, by)

/** Set null array. */
#define ffarr_null(ar) \
do { \
	(ar)->ptr = NULL; \
	(ar)->len = (ar)->cap = 0; \
} while (0)

#define _ffarr_item(ar, idx, elsz)  ((ar)->ptr + idx * elsz)
#define ffarr_itemT(ar, idx, T)  (&((T*)(ar)->ptr)[idx])

/** The last element in array. */
#define ffarr_back(ar)  ((ar)->ptr[(ar)->len - 1])
#define ffarr_lastT(ar, T)  (&((T*)(ar)->ptr)[(ar)->len - 1])

/** The tail of array. */
#define _ffarr_end(ar, elsz)  _ffarr_item(ar, ar->len, elsz)
#define ffarr_end(ar)  ((ar)->ptr + (ar)->len)
#define ffarr_endT(ar, T)  ((T*)(ar)->ptr + (ar)->len)

/** Get the edge of allocated buffer. */
#define ffarr_edge(ar)  ((ar)->ptr + (ar)->cap)

/** The number of free elements. */
#define ffarr_unused(ar)  ((ar)->cap - (ar)->len)

/** Return TRUE if array is full. */
#define ffarr_isfull(ar)  ((ar)->len == (ar)->cap)

/** Reverse walk. */
#define FFARR_RWALKT(ar, it, T) \
	for (it = ffarr_lastT(ar, T);  it - (T*)(ar)->ptr >= 0;  it--)

#define _ffarr_realloc(ar, newlen, elsz) \
	ffvec_realloc((ffvec*)(ar), newlen, elsz)

/** Reallocate array memory if new size is larger.
Pointing buffer: transform into an allocated buffer, copying data.
Return NULL on error. */
#define ffarr_realloc(ar, newlen) \
	_ffarr_realloc((ffarr*)(ar), newlen, sizeof(*(ar)->ptr))

static inline void * _ffarr_alloc(ffarr *ar, size_t len, size_t elsz) {
	ffarr_null(ar);
	return _ffarr_realloc(ar, len, elsz);
}

/** Allocate memory for an array. */
#define ffarr_alloc(ar, len) \
	_ffarr_alloc((ffarr*)(ar), (len), sizeof(*(ar)->ptr))

#define ffarr_allocT(ar, len, T) \
	_ffarr_alloc(ar, len, sizeof(T))

static inline void* _ffarr_allocz(ffarr *ar, size_t len, size_t elsz)
{
	void *r;
	if (NULL != (r = _ffarr_alloc(ar, len, elsz)))
		ffmem_zero(ar->ptr, len * elsz);
	return r;
}

#define ffarr_alloczT(ar, len, T) \
	_ffarr_allocz(ar, len, sizeof(T))

#define ffarr_reallocT(ar, len, T) \
	_ffarr_realloc(ar, len, sizeof(T))

/** Deallocate array memory. */
#define ffarr_free(ar)  ffvec_free((ffvec*)ar)

#define _ffarr_push(ar, elsz)  ffvec_push((ffvec*)(ar), elsz)

#define ffarr_push(ar, T) \
	(T*)_ffarr_push((ffarr*)ar, sizeof(T))

#define ffarr_pushT(ar, T) \
	(T*)_ffarr_push(ar, sizeof(T))

#define ffarr_pushgrowT(ar, lowat, T) \
	ffvec_pushT(ar, T)

/** Add items into array.  Reallocate memory, if needed.
Return the tail.
Return NULL on error. */
static inline void * _ffarr_append(ffarr *ar, const void *src, size_t num, size_t elsz)
{
	if (num != ffvec_add((ffvec*)ar, src, num, elsz))
		return NULL;
	return ar->ptr + ar->len * elsz;
}

#define ffarr_append(ar, src, num) \
	_ffarr_append((ffarr*)ar, src, num, sizeof(*(ar)->ptr))

/** Add data into array until its size reaches the specified amount.
Don't copy any data if the required size is already available.
Return the number of bytes processed;  -1 on error. */
FF_EXTERN ssize_t ffarr_gather(ffarr *ar, const char *d, size_t len, size_t until);

static inline void * _ffarr_copy(ffarr *ar, const void *src, size_t num, size_t elsz) {
	ar->len = 0;
	return _ffarr_append(ar, src, num, elsz);
}

#define ffarr_copy(ar, src, num) \
	_ffarr_copy((ffarr*)ar, src, num, sizeof(*(ar)->ptr))

/** Allocate and copy data from memory pointed by 'a.ptr'. */
#define ffarr_copyself(a) \
do { \
	if ((a)->cap == 0 && (a)->len != 0) \
		ffarr_realloc(a, (a)->len); \
} while (0)

/** Remove element from array.  Move the last element into the hole. */
static inline void _ffarr_rmswap(ffarr *ar, void *el, size_t elsz) {
	ffslice_rmswap((ffslice*)ar, ((char*)el - (char*)ar->ptr) / elsz, 1, elsz);
}

#define ffarr_rmswap(ar, el) \
	_ffarr_rmswap((ffarr*)ar, (void*)el, sizeof(*(ar)->ptr))

#define ffarr_rmswapT(ar, el, T) \
	_ffarr_rmswap(ar, (void*)el, sizeof(T))

/** Shift elements to the right.
A[0]...  ( A[i]... )  A[i+n]... ->
A[0]...  ( ... )  A[i]...
*/
static inline void _ffarr_shiftr(ffarr *ar, size_t i, size_t n, size_t elsz)
{
	char *dst = ar->ptr + (i + n) * elsz;
	const char *src = ar->ptr + i * elsz;
	const char *end = ar->ptr + ar->len * elsz;
	memmove(dst, src, end - src);
}

/** Remove elements from the middle and shift other elements to the left:
A[0]...  ( A[i]... )  A[i+n]...
*/
static inline void _ffarr_rmshift_i(ffarr *ar, size_t i, size_t n, size_t elsz)
{
	char *dst = ar->ptr + i * elsz;
	const char *src = ar->ptr + (i + n) * elsz;
	const char *end = ar->ptr + ar->len * elsz;
	memmove(dst, src, end - src);
	ar->len -= n;
}

typedef ffarr ffstr3;

#define FFSTR2(s)  (s).ptr, (s).len

static inline void ffstr_acq(ffstr *dst, ffstr *src) {
	*dst = *src;
	ffstr_null(src);
}

static inline void ffstr_acqstr3(ffstr *dst, ffstr3 *src) {
	dst->ptr = src->ptr;
	dst->len = src->len;
	ffarr_null(src);
}

static inline size_t ffstr_cat(ffstr *s, size_t cap, const char *d, size_t len) {
	return ffstr_add(s, cap, d, len);
}

#define ffstr_alcopyz(dst, sz)  ffstr_dup(dst, sz, ffsz_len(sz))
#define ffstr_alcopystr(dst, src)  ffstr_dup(dst, (src)->ptr, (src)->len)

static inline void ffstr3_cat(ffstr3 *s, const char *d, size_t len) {
	ffstr_cat((ffstr*)s, s->cap, d, len);
}

static inline size_t ffstr_catfmtv(ffstr3 *s, const char *fmt, va_list va)
{
	va_list args;
	va_copy(args, va);
	ffsize r = ffstr_growfmtv((ffstr*)s, &s->cap, fmt, args);
	va_end(args);
	return r;
}

#define ffstr_catfmt(s, fmt, ...)  ffvec_addfmt((ffvec*)s, fmt, ##__VA_ARGS__)

static inline size_t ffstr_fmt(ffstr3 *s, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	s->len = 0;
	ffsize r = ffstr_growfmtv((ffstr*)s, &s->cap, fmt, args);
	va_end(args);
	return r;
}

#define ffsz_alfmtv  ffsz_allocfmtv
#define ffsz_alfmt  ffsz_allocfmt

/** Read the whole file into memory buffer.
@limit: maximum allowed file size
*/
static inline int fffile_readall(ffarr *a, const char *fn, uint64 limit)
{
	return fffile_readwhole(fn, (ffvec*)a, limit);
}

/** Create (overwrite) file from buffer.
@flags: FFO_* (default: FFO_CREATE | FFO_TRUNC) */
static inline int fffile_writeall(const char *fn, const char *d, size_t len, uint flags)
{
	return fffile_writewhole(fn, d, len, flags);
}


/** Memory block that can be linked with another block.
BLK0 <-> BLK1 <-> ... */
typedef struct ffmblk {
	ffarr buf;
	ffchain_item sib;
} ffmblk;

/** Allocate and add new block into the chain. */
static inline ffmblk* ffmblk_chain_push(ffchain *blocks)
{
	ffmblk *mblk;
	if (NULL == (mblk = ffmem_allocT(1, ffmblk)))
		return NULL;
	ffarr_null(&mblk->buf);
	ffchain_add(blocks, &mblk->sib);
	return mblk;
}

/** Get the last block in chain. */
static inline ffmblk* ffmblk_chain_last(ffchain *blocks)
{
	if (ffchain_empty(blocks))
		return NULL;
	ffchain_item *blk = ffchain_last(blocks);
	return FF_GETPTR(ffmblk, sib, blk);
}

static inline void ffmblk_free(ffmblk *m)
{
	ffarr_free(&m->buf);
	ffmem_free(m);
}
