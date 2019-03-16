/** GUI for file synchronization.
Copyright (c) 2018 Simon Zolin
*/

#include <fcom.h>

#include <FF/gui/loader.h>
#include <FF/gui/winapi.h>
#include <FF/data/conf.h>
#include <FF/time.h>
#include <FF/path.h>
#include <FF/number.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define infolog(fmt, ...)  fcom_infolog(FILT_NAME, fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, __VA_ARGS__)

extern const fcom_core *core;
extern const fcom_command *com;
static const fcom_fsync *fsync;
static const fcom_fops *fops;

static void wsync_action(ffui_wnd *wnd, int id);
static void wsync_destroy(ffui_wnd *wnd);
static void task_add(fftask_handler func);
static void update_status();
static void gsync_status(const char *s);
static void gsync_scancmp(void *udata);
static void gsync_showresults(uint flags);
static void gsync_sync(void *udata);
static void gsync_check(void);
static void gsync_fnop(uint id);
static void list_setdata();
#define isdir(a) !!((a) & FFUNIX_FILE_DIR)

struct opts;
static int opts_init(struct opts *c);
static int opts_load(struct opts *c);
static void opts_save(struct opts *c);
static void opts_show(const struct opts *c, ffui_view *v);
static void opts_set(struct opts *c, ffui_view *v, uint sub);

#define FILT_NAME  "gui.gsync"


/** Extensions to enum FSYNC_ST. */
enum {
	FSYNC_ST_ERROR = 1 << 28,
	FSYNC_ST_PENDING = 1 << 29, /** Operation on a file is pending */
	FSYNC_ST_CHECKED = 1 << 30,
	FSYNC_ST_DONE = 1 << 31,
};

enum SHOW_F {
	SHOW_TREE_ADD = 1, // add directories to tree
	SHOW_TREE_FILTER = 2, // filter by selected directory
};

enum L_COLS {
	L_ACTN,
	L_SRCNAME,
	L_SRCINFO,
	L_STATUS,
	L_DSTNAME,
	L_DSTINFO,
};

struct wsync {
	ffui_wnd wsync;
	ffui_view vopts;
	ffui_view tdirs;
	ffui_view vlist;
	ffui_paned pn;
	ffui_stbar stbar;
	ffui_menu mm;
};

enum VOPTS_COLUMNS {
	VOPTS_NAME,
	VOPTS_VAL,
	VOPTS_DESC,
};

struct opts {
	char *fn;
	char *srcfn;
	char *dstfn;
	ffstr filter;
	uint showmask; //enum FSYNC_ST
	byte show_dirs;
	byte time_diff_sec;
};

struct ggui {
	ffui_menu mcmd;
	ffui_menu mfile;
	struct wsync wsync;

	fsync_dir *src;
	fsync_dir *dst;
	ffarr cmptbl; // comparison results.  struct fsync_cmp[]
	ffarr cmptbl_filter; // filtered entries only.  struct fsync_cmp*[]
	struct opts opts;
	uint nchecked;
	char *filter_dirname; // full directory name to show entries in

	fftask tsk;
};

static struct ggui *gg;

static const ffui_ldr_ctl wsync_ctls[] = {
	FFUI_LDR_CTL(struct wsync, wsync),
	FFUI_LDR_CTL(struct wsync, vopts),
	FFUI_LDR_CTL(struct wsync, tdirs),
	FFUI_LDR_CTL(struct wsync, vlist),
	FFUI_LDR_CTL(struct wsync, pn),
	FFUI_LDR_CTL(struct wsync, stbar),
	FFUI_LDR_CTL(struct wsync, mm),
	FFUI_LDR_CTL_END
};

static const ffui_ldr_ctl top_ctls[] = {
	FFUI_LDR_CTL(struct ggui, mcmd),
	FFUI_LDR_CTL(struct ggui, mfile),

	FFUI_LDR_CTL3(struct ggui, wsync, wsync_ctls),
	FFUI_LDR_CTL_END
};

static void* gsync_getctl(void *udata, const ffstr *name)
{
	void *ctl = ffui_ldr_findctl(top_ctls, gg, name);
	return ctl;
}

