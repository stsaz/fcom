/** Synchronize files.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>
#include <FF/path.h>
#include <FF/sys/dir.h>
#include <FF/time.h>
#include <FF/rbtree.h>
#include <FF/crc.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, __VA_ARGS__)

static const fcom_core *core;
static const fcom_command *com;

// MODULE
static int fsync_sig(uint signo);
static const void* fsync_iface(const char *name);
static int fsync_conf(const char *name, ffpars_ctx *ctx);
static const fcom_mod fsync_mod = {
	.sig = &fsync_sig, .iface = &fsync_iface, .conf = &fsync_conf,
	.ver = FCOM_VER,
	.name = "fsync", .desc = "Synchronize files",
};

// SYNC
static void* fsync_open(fcom_cmd *cmd);
static void fsync_close(void *p, fcom_cmd *cmd);
static int fsync_process(void *p, fcom_cmd *cmd);
static const fcom_filter fsync_filt = { &fsync_open, &fsync_close, &fsync_process };

struct fsync_ctx;
struct cursor;
struct dir;
struct cmp;
struct mv;
static void cur_init(struct cursor *c, struct dir *d);
static struct dir* scan_tree(struct fsync_ctx *c, const char *fn);
static int cmp_trees(struct fsync_ctx *c, struct cmp *cmp);
static void cmp_show(struct fsync_ctx *c, struct cmp *cmp);
static int mv_index(struct fsync_ctx *c);
static struct file* mv_find(struct mv *mv, struct file *f);
static void mv_add(struct mv *mv, struct file *f);


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &fsync_mod;
}

static int fsync_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT:
		ffmem_init();
		com = core->iface("core.com");
		if (0 != com->reg("sync", "file.sync"))
			return -1;
		break;
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}

static const void* fsync_iface(const char *name)
{
	if (ffsz_eq(name, "sync"))
		return &fsync_filt;
	return NULL;
}

static int fsync_conf(const char *name, ffpars_ctx *ctx)
{
	return 0;
}


#define FILT_NAME  "file.sync"

struct file {
	char *name;
	uint64 size;
	fftime mtime;
	uint attr;
	struct dir *parent;
	struct dir *dir;
	ffchain_item sib;
	ffrbtl_node nod;
};

struct dir {
	char *path;
	ffarr files; //struct file[]
	ffchain_item sib;
};

enum FSYNC_CMP_F {
	// FSYNC_CMP_NAME = 1,
	FSYNC_CMP_SIZE = 2,
	FSYNC_CMP_MTIME = 4,
	FSYNC_CMP_ATTR = 8,
	FSYNC_CMP_MOVE = 0x10,
};

enum FSYNC_ST {
	FSYNC_ST_EQ,
	FSYNC_ST_SRC,
	FSYNC_ST_DEST,
	FSYNC_ST_MOVED,
	FSYNC_ST_NEQ,
	_FSYNC_ST_MASK = 0x0f,
	_FSYNC_ST_NMASK = 0xff0,
	FSYNC_ST_NSIZE = 0x10,
	FSYNC_ST_NSIZE_SMALLER = 0x100,
	FSYNC_ST_NSIZE_LARGER = 0x200,
	FSYNC_ST_NDATE = 0x20,
	FSYNC_ST_NDATE_OLDER = 0x400,
	FSYNC_ST_NDATE_NEWER = 0x800,
	FSYNC_ST_NATTR = 0x40,
};

struct cmp {
	struct file *left;
	struct file *right;
	uint status; //enum FSYNC_ST
};

struct cur_ctx {
	struct dir *d;
	struct file *next;
};

struct cursor {
	struct file *f;
	struct cur_ctx ctx[50];
	uint ictx;
};

struct mv {
	ffrbtree rbt;
};

typedef struct fsync_ctx {
	struct dir *src;
	struct dir *dst;
	ffarr fn;
	struct cursor curL, curR;
	struct mv mvL, mvR;
} fsync_ctx;

static void* fsync_open(fcom_cmd *cmd)
{
	fsync_ctx *f;
	if (NULL == (f = ffmem_new(fsync_ctx)))
		return FCOM_OPEN_SYSERR;
	ffrbt_init(&f->mvL.rbt);
	ffrbt_init(&f->mvR.rbt);
	return f;
}

static void dir_free(struct dir *d)
{
	ffmem_safefree(d->path);
	struct file *f;
	FFARR_WALKT(&d->files, f, struct file) {
		ffmem_safefree(f->name);
	}
	ffarr_free(&d->files);
	ffmem_free(d);
}

static void tree_free(struct dir *d)
{
	ffchain_item *it;
	it = d->sib.next;
	dir_free(d);
	while (it != NULL) {
		d = FF_GETPTR(struct dir, sib, it);
		it = it->next;
		dir_free(d);
	}
}

static void fsync_close(void *p, fcom_cmd *cmd)
{
	fsync_ctx *c = p;

	FF_SAFECLOSE(c->src, NULL, tree_free);
	FF_SAFECLOSE(c->dst, NULL, tree_free);

	ffarr_free(&c->fn);
	ffmem_free(c);
}

static int fsync_process(void *p, fcom_cmd *cmd)
{
	fsync_ctx *f = p;
	char *fn;

	if (NULL == (fn = com->arg_next(cmd, 0)))
		return FCOM_ERR;

	if (cmd->output.fn == NULL) {
		errlog("output file isn't specified", 0);
		return FCOM_ERR;
	}

	if (NULL == (f->src = scan_tree(f, fn)))
		return FCOM_ERR;

	if (NULL == (f->dst = scan_tree(f, cmd->output.fn)))
		return FCOM_ERR;

	cur_init(&f->curL, f->src);
	cur_init(&f->curR, f->dst);
	if (0 != mv_index(f))
		return FCOM_ERR;

	cur_init(&f->curL, f->src);
	cur_init(&f->curR, f->dst);

	struct cmp cmp;
	for (;;) {
		if (0 != cmp_trees(f, &cmp))
			break;
		cmp_show(f, &cmp);
	}

	return FCOM_DONE;
}

/** Get contents of a directory. */
static int scan1(struct dir *d, char *name, ffchain_item **dirs)
{
	ffdirexp dr = {0};
	fffileinfo fi;
	const char *fn;
	struct file *e;
	int r = -1;
	ffchain_item *last = *dirs;

	fcom_dbglog(0, FILT_NAME, "opening directory %s", name);

	if (0 != ffdir_expopen(&dr, name, 0)) {
		if (fferr_last() != ENOMOREFILES) {
			fcom_syserrlog(FILT_NAME, "%s", ffdir_open_S);
			return -1;
		}
		return 0;
	}

	if (NULL == ffarr_allocT(&d->files, dr.size, struct file))
		goto done;

	while (NULL != (fn = ffdir_expread(&dr))) {

		e = ffarr_pushT(&d->files, struct file);
		ffmem_tzero(e);
		if (NULL == (e->name = ffsz_alcopyz(ffdir_expname(&dr, fn))))
			goto done;
		e->parent = d;

		if (0 == fffile_infofn(fn, &fi)) {
			e->attr = fffile_infoattr(&fi);
			e->size = fffile_infosize(&fi);
			e->mtime = fffile_infomtime(&fi);
			if (fffile_isdir(fffile_infoattr(&fi))) {
				e->sib.next = last->next;
				last->next = &e->sib;
				last = &e->sib;
			}
		}
	}

	*dirs = last;
	r = 0;

done:
	ffdir_expclose(&dr);
	return r;
}

