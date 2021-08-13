/** fcom: directory contents for recursive file tree scan
2021, Simon Zolin
*/

#include <ffbase/string.h>
#include <ffbase/stringz.h>

struct dir;
struct dir {
	struct dir *parent;
	ffsize len, cap, cursor;
	char path[0];
	// char file1[];
	// ...
};

/**
parent: finalized parent directory object (Optional) */
static inline struct dir* dir_create(ffstr path, struct dir *parent)
{
	ffsize len = sizeof(struct dir) + path.len+1;
	ffsize cap = ffint_align_power2(len);
	struct dir *d = ffmem_alloc(cap);
	d->cap = cap;
	d->len = len;
	d->cursor = len;
	d->parent = parent;
	ffmem_copy(d->path, path.ptr, path.len);
	d->path[path.len] = '\0';
	return d;
}

static inline void dir_free(struct dir *d)
{
	ffmem_free(d);
}

static inline struct dir* dir_parent(struct dir *d)
{
	return d->parent;
}

static inline const char* dir_path(struct dir *d)
{
	return d->path;
}

/**
Return new object pointer */
static inline struct dir* dir_add(struct dir *d, ffstr name)
{
	ffsize need = d->len + name.len+1;
	if (need > d->cap) {
		ffsize cap = ffint_align_power2(need);
		d = ffmem_realloc(d, cap);
		d->cap = cap;
	}
	ffsz_copyn(&((char*)d)[d->len], -1, name.ptr, name.len);
	d->len = need;
	return d;
}

/** Get next file name
peek: 1=don't shift cursor
Return NULL if no more entries */
static inline const char* dir_next(struct dir *d, ffuint peek)
{
	if (d->cursor == d->len)
		return NULL;
	const char *name = &((char*)d)[d->cursor];
	if (!peek)
		d->cursor += ffsz_len(name)+1;
	return name;
}

static inline void dir_seek_begin(struct dir *d)
{
	d->cursor = sizeof(struct dir) + ffsz_len(d->path)+1;
}