enum CMDS {
	A_CMP = 1,
	A_SYNC,
	A_SNAPSAVE,
	A_SNAPLOAD,
	A_SWAP,
	A_FILTER,
	A_ONCHECK,
	A_OPENDIR,
	A_CLIPCOPY,
	A_CLIPFN,
	A_CLIPFN_RIGHT,
	A_CONF_EDIT,
	A_SELALL,
	A_EXIT,

	A_CONF_EDIT_DONE,
	A_DISPINFO,
};

static const char* const cmds[] = {
	"A_CMP",
	"A_SYNC",
	"A_SNAPSAVE",
	"A_SNAPLOAD",
	"A_SWAP",
	"A_FILTER",
	"A_ONCHECK",
	"A_OPENDIR",
	"A_CLIPCOPY",
	"A_CLIPFN",
	"A_CLIPFN_RIGHT",
	"A_CONF_EDIT",
	"A_SELALL",
	"A_EXIT",
};

static int gsync_getcmd(void *udata, const ffstr *name)
{
	int r;
	if (0 > (r = ffs_findarrz(cmds, FFCNT(cmds), name->ptr, name->len)))
		return -1;
	return r + 1;
}

/** Get comparison result entry by index. */
#define list_getobj(i) \
	*ffarr_itemT(&gg->cmptbl_filter, i, struct fsync_cmp*)


int gsync_create(void)
{
	int rc = -1;

	if (NULL == (fsync = core->iface("fsync.fsync")))
		return -1;

	if (NULL == (fops = core->iface("file.ops")))
		return -1;

	if (NULL == (gg = ffmem_new(struct ggui)))
		return -1;

	gg->wsync.vlist.dispinfo_id = A_DISPINFO;

	char *path = NULL;
	ffui_loader ldr;
	ffui_ldr_init(&ldr);

	ldr.getctl = &gsync_getctl;
	ldr.getcmd = &gsync_getcmd;
	ldr.udata = gg;
	if (NULL == (path = core->getpath(FFSTR("gsync.gui"))))
		goto end;
	if (0 != ffui_ldr_loadfile(&ldr, path)) {
		errlog("GUI loader: %s", ffui_ldr_errstr(&ldr));
		goto end;
	}

	gg->wsync.wsync.top = 1;
	gg->wsync.wsync.on_action = &wsync_action;
	gg->wsync.wsync.on_destroy = &wsync_destroy;
	gg->wsync.vopts.edit_id = A_CONF_EDIT_DONE;

	ffui_iconlist il;
	ffui_iconlist_create(&il, 16, 16);
	ffui_iconlist_addstd(&il, FFUI_ICON_DIR);
	ffui_tree_seticonlist(&gg->wsync.tdirs, &il);

	if (0 != opts_init(&gg->opts))
		goto end;
	opts_load(&gg->opts);
	opts_show(&gg->opts, &gg->wsync.vopts);

	rc = 0;

end:
	ffui_ldr_fin(&ldr);
	ffmem_safefree(path);
	return rc;
}


static void task_add(fftask_handler func)
{
	gg->tsk.handler = func;
	gg->tsk.param = NULL;
	core->task(FCOM_TASK_ADD, &gg->tsk);
}

static void wsync_action(ffui_wnd *wnd, int id)
{
	switch (id) {
	case A_CMP:
		task_add(&gsync_scancmp);
		break;
	case A_SYNC:
		task_add(&gsync_sync);
		break;
	case A_ONCHECK:
		gsync_check();
		break;

	case A_EXIT:
		ffui_wnd_close(&gg->wsync.wsync);
		break;

	case A_SNAPSAVE:
	case A_SNAPLOAD:
		break;

	case A_SWAP:
		gsync_reset();
		FF_SWAP2(gg->opts.srcfn, gg->opts.dstfn);
		ffui_view_clear(&gg->wsync.vopts);
		opts_show(&gg->opts, &gg->wsync.vopts);
		break;

	case A_SELALL:
		ffui_view_selall(&gg->wsync.vlist);
		break;

	case A_OPENDIR:
	case A_CLIPCOPY:
	case A_CLIPFN:
	case A_CLIPFN_RIGHT:
		gsync_fnop(id);
		break;

	case A_FILTER:
		gsync_showresults(SHOW_TREE_FILTER);
		break;

	case A_CONF_EDIT:
		ffui_view_edit_hittest(&gg->wsync.vopts, VOPTS_VAL);
		break;
	case A_CONF_EDIT_DONE:
		opts_set(&gg->opts, &gg->wsync.vopts, VOPTS_VAL);
		break;

	case A_DISPINFO:
		list_setdata();
		break;
	}
}


