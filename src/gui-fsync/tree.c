/** Show file tree.
Copyright (c) 2019 Simon Zolin */

#include <gui-fsync/gsync.h>
#include <FF/time.h>


static void wtree_action(ffui_wnd *wnd, int id);
static void tree_showsub(fsync_dir *d);


enum {
	A_TREE_DISPINFO = 100,
};

void tree_preinit()
{
	gg->wtree.vlist.dispinfo_id = A_TREE_DISPINFO;
}

void tree_init()
{
	gg->wtree.wnd.on_action = &wtree_action;
	gg->wtree.wnd.hide_on_close = 1;
}

enum {
	LCOL_NAME,
	LCOL_SIZE,
	LCOL_DATE,
};

static void show_listitem()
{
	LVITEM *it = gg->wtree.vlist.dispinfo_item;
	char buf[255];
	ffstr d = {};

	if (!(it->mask & LVIF_TEXT))
		return;

	uint i = it->iItem;
	if (gg->ntree_dirs != 1) {

		if (it->iItem == 0) {
			switch (it->iSubItem) {
			case LCOL_NAME:
				ffstr_setz(&d, "<UP>");
				ffui_view_dispinfo_settext(it, d.ptr, d.len);
				break;
			}
			return;
		}

		i = it->iItem - 1;
	}

	const struct fsync_file *f = fsync->get(FSYNC_GETFILE, gg->tree_dirs[gg->ntree_dirs - 1], i);
	FF_ASSERT(f != NULL);

	switch (it->iSubItem) {
	case LCOL_NAME:
		ffstr_setz(&d, f->name);
		break;

	case LCOL_SIZE: {
		if (isdir(f->attr)) {
			ffstr_setz(&d, "<DIR>");
			break;
		}
		uint n = ffs_fromint(f->size, buf, sizeof(buf), 0);
		ffstr_set(&d, buf, n);
		break;
	}

	case LCOL_DATE: {
		ffdtm dt;
		fftime_split(&dt, &f->mtime, FFTIME_TZLOCAL);
		uint n = fftime_tostr(&dt, buf, sizeof(buf), FFTIME_DATE_MDY0 | FFTIME_HMS);
		ffstr_set(&d, buf, n);
		break;
	}
	}

	if (d.len != 0)
		ffui_view_dispinfo_settext(it, d.ptr, d.len);
}

static void wtree_action(ffui_wnd *wnd, int id)
{
	switch (id) {
	case A_TREE_ENTER: {
		int i = ffui_view_focused(&gg->wtree.vlist);

		if (gg->ntree_dirs != 1) {
			if (i == 0) {
				gg->ntree_dirs--;
				fsync_dir *d = gg->tree_dirs[--gg->ntree_dirs];
				tree_showsub(d);
				break;
			}
			i--;
		}

		const struct fsync_file *f = fsync->get(FSYNC_GETFILE, gg->tree_dirs[gg->ntree_dirs - 1], i);
		FF_ASSERT(f != NULL);
		fsync_dir *d = fsync->get(FSYNC_GETSUBDIR, f);
		if (d == NULL)
			break;
		tree_showsub(d);
		break;
	}
	case A_TREE_DISPINFO:
		show_listitem();
		break;
	}
}

static void tree_showsub(fsync_dir *d)
{
	FF_ASSERT(gg->ntree_dirs != FFCNT(gg->tree_dirs));
	gg->tree_dirs[gg->ntree_dirs++] = d;
	uint n = (size_t)fsync->get(FSYNC_COUNT, d);
	dbglog(0, "FSYNC_COUNT=%u", n);
	if (gg->ntree_dirs != 1)
		n++;
	ffui_settextz(&gg->wtree.eaddr, fsync->get(FSYNC_DIRPATH, d));
	ffui_view_setcount(&gg->wtree.vlist, n);
	ffui_view_redraw(&gg->wtree.vlist, 0, 100);
}

void tree_show(fsync_dir *d)
{
	gg->ntree_dirs = 0;
	tree_showsub(d);
}
