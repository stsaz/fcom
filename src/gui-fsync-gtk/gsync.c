/** fcom: GUI-GTK: file sync
2021, Simon Zolin
*/

#include <fcom.h>
#include <util/gui-gtk/gtk.h>
#include <util/gui-gtk/loader.h>
#include <util/path.h>
#include <util/misc.h>
#include <util/conf2-scheme.h>
#include <FFOS/dirscan.h>

extern const fcom_core *core;
const fcom_fsync *_fsync;
const fcom_fops *fops;
extern const fcom_command *com;

#define dbglog(fmt, ...)  fcom_dbglog(0, "gsync", fmt, __VA_ARGS__)
#define verblog(fmt, ...)  fcom_verblog("gsync", fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog("gsync", fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_errlog("gsync", fmt, __VA_ARGS__)

void opts_save();
void opts_def();
void opts_load();

/** Set or clear bits. */
#define ffint_bitmask(pn, mask, set) \
do { \
	if (set) \
		*(pn) |= (mask); \
	else \
		*(pn) &= ~(mask); \
} while (0)


enum SHOWMASK_E {
	// 1<<FSYNC_ST_EQ
	// 1<<FSYNC_ST_SRC
	// 1<<FSYNC_ST_DEST
	// 1<<FSYNC_ST_MOVED
	// 1<<FSYNC_ST_NEQ
	SHOWMASK_NEWER = 1<<28,
	SHOWMASK_OLDER = 1<<29,
	SHOWMASK_DIRS = 1<<30,
};

struct wsync {
	ffui_wnd wnd;
	ffui_menu mm;
	ffui_edit eleft;
	ffui_edit eright;
	ffui_checkbox cbeq, cbnew, cbmod, cbdel, cbmov;
	ffui_checkbox cbshowdirs, cbshowolder, cbshownewer;
	ffui_edit eexclude, einclude;
	ffui_view vlist;
	ffui_ctl stbar;

	ffui_sel *listsel;

	ffuint showmask; // enum SHOWMASK_E
	fsync_dir *src, *dst;
	char *srcfn, *dstfn;
	ffstr show_excl, show_incl;
	char *opts_fn;
	ffvec col_width;
	ffvec cmptbl, cmptbl_filter; // comparison results.  struct fsync_cmp[]
};

struct ggui {
	ffui_menu mcmd, mfile;
	struct wsync *wsync;
	fftask tsk;
};

struct ggui *gg;

const ffui_ldr_ctl wsync_ctls[] = {
	FFUI_LDR_CTL(struct wsync, wnd),
	FFUI_LDR_CTL(struct wsync, mm),
	FFUI_LDR_CTL(struct wsync, eleft),
	FFUI_LDR_CTL(struct wsync, eright),
	FFUI_LDR_CTL(struct wsync, cbeq),
	FFUI_LDR_CTL(struct wsync, cbnew),
	FFUI_LDR_CTL(struct wsync, cbmod),
	FFUI_LDR_CTL(struct wsync, cbmov),
	FFUI_LDR_CTL(struct wsync, cbdel),
	FFUI_LDR_CTL(struct wsync, cbshowdirs),
	FFUI_LDR_CTL(struct wsync, cbshowolder),
	FFUI_LDR_CTL(struct wsync, cbshownewer),
	FFUI_LDR_CTL(struct wsync, eexclude),
	FFUI_LDR_CTL(struct wsync, einclude),
	FFUI_LDR_CTL(struct wsync, vlist),
	FFUI_LDR_CTL(struct wsync, stbar),
	FFUI_LDR_CTL_END
};

const ffui_ldr_ctl top_ctls[] = {
	FFUI_LDR_CTL(struct ggui, mcmd),
	FFUI_LDR_CTL(struct ggui, mfile),
	FFUI_LDR_CTL3_PTR(struct ggui, wsync, wsync_ctls),
	FFUI_LDR_CTL_END
};

enum {
#include "actions.h"
	A_ONCLOSE,
};
const char* const scmds[] = {
#define ACTION_NAMES
#include "actions.h"
#undef ACTION_NAMES
};

static void* wsync_getctl(void *udata, const ffstr *name)
{
	void *ctl = ffui_ldr_findctl(top_ctls, gg, name);
	return ctl;
}

static int wsync_getcmd(void *udata, const ffstr *name)
{
	int r;
	if (0 > (r = ffs_findarrz(scmds, FFCNT(scmds), name->ptr, name->len)))
		return 0;
	return r;
}

static void task_add(fftask_handler func, void *udata)
{
	gg->tsk.handler = func;
	gg->tsk.param = udata;
	core->task(FCOM_TASK_ADD, &gg->tsk);
}

#define fsfile(/* struct file* */f)  ((struct fsync_file*)(f))

#define isdir(a) !!((a) & FFUNIX_FILE_DIR)