/** Get contents of a file tree. */
static struct dir* scan_tree(fsync_ctx *f, const char *name)
{
	ffchain_item *it, *last, tmp;
	struct dir *d, *first = NULL;
	struct file *fil;

	ffchain_init(&tmp);
	last = &tmp;
	if (NULL == (d = ffmem_new(struct dir)))
		return NULL;
	first = d;
	if (NULL == (d->path = ffsz_alcopyz(name)))
		goto end;
	if (0 != scan1(d, d->path, &last))
		goto end;

	FFCHAIN_WALK(&tmp, it) {

		fil = FF_GETPTR(struct file, sib, it);
		if (NULL == (d = ffmem_new(struct dir)))
			goto end;
		fil->dir = d;
		if (NULL == (d->path = ffsz_alfmt("%s/%s"
			, fil->parent->path, fil->name)))
			goto end;

		if (0 != scan1(d, d->path, &last))
			goto end;
	}

	last->next = NULL;
	return first;

end:
	last->next = NULL;
	tree_free(first);
	return NULL;
}


static void cur_init(struct cursor *c, struct dir *d)
{
	c->ctx[0].d = d;
	c->f = (void*)d->files.ptr;
}

#define cur_get(c)  ((c)->f)

#define cur_dir(c)  (c)->ctx[(c)->ictx].d

