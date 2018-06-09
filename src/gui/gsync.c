/** GUI for file synchronization.
Copyright (c) 2018 Simon Zolin
*/

#include <fcom.h>

#include <FF/gui/loader.h>
#include <FF/gui/winapi.h>
#include <FF/data/conf.h>
#include <FF/time.h>
#include <FF/path.h>


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
#define isdir(a) !!((a) & FFUNIX_FILE_DIR)

struct opts;
static int opts_init(struct opts *c);
static int opts_load(struct opts *c);
static void opts_save(struct opts *c);
static void opts_show(const struct opts *c, ffui_view *v);
static void opts_set(struct opts *c, ffui_view *v, uint sub);

#define FILT_NAME  "gui.gsync"


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
};

struct ggui {
	ffui_menu mcmd;
	ffui_menu mfile;
	struct wsync wsync;

	fsync_dir *src;
	fsync_dir *dst;
	ffarr cmptbl;
	struct opts opts;
	uint nchecked;

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
	A_FILTER,
	A_ONCHECK,
	A_OPENDIR,
	A_CLIPCOPY,
	A_CLIPFN,
	A_CONF_EDIT,
	A_SELALL,
	A_EXIT,

	A_CONF_EDIT_DONE,
};

static const char* const cmds[] = {
	"A_CMP",
	"A_SYNC",
	"A_SNAPSAVE",
	"A_SNAPLOAD",
	"A_FILTER",
	"A_ONCHECK",
	"A_OPENDIR",
	"A_CLIPCOPY",
	"A_CLIPFN",
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


int gsync_create(void)
{
	int rc = -1;

	if (NULL == (fsync = core->iface("fsync.fsync")))
		return -1;

	if (NULL == (fops = core->iface("file.ops")))
		return -1;

	if (NULL == (gg = ffmem_new(struct ggui)))
		return -1;

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

	case A_SELALL:
		ffui_view_selall(&gg->wsync.vlist);
		break;

	case A_OPENDIR:
	case A_CLIPCOPY:
	case A_CLIPFN:
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
	}
}