enum L_COLS {
	L_ACTN,
	L_SRCNAME,
	L_SRCINFO,
	L_SRCDATE,
	L_STATUS,
	L_DSTNAME,
	L_DSTINFO,
	L_DSTDATE,
	_L_LAST,
};

static const char* const cmp_status[] = {
	"Equal", "New", "Deleted", "Moved", "Modified",
};
static const char* const cmp_status_time[] = {
	"", "Older", "Newer"
};
static const char* const cmp_status_size[] = {
	"", "Smaller", "Larger"
};

static const char* const actionstr[] = {
	"", "Copy", "Delete", "Move", "Overwrite",
};

/** Extensions to enum FSYNC_ST */
enum {
	FSYNC_ST_ERROR = 1 << 28,
	FSYNC_ST_PENDING = 1 << 29, /** Operation on a file is pending */
	FSYNC_ST_CHECKED = 1 << 30,
	FSYNC_ST_DONE = 1 << 31,
};

/** Apply exclude-include name filters */
static ffbool excl_incl(const struct fsync_cmp *c)
{
	struct wsync *w = gg->wsync;
	ffstr n1 = {}, n2 = {};
	if (c->left != NULL) {
		char *fullname = _fsync->get(FSYNC_FULLNAME, c->left);
		ffstr_setz(&n1, fullname);
	}
	if (c->right != NULL) {
		char *fullname = _fsync->get(FSYNC_FULLNAME, c->right);
		ffstr_setz(&n2, fullname);
	}

	ffbool skip = 0;
	ffstr strlist = w->show_excl;
	if (w->show_incl.len != 0)
		strlist = w->show_incl;

	while (strlist.len != 0) {
		ffstr part;
		ffstr_splitby(&strlist, ' ', &part, &strlist);
		ffstr_trimwhite(&part);
		if (part.len == 0)
			continue;

		if (ffstr_ifindstr(&n1, &part) >= 0
			|| ffstr_ifindstr(&n2, &part) >= 0) {
			// matched
			if (w->show_incl.len != 0) {
				skip = 0;
				goto fin;
			}
			skip = 1; // excluded
			break;
		} else {
			// not matched
			if (w->show_incl.len != 0)
				skip = 1; // not included
		}
	}

fin:
	ffmem_free(n1.ptr);
	ffmem_free(n2.ptr);
	if (skip)
		return 0;
	return 1;
}

static ffbool cmpent_visible(const struct fsync_cmp *c)
{
	struct wsync *w = gg->wsync;
	ffuint st = c->status & _FSYNC_ST_MASK;
	if (!ffbit_test32(&w->showmask, st))
		return 0;

	if (st == FSYNC_ST_NEQ) {
		if (!(w->showmask & SHOWMASK_OLDER) && (c->status & FSYNC_ST_OLDER))
			return 0;
		if (!(w->showmask & SHOWMASK_NEWER) && (c->status & FSYNC_ST_NEWER))
			return 0;
	}

	if (!(w->showmask & SHOWMASK_DIRS)
		&& ((c->left != NULL && isdir(fsfile(c->left)->attr))
			|| (c->right != NULL && isdir(fsfile(c->right)->attr))))
		return 0;

	if (w->show_excl.len != 0
		|| w->show_incl.len != 0) {
		if (0 == excl_incl(c))
			return 0;
	}

	return 1;
}

void ctls_set()
{
	struct wsync *w = gg->wsync;
	ffui_edit_settextz(&w->eleft, w->srcfn);
	ffui_edit_settextz(&w->eright, w->dstfn);
	ffui_checkbox_check(&w->cbeq, !!(w->showmask & (1<<FSYNC_ST_EQ)));
	ffui_checkbox_check(&w->cbnew, !!(w->showmask & (1<<FSYNC_ST_SRC)));
	ffui_checkbox_check(&w->cbdel, !!(w->showmask & (1<<FSYNC_ST_DEST)));
	ffui_checkbox_check(&w->cbmov, !!(w->showmask & (1<<FSYNC_ST_MOVED)));
	ffui_checkbox_check(&w->cbmod, !!(w->showmask & (1<<FSYNC_ST_NEQ)));
	ffui_checkbox_check(&w->cbshowdirs, !!(w->showmask & SHOWMASK_DIRS));
	ffui_checkbox_check(&w->cbshowolder, !!(w->showmask & SHOWMASK_OLDER));
	ffui_checkbox_check(&w->cbshownewer, !!(w->showmask & SHOWMASK_NEWER));
}

/** Get file info as text */
static int finfo(const struct fsync_file *e, ffarr *buf)
{
	if (isdir(e->attr)) {
		if (0 == ffstr_catfmt(buf, "[DIR]"))
			return 1;

	} else {
		if (0 == ffstr_catfmt(buf, "%,U", e->size))
			return 1;
	}
	return 0;
}