#define OFF(m)  FFPARS_DSTOFF(struct opts, m)
static const ffpars_arg opts_args[] = {
	{ "Source path",	FFPARS_TCHARPTR | FFPARS_FRECOPY | FFPARS_FSTRZ, OFF(srcfn) },
	{ "Target path",	FFPARS_TCHARPTR | FFPARS_FRECOPY | FFPARS_FSTRZ, OFF(dstfn) },
	{ "Compare: Time Diff in Seconds",	FFPARS_TINT8, OFF(time_diff_sec) },

	{ "Filter",	FFPARS_TSTR | FFPARS_FRECOPY, OFF(filter) },
	{ "Show Equal",	FFPARS_TINT | FFPARS_SETBIT(FSYNC_ST_EQ), OFF(showmask) },
	{ "Show Modified",	FFPARS_TINT | FFPARS_SETBIT(FSYNC_ST_NEQ), OFF(showmask) },
	{ "Show New",	FFPARS_TINT | FFPARS_SETBIT(FSYNC_ST_SRC), OFF(showmask) },
	{ "Show Deleted",	FFPARS_TINT | FFPARS_SETBIT(FSYNC_ST_DEST), OFF(showmask) },
	{ "Show Moved",	FFPARS_TINT | FFPARS_SETBIT(FSYNC_ST_MOVED), OFF(showmask) },
	{ "Show Directories",	FFPARS_TINT8, OFF(show_dirs) },
};
#undef OFF

static int opts_init(struct opts *c)
{
	c->srcfn = ffsz_alcopyz("");
	c->dstfn = ffsz_alcopyz("");
	c->showmask = -1;
	c->show_dirs = 1;
	return 0;
}

static void opts_destroy(struct opts *o)
{
	ffmem_safefree0(o->srcfn);
	ffmem_safefree0(o->dstfn);
	ffstr_free(&o->filter);
}

/** Load options from file. */
static int opts_load(struct opts *c)
{
	struct ffconf_loadfile conf = {0};
	if (NULL == (c->fn = core->env_expand(NULL, 0, "%APPDATA%\\fcom\\gsync.conf")))
		return -1;
	conf.fn = c->fn;
	conf.obj = c;
	conf.args = opts_args;
	conf.nargs = FFCNT(opts_args);
	dbglog(0, "reading %s", conf.fn);
	int r = ffconf_loadfile(&conf);
	if (r != 0 && !fferr_nofile(fferr_last())) {
		errlog("%s", conf.errstr);
	}
	return r;
}

/** Save options to file. */
static void opts_save(struct opts *c)
{
	char buf[64];
	ffconfw conf;
	ffui_loaderw ldr = {0};
	const ffpars_arg *a;
	ffstr s;
	ffconf_winit(&conf, NULL, 0);
	FFARRS_FOREACH(opts_args, a) {
		ffconf_write(&conf, a->name, ffsz_len(a->name), FFCONF_TKEY);
		ffpars_scheme_write(buf, a, c, &s);
		ffconf_write(&conf, s.ptr, s.len, FFCONF_TVAL);
	}
	ffconf_write(&conf, NULL, 0, FFCONF_FIN);
	ldr.confw = conf;
	if (0 != ffui_ldr_write(&ldr, c->fn) && fferr_nofile(fferr_last())) {
		if (0 != ffdir_make_path(c->fn, 0) && fferr_last() != EEXIST) {
			syserrlog("Can't create directory for the file: %s", c->fn);
			goto done;
		}
		if (0 != ffui_ldr_write(&ldr, c->fn)) {
			syserrlog("Can't write configuration file: %s", c->fn);
			goto done;
		}
	}

	dbglog(0, "saved settings to %s", c->fn);

done:
	ffui_ldrw_fin(&ldr);
}

