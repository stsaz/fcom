/** Synchronize files.
Copyright (c) 2017 Simon Zolin
*/

#include <fsync/fsync.h>
#include <util/path.h>
#include <util/time.h>
#include <FFOS/dirscan.h>

const fcom_core *core;
static const fcom_command *com;

// MODULE
static int fsync_sig(uint signo);
static const void* fsync_iface(const char *name);
static const fcom_mod fsync_mod = {
	.sig = &fsync_sig, .iface = &fsync_iface,
	.ver = FCOM_VER,
	.name = "fsync", .desc = "Synchronize files",
};

// SYNC
static void* fsync_open(fcom_cmd *cmd);
static void fsync_close(void *p, fcom_cmd *cmd);
static int fsync_process(void *p, fcom_cmd *cmd);
static const fcom_filter fsync_filt = { &fsync_open, &fsync_close, &fsync_process };

struct fsync_ctx;

static struct dir* scan_tree(const char *fn, uint flags);
static fsync_dir* combine(fsync_dir *a, fsync_dir *b, uint flags);
static void* cmp_init(fsync_dir *left, fsync_dir *right, uint flags);
static int cmp_trees(struct fsync_ctx *c, struct fsync_cmp *cmp);
static void cmp_show(struct fsync_ctx *c, struct fsync_cmp *cmp);
static void tree_free(struct dir *d);
static void* getprop(uint cmd, ...);
#define isdir(a) !!((a) & FFUNIX_FILE_DIR)

// FSYNC IFACE
const fcom_fsync fsync_if = {
	&scan_tree, &combine, &cmp_init, (void*)&cmp_trees, &tree_free, &getprop
};

#include <fsync/test.h>

FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	test_ffpath_parent();
	test_ffpath_cmp();
	core = _core;
	return &fsync_mod;
}

struct dir {
	char *path;
	ffarr files; //struct file[]
	ffchain_item sib;
};

#include <fsync/cursor.h>
#include <fsync/snapshot-save.h>

struct cmd {
	const char *name;
	const char *mod;
	const void *iface;
};
static const struct cmd cmds[] = {
	{ "sync", "fsync.sync", &fsync_filt },
	{ "sync-snapshot", "fsync.syncss", &fsyncss_filt },
	{ NULL, "fsync.fsync", &fsync_if },
};

static int fsync_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
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
		if (ffsz_eq(name, cmd->mod + FFSLEN("fsync.")))
			return cmd->iface;
	}
	return NULL;
}


#undef FILT_NAME
#define FILT_NAME  "sync"

#include <fsync/file.h>
#include <fsync/move.h>

static void* fsync_open(fcom_cmd *cmd)
{
	fsync_ctx *f;
	if (NULL == (f = ffmem_new(fsync_ctx)))
		return FCOM_OPEN_SYSERR;
	f->flags = FSYNC_CMP_SIZE | FSYNC_CMP_MTIME /*| FSYNC_CMP_ATTR*/ | FSYNC_CMP_MOVE;
	ffrbt_init(&f->mvL.names_index);
	ffrbt_init(&f->mvR.names_index);
	ffrbt_init(&f->mvL.props_index);
	ffrbt_init(&f->mvR.props_index);
	return f;
}

struct dir* dir_new(const ffstr *name)
{
	struct dir *d;
	if (NULL == (d = ffmem_new(struct dir)))
		return NULL;
	d->path = ffsz_alcopystr(name);
	return d;
}

struct file* dir_newfile(struct dir *d)
{
	return ffarr_pushT(&d->files, struct file);
}

const char* dir_path(struct dir *d)
{
	return d->path;
}

/** Find file in a tree.
d: Source directory tree, e.g. d->d1->d2
name: e.g. "d1/d2/f1"
*/
struct file* tree_file_find(struct dir *d, const ffstr *name)
{
	ffstr path = *name, dname;
	struct file *f;

	dname = ffpath_next(&path);
	if (!ffstr_eqz(&dname, d->path) || path.len == 0)
		return NULL;
	dname = ffpath_next(&path);

	f = (void*)d->files.ptr;
	for (; f != ffarr_endT(&d->files, struct file); ) {

		if (ffstr_eqz(&dname, f->name)) {
			d = f->dir;
			if (path.len == 0)
				return f;
			if (d == NULL)
				return NULL;
			f = (void*)d->files.ptr;
			dname = ffpath_next(&path);
			continue;
		}
		f++;
	}

	return NULL;
}

static void dir_free(struct dir *d)
{
	ffmem_safefree(d->path);
	struct file *f;
	FFARR_WALKT(&d->files, f, struct file) {
		file_destroy(f);
	}
	ffarr_free(&d->files);
	ffmem_free(d);
}

static void tree_free(struct dir *d)
{
	if (d == NULL)
		return;

	struct cursor c;
	cur_init2(&c, d);

	struct file *f;
	for (;;) {
		if (NULL == (f = cur_next2(&c))) {
			dir_free(d);
			return;
		}
		if (f->dir != NULL)
			dir_free(f->dir);
	}
}