static void cur_push(struct cursor *c, struct dir *d)
{
	c->ctx[c->ictx].next = c->f + 1;
	c->ictx++;
	FF_ASSERT(c->ictx != FFCNT(c->ctx));
	struct cur_ctx *cx = &c->ctx[c->ictx];
	cx->d = d;
	c->f = (void*)d->files.ptr;
}

static void* cur_next(struct cursor *c)
{
	const struct cur_ctx *cx;
	c->f++;
	for (;;) {
		cx = &c->ctx[c->ictx];
		if (c->f != ffarr_endT(&cx->d->files, struct file))
			break;
		if (c->ictx == 0) {
			c->f = NULL;
			return NULL;
		}
		c->ictx--;
		cx = &c->ctx[c->ictx];
		c->f = cx->next;
	}

	return c->f;
}


/** Compare attributes of 2 files.
Return enum FSYNC_ST. */
static int cmp_file(const struct file *f1, const struct file *f2, uint flags)
{
	if (fffile_isdir(f1->attr) != fffile_isdir(f2->attr))
		return FSYNC_ST_NEQ;

	int r;
	uint m = 0;
	if ((flags & FSYNC_CMP_SIZE)
		&& !(fffile_isdir(f1->attr) | fffile_isdir(f2->attr))
		&& f1->size != f2->size) {
		m |= FSYNC_ST_NSIZE;
		m |= (f1->size < f2->size) ? FSYNC_ST_NSIZE_SMALLER : FSYNC_ST_NSIZE_LARGER;
	}

	if ((flags & FSYNC_CMP_MTIME) && 0 != (r = fftime_cmp(&f1->mtime, &f2->mtime))) {
		m |= FSYNC_ST_NDATE;
		m |= (r < 0) ? FSYNC_ST_NDATE_OLDER : FSYNC_ST_NDATE_NEWER;
	}

	if ((flags & FSYNC_CMP_ATTR) && f1->attr != f2->attr)
		m |= FSYNC_ST_NATTR;

	return (m != 0) ? FSYNC_ST_NEQ | m : FSYNC_ST_EQ;
}

static int path_cmp(fsync_ctx *c, const struct file *f1, const struct file *f2)
{
	int rcmp;
	ffstr n1, n2;
	ffstr_setz(&n1, cur_dir(&c->curL)->path + ffsz_len(c->src->path));
	ffstr_setz(&n2, cur_dir(&c->curR)->path + ffsz_len(c->dst->path));
	if ((n1.len & n2.len) == 0)
		rcmp = (ssize_t)n2.len - n1.len;
	else
		rcmp = ffpath_cmp(&n1, &n2, 0);
	if (rcmp == 0) {
		ffstr_setz(&n1, f1->name);
		ffstr_setz(&n2, f2->name);
		rcmp = ffpath_cmp(&n1, &n2, 0);
	}
	return rcmp;
}

/** Get next change from comparing 2 directory trees.
Enter a subdirectory before processing next files at the same level (e.g. /d1/f1 before /d2). */
static int cmp_trees(fsync_ctx *c, struct cmp *cmp)
{
	int rcmp;
	uint st, st2, flags = FSYNC_CMP_SIZE | FSYNC_CMP_MTIME | FSYNC_CMP_ATTR;
	struct file *l, *r;

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
		st = st2 = FSYNC_ST_SRC;
		if (NULL != (r = mv_find(&c->mvR, l)))
			st2 = FSYNC_ST_MOVED;
	} else if (rcmp > 0) {
		st = st2 = FSYNC_ST_DEST;
		if (NULL != (l = mv_find(&c->mvL, r)))
			st2 = FSYNC_ST_MOVED;
	} else
		st = st2 = cmp_file(l, r, flags);

	cmp->left = l;
	cmp->right = r;
	cmp->status = st2;

	if (st != FSYNC_ST_DEST) {
		if (l->dir != NULL)
			cur_push(&c->curL, l->dir);
		else
			cur_next(&c->curL);
	}
	if (st != FSYNC_ST_SRC) {
		if (r->dir != NULL)
			cur_push(&c->curR, r->dir);
		else
			cur_next(&c->curR);
	}

	return 0;
}

static const char* const cmp_sstatus[] = {
	"Equal", "New", "Deleted", "Moved", "Modified",
};
static const char* const cmp_sstatus_tm[] = {
	"", "Older", "Newer"
};
static const char* const cmp_sstatus_sz[] = {
	"", "Smaller", "Larger"
};