/* struct opts -> GUI */
static void opts_show(const struct opts *c, ffui_view *v)
{
	union ffpars_val u;
	ffui_viewitem it = {0};
	char buf[128];

	for (uint i = 0;  i != FFCNT(opts_args);  i++) {

		const ffpars_arg *a = &opts_args[i];
		u.b = ffpars_arg_ptr(a, (void*)c);

		ffui_view_settextz(&it, a->name);
		ffui_view_append(v, &it);

		switch (a->flags & FFPARS_FTYPEMASK) {
		case FFPARS_TSTR:
			ffui_view_settextstr(&it, u.s);
			break;
		case FFPARS_TCHARPTR:
			ffui_view_settextz(&it, *u.charptr);
			break;
		case FFPARS_TINT: {
			int64 val = ffpars_getint(a, u);
			uint f;
			f = (a->flags & FFINT_SIGNED) ? FFINT_SIGNED : 0;
			uint n = ffs_fromint(val, buf, sizeof(buf), f);
			ffui_view_settext(&it, buf, n);
			break;
		}
		default:
			FF_ASSERT(0);
		}

		ffui_view_set(v, VOPTS_VAL, &it);

		ffui_view_settextz(&it, "");
		ffui_view_set(v, VOPTS_DESC, &it);
	}
	ffui_view_itemreset(&it);
}

/* GUI -> struct opts */
static void opts_set(struct opts *c, ffui_view *v, uint sub)
{
	int i = ffui_view_focused(v);
	int r;
	FF_ASSERT(i >= 0);

	ffstr text;
	ffstr_setz(&text, v->text);
	r = ffpars_arg_process(&opts_args[i], &text, c, NULL);
	if (ffpars_iserr(r))
		return;

	if (!ffsz_cmp(opts_args[i].name, "Source")) {
		r = ffpath_norm(gg->opts.srcfn, ffsz_len(gg->opts.srcfn), gg->opts.srcfn, ffsz_len(gg->opts.srcfn), FFPATH_MERGESLASH | FFPATH_MERGEDOTS | FFPATH_FORCESLASH);
		gg->opts.srcfn[r] = '\0';

	} else if (!ffsz_cmp(opts_args[i].name, "Target")) {
		r = ffpath_norm(gg->opts.dstfn, ffsz_len(gg->opts.dstfn), gg->opts.dstfn, ffsz_len(gg->opts.dstfn), FFPATH_MERGESLASH | FFPATH_MERGEDOTS | FFPATH_FORCESLASH);
		gg->opts.dstfn[r] = '\0';

	} else if (!ffsz_cmp(opts_args[i].name, "Filter")
		|| ffsz_match(opts_args[i].name, "Show ", 5))
		gsync_showresults(SHOW_TREE_FILTER);

	ffui_view_edit_set(v, i, sub);
}

static void wsync_destroy(ffui_wnd *wnd)
{
	ffmem_free(gg->filter_dirname);
	opts_save(&gg->opts);
	opts_destroy(&gg->opts);
	fsync->tree_free(gg->src);
	fsync->tree_free(gg->dst);
	ffarr_free(&gg->cmptbl);
	ffarr_free(&gg->cmptbl_filter);
	ffmem_free0(gg);
}

/** Scan source and target trees, compare and show results. */
static void gsync_scancmp(void *udata)
{
	void *cmpctx = NULL;
	ffui_view_clear(&gg->wsync.vlist);
	ffui_tree_clear(&gg->wsync.tdirs);

	fsync->tree_free(gg->src);  gg->src = NULL;
	fsync->tree_free(gg->dst);  gg->dst = NULL;
	ffarr_free(&gg->cmptbl);

	if (NULL == (gg->src = fsync->scan_tree(gg->opts.srcfn)))
		goto end;
	if (NULL == (gg->dst = fsync->scan_tree(gg->opts.dstfn)))
		goto end;

	struct fsync_cmp cmp, *c;
	uint f = FSYNC_CMP_DEFAULT;
	if (gg->opts.time_diff_sec)
		f |= FSYNC_CMP_MTIME_SEC;
	cmpctx = fsync->cmp_init(gg->src, gg->dst, f);
	for (;;) {
		if (0 != fsync->cmp_trees(cmpctx, &cmp))
			break;
		if ((cmp.status & _FSYNC_ST_MASK) == FSYNC_ST_MOVED
			&& cmp.status & FSYNC_ST_MOVED_DST)
			continue;
		if (NULL == (c = ffarr_pushgrowT(&gg->cmptbl, 256 | FFARR_GROWQUARTER, struct fsync_cmp)))
			goto end;
		*c = cmp;
	}

	gsync_showresults(SHOW_TREE_ADD);

end:
	if (cmpctx != NULL)
		fsync->cmp_trees(cmpctx, NULL);
	return;
}