void modtime(ffarr *buf, struct fsync_file *e)
{
	ffvec_grow(buf, 64, 1);
	ffdatetime dt = {};
	fftime t = e->mtime;
	t.sec += FFTIME_1970_SECONDS;
	fftime_split1(&dt, &t);
	buf->len += fftime_tostr1(&dt, ffarr_end(buf), ffarr_unused(buf), FFTIME_DATE_MDY | FFTIME_HMS);
}

/** Get file comparison status */
static const char* cmp_status_str(char *buf, size_t cap, const struct fsync_cmp *cmp)
{
	uint st = cmp->status;

	if ((st & _FSYNC_ST_MASK) == FSYNC_ST_NEQ) {
		uint itm = 0, isz = 0;

		if (st & (FSYNC_ST_OLDER | FSYNC_ST_NEWER))
			itm = (st & FSYNC_ST_OLDER) ? 1 : 2;

		if (st & (FSYNC_ST_SMALLER | FSYNC_ST_LARGER))
			isz = (st & FSYNC_ST_SMALLER) ? 1 : 2;

		ffs_fmt(buf, buf + cap, "%s (%s,%s)%Z"
			, cmp_status[st & _FSYNC_ST_MASK], cmp_status_time[itm], cmp_status_size[isz]);
		return buf;
	}

	return cmp_status[st & _FSYNC_ST_MASK];
}

void disp(ffui_view *v)
{
	struct wsync *w = gg->wsync;
	struct ffui_view_disp *disp = &v->disp;
	ffarr buf = {};
	char *fullname = NULL;
	ffstr d = {};

	const struct fsync_cmp *c = *ffslice_itemT(&w->cmptbl_filter, disp->idx, struct fsync_cmp*);
	uint st = c->status & _FSYNC_ST_MASK;

	if (!cmpent_visible(c))
		return;

	struct fsync_file *e1, *e2;
	e1 = fsfile(c->left);
	e2 = fsfile(c->right);

	switch (disp->sub) {

	case L_ACTN:
		if (c->status & FSYNC_ST_DONE)
			ffstr_setz(&d, "Done");
		else if (c->status & FSYNC_ST_ERROR)
			ffstr_setz(&d, "Error");
		else if (c->status & FSYNC_ST_PENDING)
			ffstr_setz(&d, "...");
		else
			ffstr_setz(&d, actionstr[st]);
		break;

	case L_SRCNAME:
		if (st != FSYNC_ST_DEST) {
			fullname = _fsync->get(FSYNC_FULLNAME, e1);
			ffstr_setz(&d, fullname);
		}
		break;

	case L_SRCINFO:
		if (st != FSYNC_ST_DEST) {
			if (0 != finfo(e1, &buf))
				goto end;
			ffstr_set2(&d, &buf);
		}
		break;

	case L_SRCDATE:
		if (st != FSYNC_ST_DEST) {
			modtime(&buf, e1);
			ffstr_set2(&d, &buf);
		}
		break;

	case L_STATUS:
		if (NULL == ffarr_alloc(&buf, 64))
			goto end;
		ffstr_setz(&d, cmp_status_str(buf.ptr, buf.cap, c));
		break;

	case L_DSTNAME:
		if (st != FSYNC_ST_SRC) {
			fullname = _fsync->get(FSYNC_FULLNAME, e2);
			ffstr_setz(&d, fullname);
		}
		break;

	case L_DSTINFO:
		if (st != FSYNC_ST_SRC) {
			if (0 != finfo(e2, &buf))
				goto end;
			ffstr_set2(&d, &buf);
		}
		break;

	case L_DSTDATE:
		if (st != FSYNC_ST_SRC) {
			modtime(&buf, e2);
			ffstr_set2(&d, &buf);
		}
	}

	if (d.len != 0)
		disp->text.len = ffmem_ncopy(disp->text.ptr, disp->text.len, d.ptr, d.len);

end:
	ffarr_free(&buf);
	ffmem_free(fullname);
}

void status(const char *fmt, ...)
{
	struct wsync *w = gg->wsync;
	va_list va;
	va_start(va, fmt);
	char *s = ffsz_allocfmtv(fmt, va);
	va_end(va);
	ffui_send_stbar_settextz(&w->stbar, s);
	ffmem_free(s);
}

/** Scan directories, possibly with wildcards */
static fsync_dir* scan_path(const char *path, uint flags)
{
	ffstr p;
	ffstr_setz(&p, path);
	ffdirscan de = {};
	const char *fn;
	char *fullname = NULL, *dirz = NULL;
	fsync_dir *r = NULL, *d = NULL, *parent = NULL;

	if (NULL == ffs_findc(p.ptr, p.len, '*'))
		return _fsync->scan_tree(path, 0);

	ffstr dir, name;
	ffpath_splitpath(path, ffsz_len(path), &dir, &name);
	dirz = ffsz_dupstr(&dir);
	de.wildcard = name.ptr;
	if (0 != ffdirscan_open(&de, dirz, FFDIRSCAN_USEWILDCARD))
		goto done;

	while (NULL != (fn = ffdirscan_next(&de))) {
		ffmem_free(fullname);
		fullname = ffsz_allocfmt("%s/%s", dirz, fn);
		if (NULL == (d = _fsync->scan_tree(fullname, flags)))
			goto done;
		if (parent != NULL) {
			fsync_dir *dnew;
			if (NULL == (dnew = _fsync->combine(parent, d, 0)))
				goto done;
			d = dnew;
		}
		parent = d;
		d = NULL;
	}

	r = parent;

done:
	ffmem_free(dirz);
	ffmem_free(fullname);
	ffdirscan_close(&de);
	if (r == NULL) {
		_fsync->tree_free(d);
		_fsync->tree_free(parent);
	}
	return r;
}

