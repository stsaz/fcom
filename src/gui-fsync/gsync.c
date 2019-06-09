/** GUI for file synchronization.
Copyright (c) 2018 Simon Zolin
*/

#include <gui-fsync/gsync.h>

#include <FF/time.h>
#include <FF/sys/dir.h>
#include <FF/number.h>

static void wsync_action(ffui_wnd *wnd, int id);
static void wsync_destroy(ffui_wnd *wnd);
static void task_add(fftask_handler func);
static void gsync_reset();
static void gsync_status(const char *s);
static void gsync_scancmp(void *udata);
static void gsync_showresults(uint flags);
static void gsync_check(void);
static void gsync_fnop(uint id);
static void list_setdata();

struct ggui *gg;
const fcom_fsync *fsync;
const fcom_fops *fops;


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

static const ffui_ldr_ctl wtree_ctls[] = {
	FFUI_LDR_CTL(struct wtree, wnd),
	FFUI_LDR_CTL(struct wtree, tdirs),
	FFUI_LDR_CTL(struct wtree, eaddr),
	FFUI_LDR_CTL(struct wtree, vlist),
	FFUI_LDR_CTL(struct wtree, pn),
	FFUI_LDR_CTL_END
};

static const ffui_ldr_ctl top_ctls[] = {
	FFUI_LDR_CTL(struct ggui, mcmd),
	FFUI_LDR_CTL(struct ggui, mfile),
	FFUI_LDR_CTL(struct ggui, dlg),

	FFUI_LDR_CTL3(struct ggui, wsync, wsync_ctls),
	FFUI_LDR_CTL3(struct ggui, wtree, wtree_ctls),
	FFUI_LDR_CTL_END
};

static void* gsync_getctl(void *udata, const ffstr *name)
{
	void *ctl = ffui_ldr_findctl(top_ctls, gg, name);
	return ctl;
}

enum {
	A_CONF_EDIT_DONE = 1000,
	A_DISPINFO,
};

static const char* const cmds[] = {
	"A_CMP",
	"A_SYNC",
	"A_SNAPSAVE",
	"A_SNAPLOAD",
	"A_SNAPLOAD_RIGHT",
	"A_SNAPSHOW",
	"A_SNAPSHOW_RIGHT",
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

	"A_TREE_ENTER",
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

	gg->wsync.vlist.dispinfo_id = A_DISPINFO;
	tree_preinit();

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
	tree_init();

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

static void task_add2(fftask_handler func, void *udata)
{
	gg->tsk.handler = func;
	gg->tsk.param = udata;
	core->task(FCOM_TASK_ADD, &gg->tsk);
}

struct snap {
	char *name;
	uint id;
};

static void snapload(void *udata)
{
	struct snap *s = udata;
	fsync_dir *d = fsync->scan_tree(s->name, FSYNC_SCAN_SNAPSHOT);
	if (s->id == A_SNAPLOAD)
		gg->src = d;
	else
		gg->dst = d;
	ffmem_free(s->name);
	ffmem_free(s);
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
		break;
	case A_SNAPLOAD:
	case A_SNAPLOAD_RIGHT: {
		struct snap *s = ffmem_new(struct snap);
		s->id = id;
		char *fn = ffui_dlg_open(&gg->dlg, &gg->wsync.wsync);
		if (fn == NULL)
			break;
		s->name = ffsz_alcopyz(fn);
		task_add2(&snapload, s);
		break;
	}

	case A_SNAPSHOW:
		if (gg->src == NULL)
			break;
		ffui_show(&gg->wtree.wnd, 1);
		tree_show(gg->src);
		break;
	case A_SNAPSHOW_RIGHT:
		if (gg->dst == NULL)
			break;
		ffui_show(&gg->wtree.wnd, 1);
		tree_show(gg->dst);
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
		if (1 == opts_set(&gg->opts, &gg->wsync.vopts, VOPTS_VAL))
			gsync_showresults(SHOW_TREE_FILTER);
		break;

	case A_DISPINFO:
		list_setdata();
		break;
	}
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

static void gsync_reset()
{
	ffui_view_clear(&gg->wsync.vlist);
	ffui_tree_clear(&gg->wsync.tdirs);

	fsync->tree_free(gg->src);  gg->src = NULL;
	fsync->tree_free(gg->dst);  gg->dst = NULL;
	ffarr_free(&gg->cmptbl);

	gg->cmptbl_filter.len = 0;
	gg->nchecked = 0;
}

/** Scan directories, possibly with wildcards. */
static fsync_dir* scan(const char *path, uint flags)
{
	ffstr p;
	ffstr_setz(&p, path);
	ffdirexp de;
	const char *fn;
	char *wc = NULL;
	fsync_dir *r = NULL, *d = NULL, *parent = NULL;

	if (NULL == ffs_findc(p.ptr, p.len, '*'))
		return fsync->scan_tree(path, 0);

	if (NULL == (wc = ffsz_alcopyz(path)))
		goto done;
	if (0 != ffdir_expopen(&de, wc, 0))
		goto done;

	while (NULL != (fn = ffdir_expread(&de))) {
		if (NULL == (d = fsync->scan_tree(fn, flags)))
			goto done;
		if (parent != NULL) {
			fsync_dir *dnew;
			if (NULL == (dnew = fsync->combine(parent, d, 0)))
				goto done;
			d = dnew;
		}
		parent = d;
		d = NULL;
	}

	r = parent;

done:
	ffdir_expclose(&de);
	ffmem_free(wc);
	if (r == NULL) {
		fsync->tree_free(d);
		fsync->tree_free(parent);
	}
	return r;
}

/** Scan source and target trees, compare and show results. */
static void gsync_scancmp(void *udata)
{
	void *cmpctx = NULL;

	gsync_reset();

	gsync_status("Scanning Source...");
	if (NULL == (gg->src = scan(gg->opts.srcfn, 0)))
		goto end;

	gsync_status("Scanning Target...");
	if (NULL == (gg->dst = scan(gg->opts.dstfn, 0)))
		goto end;

	gsync_status("Comparing...");
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

void update_status()
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
	if (st == FSYNC_ST_NEQ) {
		uint v = gg->opts.show_modmask;
		uint m = 0;
		if (v & FF_BIT32(0))
			m |= FSYNC_ST_OLDER | FSYNC_ST_NEWER;
		if (v & FF_BIT32(1))
			m |= FSYNC_ST_SMALLER | FSYNC_ST_LARGER;
		if (v & FF_BIT32(2))
			m |= FSYNC_ST_ATTR;
		if (((c->status & _FSYNC_ST_NMASK) & m) == 0)
			return 0;

		if (!gg->opts.show_dirs && st != FSYNC_ST_DEST && isdir(e1->attr))
			return 0;
	}

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