/** Get file info as text. */
static int gsync_finfo(const struct fsync_file *e, ffarr *buf)
{
	ffdtm dt;

	if (isdir(e->attr)) {
		if (0 == ffstr_catfmt(buf, "[DIR], "))
			return 1;

	} else {
		if (0 == ffstr_catfmt(buf, "%,U Bytes, "
			, e->size))
			return 1;
	}

	if (NULL == ffarr_grow(buf, 64, 0))
		return 1;
	fftime_split(&dt, &e->mtime, FFTIME_TZLOCAL);
	buf->len += fftime_tostr(&dt, ffarr_end(buf), ffarr_unused(buf), FFTIME_DATE_MDY | FFTIME_HMS);
	return 0;
}

static void update_status()
{
	ffarr buf = {};
	ffstr_catfmt(&buf, "Results: %L/%L (%L)%Z"
		, (size_t)gg->nchecked
		, (size_t)gg->cmptbl_filter.len, gg->cmptbl.len);
	gsync_status(buf.ptr);
	ffarr_free(&buf);
}

static void gsync_status(const char *s)
{
	ffui_stbar_settextz(&gg->wsync.stbar, 0, s);
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
			, cmp_sstatus[st & _FSYNC_ST_MASK], cmp_sstatus_tm[itm], cmp_sstatus_sz[isz]);
		return buf;
	}

	return cmp_sstatus[st & _FSYNC_ST_MASK];
}

#define fsfile(/* struct file* */f)  ((struct fsync_file*)(f))

static const char* const actionstr[] = {
	"",
	"Copy",
	"Delete",
	"Move",
	"Overwrite",
};

/** Check whether we should show the entry:
. Show [TYPE] settings
. Show only files inside a specific directory
. Show only files containing a specific text */
static ffbool cmpent_visible(const struct fsync_cmp *c)
{
	uint st = c->status & _FSYNC_ST_MASK;
	struct fsync_file *e1, *e2;
	e1 = fsfile(c->left);
	e2 = fsfile(c->right);

	if (!(ffbit_test32(&gg->opts.showmask, st)))
		return 0;

	if (!gg->opts.show_dirs && st != FSYNC_ST_DEST && isdir(e1->attr))
		return 0;

	if (gg->filter_dirname != NULL) {
		if (st == FSYNC_ST_DEST)
			return 0;
		char *dirname = fsync->get(FSYNC_DIRNAME, c->left);
		if (!ffsz_eq(dirname, gg->filter_dirname))
			return 0;
	}

	if (gg->opts.filter.len != 0) {
		ffstr n1 = {}, n2 = {};
		if (st != FSYNC_ST_DEST)
			ffstr_setz(&n1, e1->name);
		if (st != FSYNC_ST_SRC)
			ffstr_setz(&n2, e2->name);
		if (-1 == ffstr_ifindstr(&n1, &gg->opts.filter)
			&& -1 == ffstr_ifindstr(&n2, &gg->opts.filter))
			return 0;
	}

	return 1;
}

/** Set data for a listview item. */
static void list_setdata()
{
	ffarr buf = {};
	char *fullname = NULL;
	ffstr d = {};
	LVITEM *it = gg->wsync.vlist.dispinfo_item;

	const struct fsync_cmp *c = list_getobj(it->iItem);
	uint st = c->status & _FSYNC_ST_MASK;

	if (!cmpent_visible(c))
		return;

	if (it->mask & LVIF_STATE)
		ffui_view_dispinfo_check(it, (c->status & FSYNC_ST_CHECKED));

	if (!(it->mask & LVIF_TEXT))
		return;

	struct fsync_file *e1, *e2;
	e1 = fsfile(c->left);
	e2 = fsfile(c->right);

	switch (it->iSubItem) {

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
			fullname = fsync->get(FSYNC_FULLNAME, e1);
			ffstr_setz(&d, fullname);
		}
		break;

	case L_SRCINFO:
		if (st != FSYNC_ST_DEST) {
			if (0 != gsync_finfo(e1, &buf))
				goto end;
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
			fullname = fsync->get(FSYNC_FULLNAME, e2);
			ffstr_setz(&d, fullname);
		}
		break;

	case L_DSTINFO:
		if (st != FSYNC_ST_SRC) {
			if (0 != gsync_finfo(e2, &buf))
				goto end;
			ffstr_set2(&d, &buf);
		}
		break;
	}

	if (d.len != 0)
		ffui_view_dispinfo_settext(it, d.ptr, d.len);