struct cmpstat {
	uint eq;
	uint new;
	uint mov;
	uint mod;
	uint del;
	uint dir;
	uint older, newer;
};

void cmpstat_add(struct cmpstat *c, struct fsync_cmp *cmp)
{
	int st = cmp->status & _FSYNC_ST_MASK;
	if (st == FSYNC_ST_EQ)
		c->eq++;
	else if (st == FSYNC_ST_SRC)
		c->new++;
	else if (st == FSYNC_ST_DEST)
		c->del++;
	else if (st == FSYNC_ST_MOVED)
		c->mov++;
	else if (st == FSYNC_ST_NEQ)
		c->mod++;

	if ((cmp->left != NULL && isdir(fsfile(cmp->left)->attr))
		|| (cmp->right != NULL && isdir(fsfile(cmp->right)->attr)))
		c->dir++;

	if (cmp->status & FSYNC_ST_OLDER)
		c->older++;
	else if (cmp->status & FSYNC_ST_NEWER)
		c->newer++;
}

void ctls_set_cmpstat(struct cmpstat *c)
{
	struct wsync *w = gg->wsync;
	char buf[64];
	ffs_format(buf, sizeof(buf), "Eq (%u)%Z", c->eq);
	ffui_send_checkbox_settextz(&w->cbeq, buf);

	ffs_format(buf, sizeof(buf), "New (%u)%Z", c->new);
	ffui_send_checkbox_settextz(&w->cbnew, buf);

	ffs_format(buf, sizeof(buf), "Mod (%u)%Z", c->mod);
	ffui_send_checkbox_settextz(&w->cbmod, buf);

	ffs_format(buf, sizeof(buf), "Mov (%u)%Z", c->mov);
	ffui_send_checkbox_settextz(&w->cbmov, buf);

	ffs_format(buf, sizeof(buf), "Del (%u)%Z", c->del);
	ffui_send_checkbox_settextz(&w->cbdel, buf);

	ffs_format(buf, sizeof(buf), "Dirs (%u)%Z", c->dir);
	ffui_send_checkbox_settextz(&w->cbshowdirs, buf);

	ffs_format(buf, sizeof(buf), "Older (%u)%Z", c->older);
	ffui_send_checkbox_settextz(&w->cbshowolder, buf);

	ffs_format(buf, sizeof(buf), "Newer (%u)%Z", c->newer);
	ffui_send_checkbox_settextz(&w->cbshownewer, buf);
}

void reset()
{
	struct wsync *w = gg->wsync;

	_fsync->tree_free(w->src);  w->src = NULL;
	_fsync->tree_free(w->dst);  w->dst = NULL;
	ffvec_free(&w->cmptbl);

	w->cmptbl_filter.len = 0;
}

void filter()
{
	struct wsync *w = gg->wsync;
	w->cmptbl_filter.len = 0;

	ffstr ss = {};
	ffui_send_edit_textstr(&w->eexclude, &ss);
	ffstr_free(&w->show_excl);
	w->show_excl = ss;
	ffstr_null(&ss);

	ffui_send_edit_textstr(&w->einclude, &ss);
	ffstr_free(&w->show_incl);
	w->show_incl = ss;
	ffstr_null(&ss);

	struct fsync_cmp *c;
	FFSLICE_WALK(&w->cmptbl, c) {

		c->status &= ~FSYNC_ST_CHECKED;

		if (!cmpent_visible(c))
			continue;

		void **p;
		if (NULL == (p = ffvec_pushT(&w->cmptbl_filter, void*)))
			return;
		*p = c;
	}
}

void showresults()
{
	struct wsync *w = gg->wsync;
	ffui_post_view_clear(&w->vlist);
	ffui_post_view_setdata(&w->vlist, 0, w->cmptbl_filter.len);

	status("Results: %L (%L)", w->cmptbl_filter.len, w->cmptbl.len);
}

/** Scan source and target trees */
int scan()
{
	struct wsync *w = gg->wsync;
	reset();

	if (ffsz_len(w->srcfn) == 0
		|| ffsz_len(w->dstfn) == 0)
		return 1;

	status("Scanning Source...");
	if (NULL == (w->src = scan_path(w->srcfn, 0)))
		return 1;

	status("Scanning Target...");
	if (NULL == (w->dst = scan_path(w->dstfn, 0)))
		return 1;
	return 0;
}

