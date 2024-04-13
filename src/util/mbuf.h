/** Buffer that can be linked with another buffer.
2017, Simon Zolin */

#pragma once
#include <ffbase/list.h>
#include <ffbase/vector.h>

/** Memory block that can be linked with another block.
BLK0 <-> BLK1 <-> ... */
typedef struct ffmbuf {
	ffvec buf;
	ffchain_item sib;
} ffmbuf;

/** Allocate and add new block into the chain. */
static inline ffmbuf* ffmbuf_chain_push(fflist *blocks)
{
	ffmbuf *mblk;
	if (NULL == (mblk = ffmem_new(ffmbuf)))
		return NULL;
	ffvec_null(&mblk->buf);
	fflist_add(blocks, &mblk->sib);
	return mblk;
}

/** Get the last block in chain. */
static inline ffmbuf* ffmbuf_chain_last(fflist *blocks)
{
	if (fflist_empty(blocks))
		return NULL;
	ffchain_item *blk = fflist_last(blocks);
	return FF_STRUCTPTR(ffmbuf, sib, blk);
}

static inline void ffmbuf_free(ffmbuf *m)
{
	ffvec_free(&m->buf);
	ffmem_free(m);
}