end:
	ffarr_free(&buf);
	ffmem_safefree(fullname);
}

/** Reset views and apply filtering settings.
. Uncheck all entries
. Create a list of filtered entries */
static void gsync_showresults(uint flags)
{
	struct fsync_file *e1;
	struct fsync_cmp *c;
	ffarr buf = {0};
	char *fullname = NULL;
	size_t n = 0;
	void *troot;

	gg->cmptbl_filter.len = 0;
	gg->nchecked = 0;
	ffarr_alloc(&buf, 64);

	if (flags & SHOW_TREE_ADD) {
		ffui_redraw(&gg->wsync.tdirs, 0);
		ffui_tree_clear(&gg->wsync.tdirs);
		ffui_tvitem it = {0};
		ffui_tree_settextz(&it, gg->opts.srcfn);
		ffui_tree_setexpand(&it, 1);
		troot = ffui_tree_append(&gg->wsync.tdirs, NULL, &it);
	}

	if (flags & SHOW_TREE_FILTER) {
		void *tsel;
		if (NULL != (tsel = ffui_tree_focused(&gg->wsync.tdirs))) {
			ffui_tvitem it = {0};
			ffui_tree_setparam(&it, NULL);
			ffui_tree_get(&gg->wsync.tdirs, tsel, &it);
			ffmem_free0(gg->filter_dirname);
			if (NULL != (e1 = ffui_tree_param(&it))) {
				gg->filter_dirname = fsync->get(FSYNC_FULLNAME, e1);
			}
		}
	}

	FFARR_WALKT(&gg->cmptbl, c, struct fsync_cmp) {

		c->status &= ~FSYNC_ST_CHECKED;

		uint st = c->status & _FSYNC_ST_MASK;
		e1 = fsfile(c->left);

		if (st != FSYNC_ST_DEST && (flags & SHOW_TREE_ADD) && isdir(e1->attr)) {
			ffmem_free(fullname);
			fullname = fsync->get(FSYNC_FULLNAME, c->left);
			ffui_tvitem it = {0};
			ffui_tree_settextz(&it, fullname);
			ffui_tree_setparam(&it, e1);
			ffui_tree_append(&gg->wsync.tdirs, troot, &it);
		}

		if (!cmpent_visible(c))
			continue;

		void **p;
		if (NULL == (p = ffarr_pushgrowT(&gg->cmptbl_filter, 256 | FFARR_GROWQUARTER, void*)))
			goto end;
		*p = c;

		n++;
	}

	if (flags & SHOW_TREE_ADD)
		ffui_redraw(&gg->wsync.tdirs, 1);

	ffui_view_setcount_redraw(&gg->wsync.vlist, n);

	ffstr_fmt(&buf, "Results: %L (%L)%Z", n, gg->cmptbl.len);
	gsync_status(buf.ptr);

end:
	ffarr_free(&buf);
	ffmem_safefree(fullname);
}


struct sync_ctx {
	struct fsync_cmp *cmp;
	char *fnL, *fnR;
	uint row_index;
};

static void sync_cmdmon_onsig(fcom_cmd *cmd, uint sig);
static const struct fcom_cmd_mon sync_cmdmon = { &sync_cmdmon_onsig };
static void sync(struct sync_ctx *sc);

/** Start synchronization. */
static void gsync_sync(void *udata)
{
	struct sync_ctx *sc = ffmem_new(struct sync_ctx);
	if (sc == NULL)
		return;
	sc->row_index = -1;
	sync(sc);
}

static void sync_reset(struct sync_ctx *sc)
{
	ffmem_free0(sc->fnL);
	ffmem_free0(sc->fnR);
	sc->cmp = NULL;
}

static void sync_free(struct sync_ctx *sc)
{
	sync_reset(sc);
	ffmem_free(sc);
}