void compare()
{
	struct wsync *w = gg->wsync;
	status("Comparing...");
	struct fsync_cmp cmp, *c;
	uint f = FSYNC_CMP_DEFAULT;
	// ffint_bitmask(&f, FSYNC_CMP_MTIME, w->time_diff);
	ffint_bitmask(&f, FSYNC_CMP_MTIME_SEC, 1);
	void *cmpctx = _fsync->cmp_init(w->src, w->dst, f);

	struct cmpstat ns = {};

	for (;;) {
		if (0 != _fsync->cmp_trees(cmpctx, &cmp))
			break;

		if ((cmp.status & _FSYNC_ST_MASK) == FSYNC_ST_MOVED
			&& (cmp.status & FSYNC_ST_MOVED_DST))
			continue;

		cmpstat_add(&ns, &cmp);

		if (NULL == (c = ffvec_pushT(&w->cmptbl, struct fsync_cmp)))
			goto end;
		*c = cmp;
	}

	ctls_set_cmpstat(&ns);

end:
	if (cmpctx != NULL)
		_fsync->cmp_trees(cmpctx, NULL);
}

/**
"src/path/file" -> "dst/path/file"

For wildcards:
1. "\diff-src\d*": "\diff-src\d1/new" -> "d1/new"
2. "\diff-tgt\d*": "\diff-tgt" + "d1/new" -> "\diff-tgt\d1/new" */
static char* dst_fn(const char *fnL)
{
	struct wsync *w = gg->wsync;
	ffstr fn, src, dst;
	ffstr_setz(&fn, fnL);
	ffstr_setz(&src, w->srcfn);
	ffstr_setz(&dst, w->dstfn);

	ffstr dir_src;
	if (0 != ffpath_parent(&src, &fn, &dir_src)) {
		FF_ASSERT(0);
		return NULL;
	}

	const char *star = ffs_findc(dst.ptr, dst.len, '*');

	if (NULL == ffs_findc(src.ptr, src.len, '*')) {
		if (star != NULL) {
			errlog("not supported: destination dir is a wildcard", 0);
			return NULL;
		}
		ffstr_shift(&fn, dir_src.len + FFSLEN("/"));
		return ffsz_alfmt("%S/%S", &dst, &fn);
	}

	if (0 != ffs_wildcard(src.ptr, src.len, fn.ptr, fn.len, 0)) {
		FF_ASSERT(0);
		return NULL;
	}
	ffstr_shift(&fn, dir_src.len + FFSLEN("/"));

	if (star == NULL) {
		errlog("not supported: destination dir isn't a wildcard", 0);
		return NULL;
	}
	dst.len = star - dst.ptr;
	const char *sl = ffpath_rfindslash(dst.ptr, dst.len);
	dst.len = sl - dst.ptr;

	return ffsz_alfmt("%S/%S", &dst, &fn);
}

struct sync_s {
	ffui_sel *sel;
	int index;
	char *fnL, *fnR;
};

void sync2(struct sync_s *sc);

static void cmdmon_onsig(fcom_cmd *cmd, uint sig, void *param)
{
	struct wsync *w = gg->wsync;
	struct sync_s *sc = param;
	// int r = (!cmd->err) ? 0 : -1;
	ffmem_free(sc->fnL);  sc->fnL = NULL;
	ffmem_free(sc->fnR);  sc->fnR = NULL;
	struct fsync_cmp *c = *ffslice_itemT(&w->cmptbl_filter, sc->index, struct fsync_cmp*);
	c->status |= FSYNC_ST_DONE;
	ffui_post_view_setdata(&w->vlist, sc->index, 0);
	sync2(sc);
}

int copyfile(void *param, const char *src, const char *dst, int flags)
{
	fcom_cmd cmd = {};
	cmd.name = "fcopy";
	cmd.flags = FCOM_CMD_EMPTY;
	cmd.input.fn = src;
	cmd.output.fn = dst;
	if (flags & FOP_KEEPDATE)
		cmd.out_preserve_date = 1;
	if (flags & FOP_OVWR)
		cmd.out_overwrite = 1;

	void *c;
	if (NULL == (c = com->create(&cmd)))
		return FCOM_ERR;
	com->ctrl(c, FCOM_CMD_FILTADD_LAST, "core.file-in");
	com->ctrl(c, FCOM_CMD_FILTADD_LAST, "core.file-out");
	com->fcom_cmd_monitor_func(c, cmdmon_onsig, param);
	com->ctrl(c, FCOM_CMD_RUNASYNC);
	return FCOM_ASYNC;
}

void sc_free(struct sync_s *sc)
{
	ffui_view_sel_free(sc->sel);
	ffmem_free(sc->fnL);
	ffmem_free(sc->fnR);
	ffmem_free(sc);
}

