/** File name tree with economical memory management.
2022, Simon Zolin */

/*
fntree_create
fntree_free_all
fntree_add
fntree_attach
fntree_path fntree_name
fntree_entries
fntree_next fntree_next_r
fntree_from_dirscan
*/

/** Schematic representation:
root -> -----------
block   path="/"
        size
        entries
        {
          name_len
          name[]
          children -> ----------
        }             path="/dir1"
        -----------   {
                        name_len
                        name[]
                      }
                      ----------
*/

#pragma once
#include <ffbase/string.h>
#include <FFOS/dirscan.h> // optional

struct fntree_block;

typedef struct fntree_entry {
	struct fntree_block *children;
	ffbyte name_len;
	char name[0];
	// padding[]
} fntree_entry;

typedef struct fntree_block {
	ffuint cap;
	ffuint size;
	ffuint path_len;
	ffuint entries;
	char data[0];
	// path[path_len] '\0'
	// padding[]
	// fntree_entry[0]{}
	// fntree_entry[1]{}
	// ...
} fntree_block;

static ffuint _fntr_ent_size(ffuint name_len)
{
	return ffint_align_ceil2(FF_OFF(fntree_entry, name) + 1 + name_len, sizeof(void*));
}

static fntree_entry* _fntr_ent_first(fntree_block *b)
{
	return (fntree_entry*)(b->data + ffint_align_ceil2(b->path_len + 1, sizeof(void*)));
}

static fntree_entry* _fntr_ent_next(fntree_entry *e)
{
	return (fntree_entry*)((char*)e + ffint_align_ceil2(FF_OFF(fntree_entry, name) + 1 + e->name_len, sizeof(void*)));
}

static fntree_entry* _fntr_ent_end(fntree_block *b)
{
	return (fntree_entry*)((char*)b + b->size);
}

/** Create file tree block */
static inline fntree_block* fntree_create(ffstr path)
{
	fntree_block *b;
	ffuint cap = ffmax(path.len + 1, 512);
	cap = ffint_align_ceil2(cap, 512);
	if (NULL == (b = ffmem_alloc(cap)))
		return NULL;
	b->cap = cap;

	b->path_len = path.len;
	ffmem_copy(b->data, path.ptr, path.len);
	b->data[path.len] = '\0';

	b->entries = 0;
	b->size = ffint_align_ceil2(sizeof(fntree_block) + b->path_len + 1, sizeof(void*));
	return b;
}

/** Get block name set with fntree_create() */
static inline ffstr fntree_path(fntree_block *b)
{
	ffstr s = FFSTR_INITN(b->data, b->path_len);
	return s;
}

/** Reallocate buffer and add new file entry. */
static inline fntree_block* fntree_add(fntree_block *b, ffstr name)
{
	if (name.len > 255)
		return NULL;

	ffuint newsize = b->size + _fntr_ent_size(name.len);
	if (newsize > b->cap) {
		ffuint cap = b->cap * 2;
		if (NULL == (b = ffmem_realloc(b, cap)))
			return NULL;
		b->cap = cap;
	}

	struct fntree_entry *e = _fntr_ent_end(b);
	e->children = NULL;
	e->name_len = name.len;
	ffmem_copy(e->name, name.ptr, name.len);

	b->entries++;
	b->size = newsize;
	return b;
}

/** Attach tree-block to the directory entry.
Return old block pointer. */
static inline fntree_block* fntree_attach(fntree_entry *e, fntree_block *b)
{
	fntree_block *old = e->children;
	e->children = b;
	return old;
}

/** Get N of entries in this block */
static inline ffuint fntree_entries(fntree_block *b)
{
	return b->entries;
}

/** Get entry name without path */
static inline ffstr fntree_name(fntree_entry *e)
{
	ffstr s = FFSTR_INITN(e->name, e->name_len);
	return s;
}

typedef struct fntree_cursor {
	fntree_entry *cur;
	fntree_block *curblock;
	fntree_block *block_stk[64];
	fntree_entry *ent_stk[64];
	ffuint istk;
} fntree_cursor;