static void fsync_close(void *p, fcom_cmd *cmd)
{
	fsync_ctx *c = p;

	tree_free(c->src);
	tree_free(c->dst);

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
		rc = ffsz_alfmt("%s%c%s", f->parent->path, FFPATH_SLASH, f->name);
		break;
	}
	case FSYNC_DIRNAME: {
		const struct file *f = va_arg(va, struct file*);
		rc = (f->parent != NULL) ? f->parent->path : "";
		break;
	}
	case FSYNC_DIRPATH: {
		const struct dir *d = va_arg(va, struct dir*);
		rc = d->path;
		break;
	}
	case FSYNC_COUNT: {
		const struct dir *d = va_arg(va, struct dir*);
		rc = (void*)(size_t)d->files.len;
		break;
	}
	case FSYNC_GETFILE: {
		const struct dir *d = va_arg(va, struct dir*);
		uint idx = va_arg(va, uint);
		if (idx >= d->files.len) {
			rc = NULL;
			break;
		}
		rc = (void*)ffarr_itemT(&d->files, idx, struct file);
		break;
	}
	case FSYNC_GETSUBDIR: {
		const struct file *f = va_arg(va, struct file*);
		rc = f->dir;
		break;
	}
	case FSYNC_GETPARENT: {
		const struct file *f = va_arg(va, struct file*);
		rc = f->parent;
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

	uint flags = 0;
	ffstr ext;
	ffpath_split3(fn, ffsz_len(fn), NULL, NULL, &ext);
	if (ffstr_eqz(&ext, "txt"))
		flags = FSYNC_SCAN_SNAPSHOT;
	if (NULL == (f->src = scan_tree(fn, flags)))
		return FCOM_ERR;

	if (NULL == (f->dst = scan_tree(cmd->output.fn, 0)))
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
		if ((cmp.status & _FSYNC_ST_MASK) == FSYNC_ST_MOVED
			&& cmp.status & FSYNC_ST_MOVED_DST)
			continue;
		cmp_show(f, &cmp);
	}

	return FCOM_DONE;
}

/** Get contents of a directory. */
static int scan1(struct dir *d, char *name, ffchain_item **dirs)
{
	ffdirscan dr = {};
	fffileinfo fi;
	const char *fn;
	char *fullname = NULL;
	struct file *e;
	int r = -1;
	ffchain_item *last = *dirs;

	fcom_dbglog(0, FILT_NAME, "opening directory %s", name);

	if (0 != ffdirscan_open(&dr, name, 0)) {
		if (fferr_last() != FFERR_NOMOREFILES) {
			syserrlog("%s: %s", ffdir_open_S, name);
			return -1;
		}
		return 0;
	}

	uint n = 0;
	while (NULL != ffdirscan_next(&dr)) {
		n++;
	}
	ffdirscan_reset(&dr);

	if (NULL == ffarr_allocT(&d->files, n, struct file))
		goto done;

	while (NULL != (fn = ffdirscan_next(&dr))) {

		e = ffarr_pushT(&d->files, struct file);
		ffmem_tzero(e);
		if (NULL == (e->name = ffsz_alcopyz(fn)))
			goto done;
		e->parent = d;

		ffmem_free(fullname);
		fullname = ffsz_allocfmt("%s/%s", name, fn);
		if (0 == fffile_infofn(fullname, &fi)) {
			file_info_set(e, &fi);
			if (isdir(e->attr)) {
				e->sib.next = last->next;
				last->next = &e->sib;
				last = &e->sib;
			}
		}

		fcom_dbglog(0, FILT_NAME, "added %s/%s"
			, d->path, e->name);
	}

	*dirs = last;
	r = 0;

done:
	ffmem_free(fullname);
	ffdirscan_close(&dr);
	return r;
}

/** Get contents of a file tree. */
static struct dir* scan_tree(const char *name, uint flags)
{
	ffchain_item *it, *last, tmp;
	struct dir *d, *first = NULL;
	struct file *fil;

	if (flags & FSYNC_SCAN_SNAPSHOT)
		return snapshot_load(name, flags);

	ffchain_init(&tmp);
	last = &tmp;
	if (NULL == (d = ffmem_new(struct dir)))
		return NULL;
	first = d;
	if (NULL == (d->path = ffsz_alcopyz(name)))
		goto end;
	if (0 != scan1(d, d->path, &last))
		{}

	FFCHAIN_WALK(&tmp, it) {

		fil = FF_GETPTR(struct file, sib, it);
		if (NULL == (d = ffmem_new(struct dir)))
			goto end;
		fil->dir = d;
		if (NULL == (d->path = ffsz_alfmt("%s%c%s"
			, fil->parent->path, FFPATH_SLASH, fil->name)))
			goto end;

		if (0 != scan1(d, d->path, &last))
			{}
	}

	last->next = NULL;
	return first;

end:
	last->next = NULL;
	tree_free(first);
	return NULL;
}