void sync2(struct sync_s *sc)
{
	struct wsync *w = gg->wsync;
	char *fnL = NULL, *fnR = NULL;
	struct fsync_file *f;
	int r;
	ffvec toremove = {};

	for (;;) {
		int i = ffui_view_selnext(&w->vlist, sc->sel);
		if (i < 0)
			break;

		struct fsync_cmp *c = *ffslice_itemT(&w->cmptbl_filter, i, struct fsync_cmp*);
		if (c->status & (FSYNC_ST_PENDING | FSYNC_ST_DONE))
			continue;

		c->status |= FSYNC_ST_PENDING;
		ffui_post_view_setdata(&w->vlist, i, 0);
		sc->index = i;

		switch (c->status & _FSYNC_ST_MASK) {
		case FSYNC_ST_EQ:
			break;

		case FSYNC_ST_SRC:
			fnL = _fsync->get(FSYNC_FULLNAME, c->left);
			fnR = dst_fn(fnL);
			f = fsfile(c->left);
			if (isdir(f->attr)) {
				fops->mkdir(fnR, 0);
				verblog("mkdir: %s", fnR);
			} else {
				r = copyfile(sc, fnL, fnR, FOP_KEEPDATE);
				verblog("copy: %s", fnR);
			}
			break;

		case FSYNC_ST_DEST:
			fnR = _fsync->get(FSYNC_FULLNAME, c->right);
			if (toremove.len == 20) {
				r = fops->del_many(toremove.ptr, toremove.len, FOP_TRASH);
				char **it;
				FFSLICE_WALK(&toremove, it) {
					ffmem_free(*it);
				}
				toremove.len = 0;
			}
			*ffvec_pushT(&toremove, char*) = fnR;
			fnR = NULL;
			verblog("delete: %s", fnR);
			break;

		case FSYNC_ST_MOVED: {
			fnL = _fsync->get(FSYNC_FULLNAME, c->left);
			fnR = _fsync->get(FSYNC_FULLNAME, c->right);
			char *dst = dst_fn(fnL);
			fops->move(fnR, dst, FOP_RECURS);
			verblog("move: %s", dst);
			ffmem_free(dst);
			break;
		}

		case FSYNC_ST_NEQ:
			fnL = _fsync->get(FSYNC_FULLNAME, c->left);
			fnR = _fsync->get(FSYNC_FULLNAME, c->right);
			if (c->status & (FSYNC_ST_SMALLER | FSYNC_ST_LARGER)
				|| fffile_cmp(fnL, fnR, 0)) {
				r = copyfile(sc, fnL, fnR, FOP_OVWR | FOP_KEEPDATE);
				verblog("copy: %s", fnR);

			} else {
				f = fsfile(c->left);

				if (c->status & (FSYNC_ST_OLDER | FSYNC_ST_NEWER)) {
					fops->time(fnR, &f->mtime, 0);
					verblog("set time: %s", fnR);
				}

				if (c->status & FSYNC_ST_ATTR) {
					if (0 != fffile_set_attr_path(fnR, f->attr))
						syserrlog("fffile_set_attr_path: %s", fnR);
					verblog("set attr: %s", fnR);
				}
			}
			break;
		}

		if (r == FCOM_ASYNC) {
			sc->fnL = fnL;
			sc->fnR = fnR;
			goto end2;
		}

		ffmem_free(fnL);  fnL = NULL;
		ffmem_free(fnR);  fnR = NULL;

		c->status |= FSYNC_ST_DONE;
		ffui_post_view_setdata(&w->vlist, i, 0);
	}

	ffmem_free(fnL);
	ffmem_free(fnR);
	sc_free(sc);

end2:
	if (toremove.len != 0) {
		fops->del_many(toremove.ptr, toremove.len, FOP_TRASH);
	}
	char **it;
	FFSLICE_WALK(&toremove, it) {
		ffmem_free(*it);
	}
	ffvec_free(&toremove);
}

void sync()
{
	struct wsync *w = gg->wsync;
	struct sync_s *sc = ffmem_new(struct sync_s);
	sc->sel = ffui_view_getsel(&w->vlist);
	task_add((fftask_handler)sync2, sc);
}

void scan_task(void *param)
{
	if (0 != scan())
		return;
	compare();
	filter();
	showresults();
}