/** Get next entry in the same block.
Return NULL if done. */
static inline fntree_entry* fntree_next(fntree_cursor *c, fntree_block *b)
{
	struct fntree_entry *e;
	if (c->cur == NULL)
		e = _fntr_ent_first(b);
	else
		e = _fntr_ent_next(c->cur);

	if (e == _fntr_ent_end(b))
		return NULL;

	c->cur = e;
	return e;
}

static fntree_block* _fntr_cur_push(fntree_cursor *c, fntree_block *b, fntree_entry *e)
{
	if (c->istk == FF_COUNT(c->block_stk))
		return NULL;
	c->block_stk[c->istk] = b;
	c->ent_stk[c->istk] = e;
	c->istk++;
	c->cur = NULL;
	c->curblock = e->children;
	return c->curblock;
}

static fntree_block* _fntr_cur_pop(fntree_cursor *c)
{
	if (c->istk == 0)
		return NULL;
	c->istk--;
	c->cur = c->ent_stk[c->istk];
	c->curblock = c->block_stk[c->istk];
	return c->curblock;
}

static fntree_block* _fntr_cur_i(fntree_cursor *c, uint i)
{
	if (i >= c->istk)
		return NULL;
	return c->block_stk[i];
}

/** Get next entry (recursive): parent directory BEFORE its children.
Return NULL if done. */
static inline fntree_entry* fntree_next_r(fntree_cursor *c, fntree_block **root)
{
	fntree_entry *e = c->cur;

	fntree_block *b = *root;
	if (c->curblock == NULL)
		c->curblock = b;
	else
		b = c->curblock;

	if (e != NULL && e->children != NULL) {
		// 2. Go inside the directory, remembering the current block and entry
		if (NULL == (b = _fntr_cur_push(c, c->curblock, e)))
			return NULL;
	}

	for (;;) {
		e = fntree_next(c, b);
		if (e != NULL) {
			*root = b;
			return e; // 1. Return file or directory entry
		}
		// 3. No more entries in this directory - restore parent directory's context and continue
		if (NULL == (b = _fntr_cur_pop(c)))
			return NULL; // 4. Root directory is complete
	}
}

/** Get next block (recursive): parent block AFTER subblock.
Return NULL if done. */
static inline fntree_block* _fntr_blk_next_r_post(fntree_cursor *c, fntree_block *root)
{
	fntree_entry *e = c->cur;

	fntree_block *b = root;
	if (c->curblock == NULL)
		c->curblock = b;
	else
		b = c->curblock;

	if (c->curblock == (void*)-1) {
		// 3. Restore parent directory's context and continue
		if (NULL == (b = _fntr_cur_pop(c)))
			return NULL; // 4. Root directory is complete
	}

	for (;;) {
		e = fntree_next(c, b);
		if (e != NULL) {
			if (e->children != NULL) {
				// 1. Go inside the directory, remembering the current block and entry
				if (NULL == (b = _fntr_cur_push(c, b, e)))
					return NULL;
			}
			continue;
		}

		b = c->curblock;
		c->curblock = (void*)-1;
		return b; // 2. No more entries in this directory - return directory entry
	}
}

/** Free all tree-blocks. */
static inline void fntree_free_all(fntree_block *b)
{
	if (b == NULL)
		return;

	fntree_cursor c = {};
	for (;;) {
		b = _fntr_blk_next_r_post(&c, b);
		if (b == NULL)
			break;
		ffmem_free(b),  b = NULL;
	}
}

#ifdef _FFOS_DIRSCAN_H

/** Fill entries from ffdirscan object. */
static inline fntree_block* fntree_from_dirscan(ffstr path, ffdirscan *ds)
{
	fntree_block *b = fntree_create(path);
	const char *fn;
	while (NULL != (fn = ffdirscan_next(ds))) {
		ffstr name = FFSTR_INITZ(fn);
		if (NULL == (b = fntree_add(b, name)))
			return NULL;
	}
	return b;
}

#endif
