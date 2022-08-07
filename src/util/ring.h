#pragma once
#include "string.h"
#include "ffos-compat/atomic.h"

/** Circular array of pointers with fixed number of elements.
Readers and writers can run in parallel.
Empty buffer: [(r,w). . . .]
Full buffer: [(r)E1 E2 E3 (w).]
Full buffer: [(w). (r)E1 E2 E3]
Overlapped:  [. (r)E1 (w). .]
Overlapped:  [E2 (w). . (r)E1]
*/
struct ffring {
	void **d;
	size_t cap;
	ffatomic whead, wtail;
	ffatomic r;
};
typedef struct ffring ffring;

/**
@size: power of 2. */
static inline int ffring_create(ffring *r, size_t size, uint align);

static inline void ffring_destroy(ffring *r)
{
	ffmem_alignfree(r->d);
}

static inline int _ffring_write(ffring *r, void *p, uint single);

/** Add element to ring buffer.
Return 0 on success;  <0 if full. */
static inline int ffring_write(ffring *r, void *p)
{
	return _ffring_write(r, p, 0);
}

/** Exclusive-access writer (single producer). */
static inline int ffring_write_excl(ffring *r, void *p)
{
	return _ffring_write(r, p, 1);
}

/** Read element from ring buffer.
Return 0 on success;  <0 if empty. */
static inline int ffring_read(ffring *r, void **p);

static inline int ffring_empty(ffring *r)
{
	return (ffatom_get(&r->r) == ffatom_get(&r->wtail));
}

int ffring_create(ffring *r, size_t size, uint align)
{
	FF_ASSERT(0 == (size & (size - 1)));
	if (NULL == (r->d = ffmem_align(size * sizeof(void*), align)))
		return -1;
	r->cap = size;
	return 0;
}

static inline size_t ffint_increset2(size_t n, size_t cap)
{
	return (n + 1) & (cap - 1);
}

/*
. Try to reserve the space for the data to add.
  If another writer has reserved this space before us, we try again.
. Write new data
. Wait for the previous writers to finish their job
. Finalize: update writer-tail pointer
*/
int _ffring_write(ffring *r, void *p, uint single)
{
	size_t head_old, head_new;

	for (;;) {
		head_old = ffatom_get(&r->whead);
		head_new = ffint_increset2(head_old, r->cap);
		if (head_new == ffatom_get(&r->r))
			return -1;
		if (single)
			break;
		if (ffatom_cmpset(&r->whead, head_old, head_new))
			break;
		// other writer has added another element
	}

	r->d[head_old] = p;

	if (!single) {
		while (ffatom_get(&r->wtail) != head_old) {
			ffcpu_pause();
		}
	}

	ffatom_fence_rel(); // the element is complete when reader sees it
	ffatom_set(&r->wtail, head_new);
	return 0;
}

int ffring_read(ffring *r, void **p)
{
	void *rc;
	size_t rr, rnew;

	for (;;) {
		rr = ffatom_get(&r->r);
		if (rr == ffatom_get(&r->wtail))
			return -1;
		ffatom_fence_acq(); // if we see an unread element, it's complete
		rc = r->d[rr];
		rnew = ffint_increset2(rr, r->cap);
		if (ffatom_cmpset(&r->r, rr, rnew))
			break;
		// other reader has read this element
	}

	*p = rc;
	return 0;
}
