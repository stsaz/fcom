/** Synchronize files.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>
#include <FF/path.h>
#include <FF/sys/dir.h>
#include <FF/time.h>
#include <FF/rbtree.h>
#include <FF/crc.h>
#include <FF/data/conf.h>


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

// SYNC-SNAPSHOT
static void* fsyncss_open(fcom_cmd *cmd);
static void fsyncss_close(void *p, fcom_cmd *cmd);
static int fsyncss_process(void *p, fcom_cmd *cmd);
static const fcom_filter fsyncss_filt = { &fsyncss_open, &fsyncss_close, &fsyncss_process };

struct fsync_ctx;
struct cursor;
struct dir;
struct mv;
static void cur_init(struct cursor *c, struct dir *d);
static struct dir* scan_tree(const char *fn);
static void* cmp_init(fsync_dir *left, fsync_dir *right, uint flags);
static int cmp_trees(struct fsync_ctx *c, struct fsync_cmp *cmp);
static void cmp_show(struct fsync_ctx *c, struct fsync_cmp *cmp);
static int mv_index(struct fsync_ctx *c);
static struct file* mv_find(struct mv *mv, struct file *f);
static void mv_add(struct mv *mv, struct file *f);
static void tree_free(struct dir *d);
static void* getprop(uint cmd, ...);
#define isdir(a) !!((a) & FFUNIX_FILE_DIR)

// FSYNC IFACE
static const fcom_fsync fsync_if = {
	&scan_tree, &cmp_init, (void*)&cmp_trees, &tree_free, &getprop
};


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &fsync_mod;
}

struct cmd {
	const char *name;
	const char *mod;
	const void *iface;
};
static const struct cmd cmds[] = {
	{ "sync", "file.sync", &fsync_filt },
	{ "sync-snapshot", "file.syncss", &fsyncss_filt },
	{ NULL, "file.fsync", &fsync_if },
};

static int fsync_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		ffmem_init();
		com = core->iface("core.com");
		const struct cmd *c;
		FFARR_WALKNT(cmds, FFCNT(cmds), c, struct cmd) {
			if (c->name != NULL
				&& 0 != com->reg(c->name, c->mod))
				return -1;
		}
		break;
	}
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}

static const void* fsync_iface(const char *name)
{
	const struct cmd *cmd;
	FFARRS_FOREACH(cmds, cmd) {
		if (ffsz_eq(name, cmd->mod + FFSLEN("file.")))
			return cmd->iface;
	}
	return NULL;
}

static int fsync_conf(const char *name, ffpars_ctx *ctx)
{
	return 0;
}


#define FILT_NAME  "file.sync"

struct file {
	// struct fsync_file
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

struct cur_ctx {
	struct dir *d;
	struct file *next;
};

struct cursor {
	struct file *f, *cur;
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
	uint flags;
} fsync_ctx;

static void* fsync_open(fcom_cmd *cmd)
{
	fsync_ctx *f;
	if (NULL == (f = ffmem_new(fsync_ctx)))
		return FCOM_OPEN_SYSERR;
	f->flags = FSYNC_CMP_SIZE | FSYNC_CMP_MTIME /*| FSYNC_CMP_ATTR*/ | FSYNC_CMP_MOVE;
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
	if (d == NULL)
		return;
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