/** Perform file operation */
static void fop(void *param)
{
	struct wsync *w = gg->wsync;
	int id = (ffsize)param;
	char *fullname = NULL;
	ffvec vec = {}, buf = {};

	for (;;) {
		int i = ffui_view_selnext(&w->vlist, w->listsel);
		if (i < 0)
			break;

		const struct fsync_cmp *c = *ffslice_itemT(&w->cmptbl_filter, i, struct fsync_cmp*);
		void *obj = c->left;
		switch (id) {
		case A_CLIPFN_RIGHT:
		case A_DEL_RIGHT:
			obj = c->right;
			break;
		}
		if (obj == NULL)
			continue;
		ffmem_free(fullname);
		fullname = _fsync->get(FSYNC_FULLNAME, obj);

		switch (id) {
		case A_CLIPFN_LEFT:
		case A_CLIPFN_RIGHT:
			ffvec_addfmt(&buf, "%s\n", fullname);
			break;

		default: {
			char **s = ffvec_pushT(&vec, char*);
			*s = ffsz_dup(fullname);
		}
		}
	}

	if (vec.len != 0) {
		fops->del_many(vec.ptr, vec.len, FOP_TRASH);
	} else if (buf.len != 0) {
		ffui_clipboard_settextstr(&buf);
	}

	ffui_view_sel_free(w->listsel);  w->listsel = NULL;
	char **s;
	FFSLICE_WALK(&vec, s) {
		ffmem_free(*s);
	}
	ffvec_free(&vec);
	ffvec_free(&buf);
	ffmem_free(fullname);
}

/** Swap 2 objects. */
#define SWAP2(o1, o2) \
do { \
	__typeof__(o1) _tmp = o1; \
	o1 = o2; \
	o2 = _tmp; \
} while (0)

void swap_src_tgt()
{
	struct wsync *w = gg->wsync;
	SWAP2(w->src, w->dst);
	SWAP2(w->srcfn, w->dstfn);
	ffui_edit_settextz(&w->eleft, w->srcfn);
	ffui_edit_settextz(&w->eright, w->dstfn);

	struct fsync_cmp *it;
	FFSLICE_WALK(&w->cmptbl, it) {
		if (it->status == FSYNC_ST_SRC)
			it->status = FSYNC_ST_DEST;
		else if (it->status == FSYNC_ST_DEST)
			it->status = FSYNC_ST_SRC;
		SWAP2(it->left, it->right);
	}
	FFSLICE_WALK(&w->cmptbl_filter, it) {
		if (it->status == FSYNC_ST_SRC)
			it->status = FSYNC_ST_DEST;
		else if (it->status == FSYNC_ST_DEST)
			it->status = FSYNC_ST_SRC;
		SWAP2(it->left, it->right);
	}

	showresults();
}

void wsync_action(ffui_wnd *wnd, int id)
{
	struct wsync *w = gg->wsync;
	ffui_checkbox *pcb = NULL;
	int i, m;

	if ((uint)id < FF_COUNT(scmds) && id != A_DISP)
		fcom_dbglog(0, "gsync", "action: %s", scmds[id]);

	switch (id) {

	case A_DISP:
		disp(&w->vlist);
		break;

	case A_ONCLOSE:
		opts_save();
		ffui_post_quitloop();
		break;

	case A_CMP: {
		ffstr s = {};
		ffui_edit_textstr(&w->eleft, &s);
		w->srcfn = s.ptr;
		ffstr_null(&s);

		ffui_edit_textstr(&w->eright, &s);
		w->dstfn = s.ptr;

		ffui_view_clear(&w->vlist);

		task_add(scan_task, NULL);
		break;
	}

	case A_SYNC:
		sync();
		break;

	case A_EXEC:
		break;

	case A_SWAP:
		swap_src_tgt();
		break;

	case A_CLIPFN_LEFT:
	case A_CLIPFN_RIGHT:
	case A_DEL_LEFT:
	case A_DEL_RIGHT: {
		w->listsel = ffui_view_getsel(&w->vlist);
		task_add(fop, (void*)(ffsize)id);
		break;
	}

	case A_SHOWEQ:
		pcb = &w->cbeq;
		m = 1<<FSYNC_ST_EQ;
		goto show;
	case A_SHOWNEW:
		pcb = &w->cbnew;
		m = 1<<FSYNC_ST_SRC;
		goto show;
	case A_SHOWMOD:
		pcb = &w->cbmod;
		m = 1<<FSYNC_ST_NEQ;
		goto show;
	case A_SHOWDEL:
		pcb = &w->cbdel;
		m = 1<<FSYNC_ST_DEST;
		goto show;
	case A_SHOWMOVE:
		pcb = &w->cbmov;
		m = 1<<FSYNC_ST_MOVED;
		goto show;
	case A_SHOW_DIRS:
		pcb = &w->cbshowdirs;
		m = SHOWMASK_DIRS;
		goto show;
	case A_SHOW_OLDER:
		pcb = &w->cbshowolder;
		m = SHOWMASK_OLDER;
		goto show;
	case A_SHOW_NEWER:
		pcb = &w->cbshownewer;
		m = SHOWMASK_NEWER;
		goto show;
	show:
		i = ffui_checkbox_checked(pcb);
		ffint_bitmask(&w->showmask, m, i);
		filter();
		showresults();
		break;
	}
}