/** Get file comparison status. */
static const char* cmp_status_str(char *buf, size_t cap, struct cmp *cmp)
{
	uint st = cmp->status;

	if ((st & _FSYNC_ST_MASK) == FSYNC_ST_NEQ) {
		uint itm = 0, isz = 0;
		if (st & FSYNC_ST_NDATE)
			itm = (st & FSYNC_ST_NDATE_OLDER) ? 1 : 2;
		if (st & FSYNC_ST_NSIZE)
			isz = (st & FSYNC_ST_NSIZE_SMALLER) ? 1 : 2;
		ffs_fmt(buf, buf + cap, "%s (%s,%s)%Z"
			, cmp_sstatus[st & _FSYNC_ST_MASK], cmp_sstatus_tm[itm], cmp_sstatus_sz[isz]);
		return buf;
	}

	return cmp_sstatus[st & _FSYNC_ST_MASK];
}

/** Print the result of file comparison.
"status name size date name size date" */
static void cmp_show(fsync_ctx *c, struct cmp *cmp)
{
	uint st = cmp->status & _FSYNC_ST_MASK;
	char buf[64], stime[128];
	size_t n;
	ffdtm dt;
	const struct file *f;

	ffstr_catfmt(&c->fn, "%s\t", cmp_status_str(buf, sizeof(buf), cmp));

	if (st != FSYNC_ST_DEST) {
		f = cmp->left;
		fftime_split(&dt, &f->mtime, FFTIME_TZLOCAL);
		n = fftime_tostr(&dt, stime, sizeof(stime), FFTIME_DATE_YMD | FFTIME_HMS);

		ffstr_catfmt(&c->fn, "%s/%s %12U %*s\t"
			, f->parent->path, f->name, f->size, n, stime);
	} else
		ffstr_catfmt(&c->fn, "-\t");

	if (st != FSYNC_ST_SRC) {
		f = cmp->right;
		fftime_split(&dt, &f->mtime, FFTIME_TZLOCAL);
		n = fftime_tostr(&dt, stime, sizeof(stime), FFTIME_DATE_YMD | FFTIME_HMS);

		ffstr_catfmt(&c->fn, "%s/%s %12U %*s\t"
			, f->parent->path, f->name, f->size, n, stime);
	} else
		ffstr_catfmt(&c->fn, "-");

	core->log(FCOM_LOGINFO | FCOM_LOGNOPFX, FILT_NAME ": %S", &c->fn);
	c->fn.len = 0;
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
		mv_add(&c->mvL, l);
	} else if (rcmp > 0) {
		st = FSYNC_ST_DEST;
		mv_add(&c->mvR, r);
	}

	if (st != FSYNC_ST_DEST) {
		if (l->dir != NULL)
			cur_push(&c->curL, l->dir);
		else
			cur_next(&c->curL);
	}
	if (st != FSYNC_ST_SRC) {
		if (r->dir != NULL)
			cur_push(&c->curR, r->dir);
		else
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

/** Find entry with the same name and attributes in table. */
static struct file* mv_find(struct mv *mv, struct file *f)
{
	ffstr name;
	ffrbtl_node *nod;
	uint flags = FSYNC_CMP_SIZE | FSYNC_CMP_MTIME | FSYNC_CMP_ATTR;
	struct file *nf;
	fflist_item *it;

	ffstr_setz(&name, f->name);

	uint key = crc32((void*)name.ptr, name.len, 0);
	if (NULL == (nod = (void*)ffrbt_find(&mv->rbt, key, NULL)))
		return NULL;

	it = &nod->sib;
	nf = FF_GETPTR(struct file, nod, nod);
	if (ffstr_eqz(&name, nf->name)
		&& FSYNC_ST_EQ == cmp_file(f, nf, flags))
		return nf;

	for (;;) {
		it = it->next;
		if (it == &nod->sib)
			break;

		nf = FF_GETPTR(struct file, nod, ffrbtl_nodebylist(it));
		if (ffstr_eqz(&name, nf->name)
			&& FSYNC_ST_EQ == cmp_file(f, nf, flags))
			return nf;
	}
	return NULL;
}

/** Add file to table. */
static void mv_add(struct mv *mv, struct file *f)
{
	f->nod.key = crc32((void*)f->name, ffsz_len(f->name), 0);
	ffrbtl_insert(&mv->rbt, &f->nod);
}

#undef FILT_NAME