/** Operation on a file is complete. */
static void sync_result(struct sync_ctx *sc, int r)
{
	if (r != 0) {
		sc->cmp->status |= FSYNC_ST_ERROR;
	} else {
		sc->cmp->status &= ~FSYNC_ST_CHECKED;
		sc->cmp->status |= FSYNC_ST_DONE;
		gg->nchecked--;
	}
	ffui_view_redraw(&gg->wsync.vlist, sc->row_index, sc->row_index);

	update_status();
}

static void sync_cmdmon_onsig(fcom_cmd *cmd, uint sig)
{
	struct sync_ctx *sc = (void*)com->ctrl(cmd, FCOM_CMD_UDATA);
	int r = (!cmd->err) ? 0 : -1;
	sync_result(sc, r);
	sync_reset(sc);
	sync(sc);
}

/** Start file copy task. */
static int sync_copy(struct sync_ctx *sc, const char *src, const char *dst, uint flags)
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
	com->fcom_cmd_monitor(c, &sync_cmdmon);
	com->ctrl(c, FCOM_CMD_SETUDATA, sc);
	com->ctrl(c, FCOM_CMD_RUNASYNC);
	return FCOM_ASYNC;
}

// "src/path/file" -> "dst/path/file"
static char* dst_fn(const char *fnL)
{
	if (!ffsz_matchz(fnL, gg->opts.srcfn)) {
		errlog("filename %s doesn't match path %s", fnL, gg->opts.srcfn);
		return NULL;
	}
	return ffsz_alfmt("%s/%s", gg->opts.dstfn, fnL + ffsz_len(gg->opts.srcfn));
}

/** Synchronize checked files (src => dst).
FSYNC_ST_NEQ: if file size has changed or contents don't match, copy file;
  otherwise just set attributes.
Suspend processing while file copy task is running asynchronously.
*/
static void sync(struct sync_ctx *sc)
{
	struct fsync_cmp *cmp;
	struct fsync_file *f;
	int r;

	for (uint i = sc->row_index + 1;  i != gg->cmptbl_filter.len;  i++) {

		sc->row_index = i;
		sc->cmp = cmp = list_getobj(i);
		if (!(cmp->status & FSYNC_ST_CHECKED))
			continue;

		if ((cmp->status & _FSYNC_ST_MASK) == FSYNC_ST_EQ)
			continue;

		cmp->status |= FSYNC_ST_PENDING;
		ffui_view_redraw(&gg->wsync.vlist, i, i);

		switch (cmp->status & _FSYNC_ST_MASK) {

		case FSYNC_ST_SRC:
			sc->fnL = fsync->get(FSYNC_FULLNAME, cmp->left);
			f = fsfile(cmp->left);
			sc->fnR = dst_fn(sc->fnL);
			if (sc->fnL == NULL || sc->fnR == NULL)
				goto end;

			if (isdir(f->attr))
				r = fops->mkdir(sc->fnR, 0);
			else
				r = sync_copy(sc, sc->fnL, sc->fnR, FOP_KEEPDATE);
			break;

		case FSYNC_ST_NEQ:
			sc->fnL = fsync->get(FSYNC_FULLNAME, cmp->left);
			sc->fnR = fsync->get(FSYNC_FULLNAME, cmp->right);
			if (sc->fnL == NULL || sc->fnR == NULL)
				goto end;

			if (cmp->status & (FSYNC_ST_SMALLER | FSYNC_ST_LARGER)
				|| !!fffile_cmp(sc->fnL, sc->fnR, 0)) {

				r = sync_copy(sc, sc->fnL, sc->fnR, FOP_OVWR | FOP_KEEPDATE);

			} else {
				fffileinfo fi;
				if (0 == (r = fffile_infofn(sc->fnL, &fi))) {

					if (cmp->status & (FSYNC_ST_OLDER | FSYNC_ST_NEWER)) {
						fftime t = fffile_infomtime(&fi);
						r = fops->time(sc->fnR, &t, 0);
					}

					if (cmp->status & FSYNC_ST_ATTR) {
						uint attr = fffile_infoattr(&fi);
						fffile_attrsetfn(sc->fnR, attr);
					}
				}
			}
			break;

		case FSYNC_ST_MOVED: {
			sc->fnL = fsync->get(FSYNC_FULLNAME, cmp->right);
			char *L = fsync->get(FSYNC_FULLNAME, cmp->left);
			if (L == NULL)
				goto end;
			sc->fnR = dst_fn(L);
			ffmem_free(L);
			if (sc->fnL == NULL || sc->fnR == NULL)
				goto end;
			r = fops->move(sc->fnL, sc->fnR, FOP_OVWR | FOP_RECURS);
			break;
		}

		case FSYNC_ST_DEST:
			sc->fnR = fsync->get(FSYNC_FULLNAME, cmp->right);
			if (sc->fnR == NULL)
				goto end;
			r = fops->del(sc->fnR, FOP_DIR);
			break;

		default:
			r = -1;
		}

		if (r == FCOM_ASYNC)
			return;

		sync_result(sc, r);
		sync_reset(sc);
	}

end:
	sync_free(sc);
}