static fsync_dir* combine(fsync_dir *a, fsync_dir *b, uint flags)
{
	struct dir *parent, *tgt;
	struct file *f;
	ffstr p1, p2, dir, name;
	ffstr_setz(&p1, a->path);
	ffstr_setz(&p2, b->path);
	if (0 != ffpath_parent(&p1, &p2, &dir))
		return NULL;

	if (ffstr_eq2(&dir, &p1)) {
		// "/path/a" & "/path/a/a1" -> "/path/a" with files=[...,a1]
		ffpath_split2(p2.ptr, p2.len, NULL, &name);
		parent = a;
		tgt = b;

	} else if (ffstr_eq2(&dir, &p2)) {
		// "/path/a/a1" & "/path/a" -> "/path/a" with files=[...,a1]
		ffpath_split2(p1.ptr, p1.len, NULL, &name);
		parent = b;
		tgt = a;

	} else {
		// "/path/a" & "/path/b" -> "/path" with files=[a,b]
		ffstr dname;
		ffpath_split2(p1.ptr, p1.len, &dname, &name);
		if (!ffstr_eq2(&dname, &dir)) {
			errlog("combine: not supported: '%S' into '%S'", &p1, &dir);
			return NULL;
		}

		if (NULL == (parent = ffmem_new(struct dir)))
			return NULL;
		if (NULL == (parent->path = ffsz_alcopystr(&dir)))
			return NULL;

		if (NULL == (f = ffarr_pushT(&parent->files, struct file)))
			return NULL;
		ffmem_tzero(f);
		if (NULL == (f->name = ffsz_alcopystr(&name)))
			return NULL;
		f->parent = parent;
		f->dir = a;
		f->attr = FFUNIX_FILE_DIR;

		ffpath_split2(p2.ptr, p2.len, NULL, &name);
		tgt = b;
	}

	if (NULL == (f = ffarr_pushT(&parent->files, struct file)))
		return NULL;
	ffmem_tzero(f);
	if (NULL == (f->name = ffsz_alcopystr(&name)))
		return NULL;
	f->parent = parent;
	f->dir = tgt;
	f->attr = FFUNIX_FILE_DIR;
	return parent;
}

int path_cmp(fsync_ctx *c, const struct file *f1, const struct file *f2)
{
	int rcmp;
	ffstr n1, n2;
	ffstr_setz(&n1, cur_dir(&c->curL)->path + ffsz_len(c->src->path));
	ffstr_setz(&n2, cur_dir(&c->curR)->path + ffsz_len(c->dst->path));

	int f = 0;
#ifdef FF_WIN
	f = FFPATH_SUPPORT_BACKSLASH;
#endif

	rcmp = ffpath_cmp(&n1, &n2, f);
	fcom_dbglog(0, FILT_NAME, "ffpath_cmp: '%S'  and  '%S': %d"
		, &n1, &n2, rcmp);
	if (rcmp == 0) {
		ffstr_setz(&n1, f1->name);
		ffstr_setz(&n2, f2->name);
		rcmp = ffpath_cmp(&n1, &n2, f);
		fcom_dbglog(0, FILT_NAME, "ffpath_cmp 2: '%S'  and  '%S': %d"
			, &n1, &n2, rcmp);
	}
	return rcmp;
}

static void* cmp_init(fsync_dir *left, fsync_dir *right, uint flags)
{
	fsync_ctx *c;
	c = ffmem_new(fsync_ctx);
	c->flags = (flags != 0) ? flags : FSYNC_CMP_DEFAULT;
	ffrbt_init(&c->mvL.names_index);
	ffrbt_init(&c->mvR.names_index);
	ffrbt_init(&c->mvL.props_index);
	ffrbt_init(&c->mvR.props_index);
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
		rcmp = path_cmp(c, l, r);
		fcom_dbglog(0, FILT_NAME, "cmp: '%s/%s'  and  '%s/%s': %d"
			, l->parent->path, l->name, r->parent->path, r->name, rcmp);
	} else {
		rcmp = (l == NULL) ? 1 : -1;
		if (l != NULL)
			fcom_dbglog(0, FILT_NAME, "cmp: '%s/%s'  and  -"
				, l->parent->path, l->name);
		else
			fcom_dbglog(0, FILT_NAME, "cmp: -  and  '%s/%s'"
				, r->parent->path, r->name);
	}

	if (rcmp < 0) {
		st = st2 = FSYNC_ST_SRC;
		if (NULL != (r = mv_find(&c->mvR, l, c->flags)))
			st2 = FSYNC_ST_MOVED;
	} else if (rcmp > 0) {
		st = st2 = FSYNC_ST_DEST;
		if (NULL != (l = mv_find(&c->mvL, r, c->flags)))
			st2 = FSYNC_ST_MOVED | FSYNC_ST_MOVED_DST;
	} else
		st = st2 = file_cmp(l, r, c->flags);

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

#undef FILT_NAME