#define OFF(m)  FFPARS_DSTOFF(struct opts, m)
static const ffpars_arg opts_args[] = {
	{ "Source path",	FFPARS_TCHARPTR | FFPARS_FRECOPY | FFPARS_FSTRZ, OFF(srcfn) },
	{ "Target path",	FFPARS_TCHARPTR | FFPARS_FRECOPY | FFPARS_FSTRZ, OFF(dstfn) },

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
	ffui_ldr_write(&ldr, c->fn);
	ffui_ldrw_fin(&ldr);
	dbglog(0, "saved settings to %s", c->fn);
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
	opts_save(&gg->opts);
	opts_destroy(&gg->opts);
	fsync->tree_free(gg->src);
	fsync->tree_free(gg->dst);
	ffarr_free(&gg->cmptbl);
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
	cmpctx = fsync->cmp_init(gg->src, gg->dst, 0);
	for (;;) {
		if (0 != fsync->cmp_trees(cmpctx, &cmp))
			break;
		if ((cmp.status & _FSYNC_ST_MASK) == FSYNC_ST_MOVED
			&& cmp.status & FSYNC_ST_MOVED_DST)
			continue;
		if (NULL == (c = ffarr_pushgrowT(&gg->cmptbl, 256, struct fsync_cmp)))
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
		, (size_t)ffui_view_nitems(&gg->wsync.vlist), gg->cmptbl.len);
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

#define fsfile(/* struct file* */f)  ((struct fsync_file*)(f))

static const char* const actionstr[] = {
	"",
	"Copy",
	"Delete",
	"Move",
	"Overwrite",
};

static void gsync_showresults(uint flags)
{
	struct fsync_file *e1, *e2;
	struct fsync_cmp *c;
	ffui_viewitem it = {0};
	ffarr buf = {0};
	char *fullname = NULL;
	size_t n = 0;
	char *showdir = NULL;
	void *troot;

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
			if (NULL != (e1 = ffui_tree_param(&it))) {
				showdir = fsync->get(FSYNC_FULLNAME, e1);
			}
		}
	}

	ffui_redraw(&gg->wsync.vlist, 0);
	ffui_view_clear(&gg->wsync.vlist);

	FFARR_WALKT(&gg->cmptbl, c, struct fsync_cmp) {

		uint st = c->status & _FSYNC_ST_MASK;

		if (!(ffbit_test32(&gg->opts.showmask, st)))
			continue;

		e1 = fsfile(c->left);
		e2 = fsfile(c->right);
		ffstr n1 = {0}, n2 = {0};
		if (st != FSYNC_ST_DEST)
			ffstr_setz(&n1, e1->name);
		if (st != FSYNC_ST_SRC)
			ffstr_setz(&n2, e2->name);

		if (!gg->opts.show_dirs && st != FSYNC_ST_DEST && isdir(e1->attr))
			continue;

		if (showdir != NULL && st != FSYNC_ST_DEST) {
			char *dirname = fsync->get(FSYNC_DIRNAME, c->left);
			if (!ffsz_eq(dirname, showdir))
				continue;
		}

		if (gg->opts.filter.len != 0
			&& -1 == ffstr_ifindstr(&n1, &gg->opts.filter)
			&& -1 == ffstr_ifindstr(&n2, &gg->opts.filter))
			continue;

		ffui_view_setparam(&it, c);
		ffui_view_settextz(&it, actionstr[st]);
		ffui_view_append(&gg->wsync.vlist, &it);

		ffui_view_settextz(&it, cmp_status_str(buf.ptr, buf.cap, c));
		buf.len = 0;
		ffui_view_set(&gg->wsync.vlist, L_STATUS, &it);

		if (st != FSYNC_ST_DEST) {

			if ((flags & SHOW_TREE_ADD) && isdir(e1->attr)) {
				ffui_tvitem it = {0};
				ffui_tree_settextz(&it, e1->name);
				ffui_tree_setparam(&it, e1);
				ffui_tree_append(&gg->wsync.tdirs, troot, &it);
			}

			ffmem_safefree(fullname);
			fullname = fsync->get(FSYNC_FULLNAME, e1);
			ffui_view_settextz(&it, fullname);
			ffui_view_set(&gg->wsync.vlist, L_SRCNAME, &it);

			if (0 != gsync_finfo(e1, &buf))
				goto end;
			ffui_view_settextstr(&it, &buf);
			buf.len = 0;
			ffui_view_set(&gg->wsync.vlist, L_SRCINFO, &it);
		}

		if (st != FSYNC_ST_SRC) {
			ffmem_safefree(fullname);
			fullname = fsync->get(FSYNC_FULLNAME, e2);
			ffui_view_settextz(&it, fullname);
			ffui_view_set(&gg->wsync.vlist, L_DSTNAME, &it);

			if (0 != gsync_finfo(e2, &buf))
				goto end;
			ffui_view_settextstr(&it, &buf);
			buf.len = 0;
			ffui_view_set(&gg->wsync.vlist, L_DSTINFO, &it);
		}

		n++;
	}

	ffui_redraw(&gg->wsync.vlist, 1);
	ffui_redraw(&gg->wsync.tdirs, 1);

	buf.len = 0;
	ffstr_catfmt(&buf, "Results: %L (%L)%Z", n, gg->cmptbl.len);
	gsync_status(buf.ptr);

end:
	ffarr_free(&buf);
	ffmem_safefree(fullname);
	ffmem_safefree(showdir);
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

/** Operation on a file is pending. */
static void sync_pending(struct sync_ctx *sc)
{
	ffui_viewitem it = {0};
	ffui_view_setindex(&it, sc->row_index);
	ffui_view_settextz(&it, "...");
	ffui_view_set(&gg->wsync.vlist, L_ACTN, &it);
}