/** Check/uncheck the focused item and all selected items */
static void gsync_check(void)
{
	int i, check, first = -1;
	struct fsync_cmp *cmp;

	i = gg->wsync.vlist.idx;
	cmp = list_getobj(i);
	gg->nchecked += (cmp->status & FSYNC_ST_CHECKED) ? -1 : 1;
	ffint_bitmask(&cmp->status, FSYNC_ST_CHECKED, !(cmp->status & FSYNC_ST_CHECKED));
	check = !!(cmp->status & FSYNC_ST_CHECKED);
	first = i;

	ffui_viewitem it = {};
	ffui_view_setindex(&it, i);
	ffui_view_select(&it, 0);
	ffui_view_get(&gg->wsync.vlist, 0, &it);
	if (!ffui_view_selected(&it))
		goto done;

	i = -1;
	while (-1 != (i = ffui_view_selnext(&gg->wsync.vlist, i))) {

		if (i < first)
			first = i;

		cmp = list_getobj(i);
		if (check == !!(cmp->status & FSYNC_ST_CHECKED))
			continue;

		gg->nchecked += (cmp->status & FSYNC_ST_CHECKED) ? -1 : 1;
		ffint_bitmask(&cmp->status, FSYNC_ST_CHECKED, !(cmp->status & FSYNC_ST_CHECKED));
	}

done:
	ffui_view_redraw(&gg->wsync.vlist, first, first + 100);
	update_status();
	ffui_view_itemreset(&it);
}

/** Do operation with file names. */
static void gsync_fnop(uint id)
{
	int i = -1;
	struct fsync_cmp *c;
	ffarr buf = {0}, b2 = {0};
	char *fullname = NULL;
	void *obj;

	for (;;) {
		if (-1 == (i = ffui_view_selnext(&gg->wsync.vlist, i)))
			break;
		c = list_getobj(i);

		switch (id) {
		case A_CLIPFN:
		case A_OPENDIR:
		case A_CLIPCOPY:
			obj = c->left;
			break;
		case A_CLIPFN_RIGHT:
			obj = c->right;
			break;
		}
		if (obj == NULL)
			continue;
		ffmem_safefree(fullname);
		fullname = fsync->get(FSYNC_FULLNAME, obj);

		switch (id) {

		case A_CLIPFN:
		case A_CLIPFN_RIGHT:
			if (0 == ffstr_catfmt(&buf, "%s\n", fullname))
				goto end;
			break;

		case A_OPENDIR:
		case A_CLIPCOPY: {
			char **s;
			if (NULL == (s = ffarr_pushgrowT(&buf, 16, char*)))
				goto end;
			if (0 == ffstr_catfmt(&b2, "%s%Z", fullname))
				goto end;
			*s = b2.ptr;
			ffarr_null(&b2);
			break;
		}
		}
	}

	if (buf.len == 0)
		goto end;

	switch (id) {

	case A_OPENDIR:
		ffui_openfolder((const char**)buf.ptr, buf.len);
		break;

	case A_CLIPCOPY:
		ffui_clipbd_setfile((const char**)buf.ptr, buf.len);
		break;

	case A_CLIPFN:
	case A_CLIPFN_RIGHT:
		ffui_clipbd_set(buf.ptr, buf.len);
		break;
	}

end:
	if (id == A_OPENDIR || id == A_CLIPCOPY) {
		char **s;
		FFARR_WALKT(&buf, s, char*) {
			ffmem_free(*s);
		}
	}
	ffarr_free(&buf);
	ffmem_safefree(fullname);
}
