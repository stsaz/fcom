/** fcom fsync
2022, Simon Zolin
*/

#include <util/crc.h>
#include <util/rbtree.h>
#include <ffbase/murmurhash3.h>

/** Fast CRC32 implementation using 8k table. */
extern uint crc32(const void *buf, size_t size, uint crc);

/** Get name hash. */
static ffrbtkey mv_namehash(struct file *f)
{
	return murmurhash3(f->name, ffsz_len(f->name), 0x12345678);
}

/** Get properties hash. */
static ffrbtkey mv_hash(struct file *f, uint flags)
{
	uint k = crc32(&f->size, sizeof(f->size), 0);
	if (flags & FSYNC_CMP_MTIME) {
		if (flags & FSYNC_CMP_MTIME_SEC)
			k = crc32(&f->mtime.sec, sizeof(f->mtime.sec), k);
		else
			k = crc32(&f->mtime, sizeof(f->mtime), k);
	}
	return k;
}

/** Add file to table. */
static void mv_add(struct mv *mv, struct file *f, uint flags)
{
	ffrbtl_insert_withhash(&mv->names_index, &f->nod_name, mv_namehash(f));
	ffrbtl_insert_withhash(&mv->props_index, &f->nod_props, mv_hash(f, flags));
}

/** Add entry to a "moved files" table in case left and right files have different names. */
static int mv_index1(fsync_ctx *c)
{
	int rcmp;
	struct file *l, *r;
	uint st = 0;

	l = cur_get(&c->curL);
	r = cur_get(&c->curR);
	if (l == NULL && r == NULL)
		return -1;
	else if (l != NULL && r != NULL) {
		fcom_dbglog(0, FILT_NAME, "cmp: %s/%s  and  %s/%s"
			, l->parent->path, l->name, r->parent->path, r->name);
		rcmp = path_cmp(c, l, r);
	} else {
		rcmp = (l == NULL) ? 1 : -1;
		if (l != NULL)
			fcom_dbglog(0, FILT_NAME, "cmp: %s/%s  and  -"
				, l->parent->path, l->name);
		else
			fcom_dbglog(0, FILT_NAME, "cmp: -  and  %s/%s"
				, r->parent->path, r->name);
	}

	if (rcmp < 0) {
		st = FSYNC_ST_SRC;
		mv_add(&c->mvL, l, c->flags);
	} else if (rcmp > 0) {
		st = FSYNC_ST_DEST;
		mv_add(&c->mvR, r, c->flags);
	}

	if (st != FSYNC_ST_DEST) {
		cur_next(&c->curL);
	}
	if (st != FSYNC_ST_SRC) {
		cur_next(&c->curR);
	}

	return 0;
}

/** Build indexes for unique items. */
static int mv_index(fsync_ctx *c)
{
	for (;;) {
		if (0 != mv_index1(c))
			return 0;
	}
	return 0;
}

/** Find an entry with the same properties in table.
. Find candidate entries with the same properties and, possibly, names.
. If names match, this is a moved file.
. If names don't match, this is a renamed or moved file. */
static struct file* mv_find(struct mv *mv, struct file *f, uint flags)
{
	ffrbtl_node *nod_name, *nod;

	if (NULL == (nod = (void*)ffrbt_find(&mv->props_index, mv_hash(f, flags))))
		return NULL;
	nod_name = (void*)ffrbt_find(&mv->names_index, mv_namehash(f));

	if (nod_name != NULL) {
		ffrbtl_node *it;
		FFRBTL_FOR_SIB(nod, it) {

			struct file *nf = FF_GETPTR(struct file, nod_props, it);
			if (nf->moved || FSYNC_ST_EQ != file_cmp(f, nf, flags))
				continue;

			ffrbtl_node *it_name;
			FFRBTL_FOR_SIB(nod_name, it_name) {
				struct file *nf_name = FF_GETPTR(struct file, nod_name, it_name);
				if (nf == nf_name && ffsz_eq(nf->name, nf_name->name)) {
					nf->moved = 1;
					return nf;
				}
			}
		}
	}

	ffrbtl_node *it;
	FFRBTL_FOR_SIB(nod, it) {
		struct file *nf = FF_GETPTR(struct file, nod_props, it);
		if (nf->moved || FSYNC_ST_EQ != file_cmp(f, nf, flags))
			continue;
		nf->moved = 1;
		return nf;
	}

	return NULL;
}