/** Operation on a file is complete. */
static void sync_result(struct sync_ctx *sc, int r)
{
	ffui_viewitem it = {0};
	ffui_view_setindex(&it, sc->row_index);
	if (r != 0) {
		ffui_view_settextz(&it, "Error");
	} else {
		ffui_view_check(&it, 0);
		gg->nchecked--;
		ffui_view_settextz(&it, "Done");
	}
	ffui_view_set(&gg->wsync.vlist, 0, &it);

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
	ffui_viewitem it = {0};
	struct fsync_cmp *cmp;
	struct fsync_file *f;
	int r;

	for (uint i = sc->row_index + 1;  ;  i++) {

		ffui_view_setindex(&it, i);
		ffui_view_setparam(&it, NULL);
		ffui_view_check(&it, 0);
		if (0 != ffui_view_get(&gg->wsync.vlist, 0, &it))
			break;

		if (!ffui_view_checked(&it))
			continue;

		sc->row_index = i;

		sc->cmp = cmp = (void*)ffui_view_param(&it);
		ffui_view_itemreset(&it);

		if ((cmp->status & _FSYNC_ST_MASK) == FSYNC_ST_EQ)
			continue;

		sync_pending(sc);

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

/** Check/uncheck all selected items */
static void gsync_check(void)
{
	ffui_viewitem it = {0};
	int i, check;

	i = gg->wsync.vlist.idx;
	ffui_view_setindex(&it, i);
	ffui_view_select(&it, 0);
	ffui_view_check(&it, 0);
	ffui_view_get(&gg->wsync.vlist, 0, &it);
	check = ffui_view_checked(&it);
	gg->nchecked += (check) ? 1 : -1;
	if (!ffui_view_selected(&it))
		goto done;
	ffui_view_itemreset(&it);

	ffui_redraw(&gg->wsync.vlist, 0);

	i = -1;
	while (-1 != (i = ffui_view_selnext(&gg->wsync.vlist, i))) {
		ffui_view_setindex(&it, i);
		ffui_view_check(&it, 0);
		ffui_view_get(&gg->wsync.vlist, 0, &it);
		int check2 = ffui_view_checked(&it);
		ffui_view_itemreset(&it);
		if (check2 == check)
			continue;

		ffui_view_setindex(&it, i);
		ffui_view_check(&it, check);
		ffui_view_set(&gg->wsync.vlist, 0, &it);
		gg->nchecked += (check) ? 1 : -1;
	}

	ffui_redraw(&gg->wsync.vlist, 1);

done:
	update_status();
	ffui_view_itemreset(&it);
}

/** Do operation with file names. */
static void gsync_fnop(uint id)
{
	int i = -1;
	struct fsync_cmp *c;
	ffui_viewitem it = {0};
	ffarr buf = {0}, b2 = {0};
	char *fullname = NULL;

	for (;;) {
		if (-1 == (i = ffui_view_selnext(&gg->wsync.vlist, i)))
			break;
		ffui_view_setindex(&it, i);
		ffui_view_setparam(&it, NULL);
		ffui_view_get(&gg->wsync.vlist, 0, &it);
		c = (void*)ffui_view_param(&it);

		ffmem_safefree(fullname);
		fullname = fsync->get(FSYNC_FULLNAME, c->left);
		if (id == A_CLIPFN) {
			if (0 == ffstr_catfmt(&buf, "%s\n", fullname))
				goto end;
		} else {
			char **s;
			if (NULL == (s = ffarr_pushgrowT(&buf, 16, char*)))
				goto end;
			if (0 == ffstr_catfmt(&b2, "%s%Z", fullname))
				goto end;
			*s = b2.ptr;
			ffarr_null(&b2);
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
		ffui_clipbd_set(buf.ptr, buf.len);
		break;
	}

end:
	if (id != A_CLIPFN) {
		char **s;
		FFARR_WALKT(&buf, s, char*) {
			ffmem_free(*s);
		}
	}
	ffarr_free(&buf);
	ffmem_safefree(fullname);
}