int load_ui()
{
	int rc = -1;
	ffui_loader ldr;
	ffui_ldr_init2(&ldr, wsync_getctl, wsync_getcmd, gg);

	char *path = NULL;
	if (NULL == (path = core->getpath(FFSTR("gsync.gui"))))
		goto end;

	if (0 != ffui_ldr_loadfile(&ldr, path)) {
		errlog("GUI loader: %s", ffui_ldr_errstr(&ldr));
		goto end;
	}
	rc = 0;

end:
	ffui_ldr_fin(&ldr);
	ffmem_free(path);
	return rc;
}

void wsync_init()
{
	gg = ffmem_new(struct ggui);
	struct wsync *w = ffmem_new(struct wsync);
	gg->wsync = w;
	w->opts_fn = core->env_expand(NULL, 0, "$HOME/.config/fcom/gsync.conf");
	w->wnd.on_action = wsync_action;
	w->wnd.onclose_id = A_ONCLOSE;
	w->vlist.dispinfo_id = A_DISP;

	if (NULL == (_fsync = core->iface("fsync.fsync")))
		return;
	if (NULL == (fops = core->iface("file.ops")))
		return;

	if (0 != load_ui())
		return;

	opts_def();
	opts_load();

	ffui_show(&w->wnd, 1);
	ctls_set();
}

/** Write widths of list's columns to config */
void cols_width_write(ffconfw *conf)
{
	struct wsync *w = gg->wsync;
	for (uint i = 0;  i != _L_LAST;  i++) {
		int n = ffui_view_col_width(&w->vlist, i);
		ffconfw_addint(conf, n);
	}
}

int col_width(ffconf_scheme *cs, void *obj, ffint64 val)
{
	struct wsync *w = gg->wsync;
	*ffvec_pushT(&w->col_width, int) = (int)val;
	return 0;
}
static const ffconf_arg opts_args[] = {
	{ "source_path",	FFCONF_TSTRZ, FF_OFF(struct wsync, srcfn) },
	{ "target_path",	FFCONF_TSTRZ, FF_OFF(struct wsync, dstfn) },
	{ "file_display_mask",	FFCONF_TINT32, FF_OFF(struct wsync, showmask) },
	{ "columns_width",	FFCONF_TINT32 | FFCONF_FLIST, (ffsize)col_width },
};
void cols_width_read()
{
	struct wsync *w = gg->wsync;
	int i = 0;
	int *it;
	FFSLICE_WALK(&w->col_width, it) {
		ffui_view_setcol_width(&w->vlist, i, *it);
		i++;
	}
}
void opts_def()
{
	struct wsync *w = gg->wsync;
	w->srcfn = ffsz_dup("");
	w->dstfn = ffsz_dup("");
	w->showmask = (1<<FSYNC_ST_SRC) | (1<<FSYNC_ST_DEST) | (1<<FSYNC_ST_MOVED) | (1<<FSYNC_ST_NEQ)
		| SHOWMASK_DIRS | SHOWMASK_OLDER | SHOWMASK_NEWER;
}
void opts_load()
{
	struct wsync *w = gg->wsync;
	ffconf_parse_file(opts_args, w, w->opts_fn, 0, NULL);
	cols_width_read();
}

/** Save options to file */
void opts_save()
{
	struct wsync *w = gg->wsync;
	ffconfw conf;
	ffconfw_init(&conf, 0);

	ffconfw_addpairz(&conf, "source_path", w->srcfn);
	ffconfw_addpairz(&conf, "target_path", w->dstfn);
	ffconfw_addkeyz(&conf, "file_display_mask");
	ffconfw_addint(&conf, w->showmask);

	ffconfw_addkeyz(&conf, "columns_width");
	cols_width_write(&conf);

	ffconfw_fin(&conf);

	if (0 != fffile_writeall(w->opts_fn, conf.buf.ptr, conf.buf.len, 0)
		&& fferr_notexist(fferr_last())) {
		if (0 != ffdir_make_path(w->opts_fn, 0) && !fferr_exist(fferr_last())) {
			syserrlog("Can't create directory for the file: %s", w->opts_fn);
			goto end;
		}
		if (0 != fffile_writeall(w->opts_fn, conf.buf.ptr, conf.buf.len, 0)) {
			syserrlog("Can't write configuration file: %s", w->opts_fn);
			goto end;
		}
	}

	dbglog("saved settings to %s", w->opts_fn);

end:
	ffconfw_close(&conf);
}

void wsync_destroy()
{
	struct wsync *w = gg->wsync;
	_fsync->tree_free(w->src);  w->src = NULL;
	_fsync->tree_free(w->dst);  w->dst = NULL;
	ffmem_free(w->srcfn);
	ffmem_free(w->dstfn);
	ffstr_free(&w->show_excl);
	ffstr_free(&w->show_incl);
	ffmem_free(w->opts_fn);
	ffvec_free(&w->cmptbl);
	ffvec_free(&w->cmptbl_filter);
	ffvec_free(&w->col_width);
	ffmem_free(gg->wsync);
	ffmem_free(gg);
}