static void* getprop(uint cmd, ...)
{
	void *rc = NULL;
	va_list va;
	va_start(va, cmd);

	switch ((enum FSYNC_CMD)cmd) {
	case FSYNC_FULLNAME: {
		const struct file *f = va_arg(va, struct file*);
		rc = ffsz_alfmt("%s/%s", f->parent->path, f->name);
		break;
	}
	case FSYNC_DIRNAME: {
		const struct file *f = va_arg(va, struct file*);
		rc = (f->parent != NULL) ? f->parent->path : "";
		break;
	}
	}

	va_end(va);
	return rc;
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

	if (NULL == (f->src = scan_tree(fn)))
		return FCOM_ERR;

	if (NULL == (f->dst = scan_tree(cmd->output.fn)))
		return FCOM_ERR;

	cur_init(&f->curL, f->src);
	cur_init(&f->curR, f->dst);
	if (0 != mv_index(f))
		return FCOM_ERR;

	cur_init(&f->curL, f->src);
	cur_init(&f->curR, f->dst);

	struct fsync_cmp cmp;
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
			syserrlog("%s: %s", ffdir_open_S, name);
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
#ifdef FF_UNIX
			e->attr = fffile_infoattr(&fi);
#else
			e->attr = fffile_isdir(fffile_infoattr(&fi)) ? FFUNIX_FILE_DIR : FFUNIX_FILE_REG;
			e->attr |= 0755;
#endif
			e->size = fffile_infosize(&fi);
			e->mtime = fffile_infomtime(&fi);
			if (isdir(e->attr)) {
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
static struct dir* scan_tree(const char *name)
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


static struct file* cur_nextfile(struct cursor *c);

static void cur_init(struct cursor *c, struct dir *d)
{
	c->ictx = 0;
	c->ctx[0].d = d;
	c->ctx[0].next = NULL;
	c->f = (void*)d->files.ptr;
	c->cur = cur_nextfile(c);
}

#define cur_get(c)  ((c)->cur)

#define cur_dir(c)  (c)->ctx[(c)->ictx].d

/** Get the next file at this level. */
static struct file* cur_nextfile(struct cursor *c)
{
	const struct cur_ctx *cx = &c->ctx[c->ictx];
	if (c->f == ffarr_endT(&cx->d->files, struct file)) {
		c->cur = NULL;
		return NULL;
	}
	c->cur = c->f++;
	return c->cur;
}

/** Get the next directory. */
static struct file* cur_nextdir(struct cursor *c)
{
	struct file *f;
	for (;;) {
		f = cur_nextfile(c);
		if (f == NULL)
			return NULL;
		if (isdir(f->attr))
			break;
	}
	return f;
}

static void cur_push(struct cursor *c, struct dir *d)
{
	c->ictx++;
	FF_ASSERT(c->ictx != FFCNT(c->ctx));
	c->ctx[c->ictx].d = d;
	c->ctx[c->ictx].next = NULL;
	c->f = (void*)d->files.ptr;
}

static void* cur_pop(struct cursor *c)
{
	if (c->ictx == 0)
		return NULL;
	c->ctx[c->ictx].next = NULL;
	c->ictx--;
	return &c->ctx[c->ictx];
}

/** Get next entry (post-increment algorithm).
Phase 1: Return all entries at this level
Phase 2: Recursively return all entries from sub-directories
Return NULL if no more entries. */
static struct file* cur_next(struct cursor *c)
{
	struct file *f;
	struct cur_ctx *cx;

	for (;;) {
		cx = &c->ctx[c->ictx];

		if (cx->next == NULL) {
			f = cur_nextfile(c);
			if (f != NULL)
				return f;

			c->f = (void*)cx->d->files.ptr;
			if (c->f == NULL) {
				// empty dir
				if (NULL == cur_pop(c))
					return NULL;
				continue;
			}
			cx->next = c->f;
			continue;
		}

		c->f = cx->next;
		f = cur_nextdir(c);
		if (f == NULL) {
			if (NULL == cur_pop(c))
				return NULL;
			continue;
		}
		cx->next = c->f;
		cur_push(c, f->dir);
	}
}


/** Compare attributes of 2 files.
Return enum FSYNC_ST. */
static int cmp_file(const struct file *f1, const struct file *f2, uint flags)
{
	if (isdir(f1->attr) != isdir(f2->attr))
		return FSYNC_ST_NEQ;

	int r;
	uint m = 0;
	if ((flags & FSYNC_CMP_SIZE)
		&& !(isdir(f1->attr) | isdir(f2->attr))
		&& f1->size != f2->size) {
		m |= (f1->size < f2->size) ? FSYNC_ST_SMALLER : FSYNC_ST_LARGER;
	}

	if ((flags & FSYNC_CMP_MTIME) && 0 != (r = fftime_cmp(&f1->mtime, &f2->mtime))) {
		m |= (r < 0) ? FSYNC_ST_OLDER : FSYNC_ST_NEWER;
	}

	if ((flags & FSYNC_CMP_ATTR) && f1->attr != f2->attr)
		m |= FSYNC_ST_ATTR;

	return (m != 0) ? FSYNC_ST_NEQ | m : FSYNC_ST_EQ;
}

static int path_cmp(fsync_ctx *c, const struct file *f1, const struct file *f2)
{
	int rcmp;
	ffstr n1, n2;
	ffstr_setz(&n1, cur_dir(&c->curL)->path + ffsz_len(c->src->path));
	ffstr_setz(&n2, cur_dir(&c->curR)->path + ffsz_len(c->dst->path));
	rcmp = ffpath_cmp(&n1, &n2, 0);
	if (rcmp == 0) {
		ffstr_setz(&n1, f1->name);
		ffstr_setz(&n2, f2->name);
		rcmp = ffpath_cmp(&n1, &n2, 0);
	}
	return rcmp;
}

static void* cmp_init(fsync_dir *left, fsync_dir *right, uint flags)
{
	fsync_ctx *c;
	c = ffmem_new(fsync_ctx);
	c->flags = (flags != 0) ? flags : FSYNC_CMP_SIZE | FSYNC_CMP_MTIME | FSYNC_CMP_MOVE;
	ffrbt_init(&c->mvL.rbt);
	ffrbt_init(&c->mvR.rbt);
	c->src = left;
	c->dst = right;

	if (c->flags & FSYNC_CMP_MOVE) {
		cur_init(&c->curL, left);
		cur_init(&c->curR, right);
		if (0 != mv_index(c)) {
			ffmem_free(c);
			return NULL;
		}
	}

	cur_init(&c->curL, left);
	cur_init(&c->curR, right);
	return c;
}

/** Get next change from comparing 2 directory trees.
Return -1 if finished. */
static int cmp_trees(fsync_ctx *c, struct fsync_cmp *cmp)
{
	int rcmp;
	uint st, st2;
	struct file *l, *r;

	if (cmp == NULL) {
		ffmem_free(c);
		return 0;
	}

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
		st = st2 = cmp_file(l, r, c->flags);

	cmp->left = l;
	cmp->right = r;
	cmp->status = st2;

	if (st != FSYNC_ST_DEST) {
		cur_next(&c->curL);
	}
	if (st != FSYNC_ST_SRC) {
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
static const char* cmp_status_str(char *buf, size_t cap, struct fsync_cmp *cmp)
{
	uint st = cmp->status;

	if ((st & _FSYNC_ST_MASK) == FSYNC_ST_NEQ) {
		uint itm = 0, isz = 0;
		if (st & (FSYNC_ST_OLDER | FSYNC_ST_NEWER))
			itm = (st & FSYNC_ST_OLDER) ? 1 : 2;
		if (st & (FSYNC_ST_SMALLER | FSYNC_ST_LARGER))
			isz = (st & FSYNC_ST_SMALLER) ? 1 : 2;
		ffs_fmt(buf, buf + cap, "%s (%s,%s)%Z"
			, cmp_sstatus[st & _FSYNC_ST_MASK], cmp_sstatus_tm[itm], cmp_sstatus_sz[isz]);
		return buf;
	}

	return cmp_sstatus[st & _FSYNC_ST_MASK];
}

/** Print the result of file comparison.
"status name size date name size date" */
static void cmp_show(fsync_ctx *c, struct fsync_cmp *cmp)
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

/** Find entry with the same name and attributes in table. */
static struct file* mv_find(struct mv *mv, struct file *f)
{
	ffstr name;
	ffrbtl_node *nod;
	uint flags = FSYNC_CMP_SIZE | FSYNC_CMP_MTIME;
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


#define FILT_NAME  "file.syncss"

struct fsyncss {
	uint state;
	struct dir *tree;
	struct cursor cur;
	ffconfw cw;
	void *curdir;
};

static void* fsyncss_open(fcom_cmd *cmd)
{
	struct fsyncss *f;
	if (NULL == (f = ffmem_new(struct fsyncss)))
		return FCOM_OPEN_SYSERR;
	ffconf_winit(&f->cw, NULL, 0);
	ffarr_alloc(&f->cw.buf, 4096);
	return f;
}

static void fsyncss_close(void *p, fcom_cmd *cmd)
{
	struct fsyncss *f = p;
	FF_SAFECLOSE(f->tree, NULL, tree_free);
	ffconf_wdestroy(&f->cw);
	ffmem_free(f);
}

/** Write dir info into snapshot. */
static void fsyncss_writedir(struct fsyncss *f, struct dir *d, ffbool close)
{
	if (close)
		ffconf_write(&f->cw, NULL, FFCONF_CLOSE, FFCONF_TOBJ);
	f->curdir = d;
	ffconf_write(&f->cw, "d", FFCONF_STRZ, FFCONF_TKEY);
	ffconf_write(&f->cw, d->path, FFCONF_STRZ, FFCONF_TVAL);
	ffconf_write(&f->cw, NULL, FFCONF_OPEN, FFCONF_TOBJ);
	ffconf_write(&f->cw, "v", FFCONF_STRZ, FFCONF_TKEY);
	ffconf_write(&f->cw, "0", FFCONF_STRZ, FFCONF_TVAL);
}

/** Write file info into snapshot. */
static void fsyncss_write(struct fsyncss *f, const struct file *fl)
{
	ffconf_write(&f->cw, "f", FFCONF_STRZ, FFCONF_TKEY);
	ffconf_write(&f->cw, fl->name, FFCONF_STRZ, FFCONF_TVAL);
	ffconf_writeint(&f->cw, fl->size, FFINT_HEXLOW, FFCONF_TVAL);
	ffconf_writeint(&f->cw, fl->attr, FFINT_HEXLOW, FFCONF_TVAL);
	ffconf_writeint(&f->cw, 0, FFINT_HEXLOW, FFCONF_TVAL);
	ffconf_writeint(&f->cw, 0, FFINT_HEXLOW, FFCONF_TVAL);
	ffconf_writeint(&f->cw, fl->mtime.sec, FFINT_HEXLOW, FFCONF_TVAL);
	ffconf_write(&f->cw, "0", FFCONF_STRZ, FFCONF_TVAL);
}

/* ver.0 format:
# fcom file tree snapshot
d "/d" {
v 0 // version
// name size attr uid gid mtime crc
f "1" 0 0 0 0 0 0
}
d "/d/1" {
v 0
f "2" 0 0 0 0 0 0
}
*/
static int fsyncss_process(void *p, fcom_cmd *cmd)
{
	struct fsyncss *f = p;

	for (;;) {
	switch (f->state) {
	case 0: {
		char *fn;
		if (NULL == (fn = com->arg_next(cmd, 0)))
			return FCOM_ERR;

		if (cmd->output.fn == NULL) {
			errlog("output file isn't specified", 0);
			return FCOM_ERR;
		}

		com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));

		if (NULL == (f->tree = scan_tree(fn)))
			return FCOM_ERR;

		cur_init(&f->cur, f->tree);

		ffconf_write(&f->cw, "fcom file tree snapshot", FFCONF_STRZ, FFCONF_TCOMMENTSHARP);
		f->curdir = NULL;
	}
	// fall through

	case 1:
		for (;;) {
			const struct file *fl = cur_next(&f->cur);
			if (fl == NULL)
				break;

			if (fl->parent != f->curdir) {
				// got a file from another directory
				fsyncss_writedir(f, fl->parent, (f->curdir != NULL));
			}

			fsyncss_write(f, fl);

			ffstr s;
			ffconf_output(&f->cw, &s);
			if (cmd->out.len >= 64 * 1024) {
				cmd->out = s;
				f->state = 2;
				return FCOM_DATA;
			}
		}
		break;

	case 2:
		ffconf_clear(&f->cw);
		f->state = 1;
		continue;
	}

	break;
	}

	ffconf_write(&f->cw, NULL, FFCONF_CLOSE, FFCONF_TOBJ);
	if (0 == ffconf_write(&f->cw, NULL, 0, FFCONF_FIN))
		errlog("ffconf_write", 0);
	ffconf_output(&f->cw, &cmd->out);
	return FCOM_DONE;
}

#undef FILT_NAME
