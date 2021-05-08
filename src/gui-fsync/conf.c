/** Settings for file synchronization GUI.
Copyright (c) 2019 Simon Zolin
*/

#include <gui-fsync/gsync.h>
#include <FF/path.h>
#include <FFOS/dir.h>


#define OFF(m)  FFPARS_DSTOFF(struct opts, m)
static const ffpars_arg opts_args[] = {
	{ "Compare: Time Diff",	FFPARS_TINT8, OFF(time_diff) },
	{ "Compare: Time Diff in Seconds",	FFPARS_TINT8, OFF(time_diff_sec) },

	{ "Filter Name",	FFPARS_TSTR | FFPARS_FRECOPY, OFF(filter_name) },
	{ "Filter",	FFPARS_TSTR | FFPARS_FRECOPY, OFF(filter) },
	{ "Show Modified by Date",	FFPARS_TINT | FFPARS_SETBIT(0), OFF(show_modmask) },
	{ "Show Modified by Size",	FFPARS_TINT | FFPARS_SETBIT(1), OFF(show_modmask) },
	{ "Show Modified by Attr",	FFPARS_TINT | FFPARS_SETBIT(2), OFF(show_modmask) },
	{ "Show Directories",	FFPARS_TINT8, OFF(show_dirs) },
	{ "Show Only Directories",	FFPARS_TINT8, OFF(show_dirs_only) },
	{ "Show \"Done\"",	FFPARS_TINT8, OFF(show_done) },
};
int col_width(ffparser_schem *p, void *obj, const int64 *val)
{
	*ffvec_pushT(&gg->opts.list_col_width, int) = *val;
	return 0;
}
static const ffpars_arg opts_args_conf[] = {
	{ "Source path",	FFPARS_TCHARPTR | FFPARS_FRECOPY | FFPARS_FSTRZ, OFF(srcfn) },
	{ "Target path",	FFPARS_TCHARPTR | FFPARS_FRECOPY | FFPARS_FSTRZ, OFF(dstfn) },
	{ "Compare: Time Diff",	FFPARS_TINT8, OFF(time_diff) },
	{ "Compare: Time Diff in Seconds",	FFPARS_TINT8, OFF(time_diff_sec) },

	{ "Filter Name",	FFPARS_TSTR | FFPARS_FRECOPY, OFF(filter_name) },
	{ "Filter",	FFPARS_TSTR | FFPARS_FRECOPY, OFF(filter) },
	{ "Show Equal",	FFPARS_TINT | FFPARS_SETBIT(FSYNC_ST_EQ), OFF(showmask) },
	{ "Show Modified",	FFPARS_TINT | FFPARS_SETBIT(FSYNC_ST_NEQ), OFF(showmask) },
	{ "Show Modified by Date",	FFPARS_TINT | FFPARS_SETBIT(0), OFF(show_modmask) },
	{ "Show Modified by Size",	FFPARS_TINT | FFPARS_SETBIT(1), OFF(show_modmask) },
	{ "Show Modified by Attr",	FFPARS_TINT | FFPARS_SETBIT(2), OFF(show_modmask) },
	{ "Show New",	FFPARS_TINT | FFPARS_SETBIT(FSYNC_ST_SRC), OFF(showmask) },
	{ "Show Deleted",	FFPARS_TINT | FFPARS_SETBIT(FSYNC_ST_DEST), OFF(showmask) },
	{ "Show Moved",	FFPARS_TINT | FFPARS_SETBIT(FSYNC_ST_MOVED), OFF(showmask) },
	{ "Show Directories",	FFPARS_TINT8, OFF(show_dirs) },
	{ "Show Only Directories",	FFPARS_TINT8, OFF(show_dirs_only) },
	{ "Show \"Done\"",	FFPARS_TINT8, OFF(show_done) },
	{ "Column Width",	FFPARS_TINT16 | FFPARS_FLIST, FFPARS_DST(col_width) },
};
#undef OFF

int opts_init(struct opts *c)
{
	c->srcfn = ffsz_alcopyz("");
	c->dstfn = ffsz_alcopyz("");
	c->showmask = -1;
	c->show_modmask = -1;
	c->show_dirs = 1;
	return 0;
}

void opts_destroy(struct opts *o)
{
	ffmem_safefree0(o->srcfn);
	ffmem_safefree0(o->dstfn);
	ffstr_free(&o->filter_name);
	ffstr_free(&o->filter);
	ffvec_free(&o->list_col_width);
}

/** Load options from file. */
int opts_load(struct opts *c)
{
	struct ffconf_loadfile conf = {0};
	if (NULL == (c->fn = core->env_expand(NULL, 0, "%APPDATA%\\fcom\\gsync.conf")))
		return -1;
	conf.fn = c->fn;
	conf.obj = c;
	conf.args = opts_args_conf;
	conf.nargs = FFCNT(opts_args_conf);
	dbglog(0, "reading %s", conf.fn);
	int r = ffconf_loadfile(&conf);
	if (r != 0 && !fferr_nofile(fferr_last())) {
		errlog("%s", conf.errstr);
	}
	return r;
}

/** Save options to file. */
void opts_save(struct opts *c)
{
	char buf[64];
	ffconfw conf;
	ffui_loaderw ldr = {0};
	const ffpars_arg *a;
	ffstr s;
	ffconf_winit(&conf, NULL, 0);
	FFARRS_FOREACH(opts_args_conf, a) {
		if (ffsz_eq(a->name, "Column Width"))
			continue;
		ffconf_write(&conf, a->name, ffsz_len(a->name), FFCONF_TKEY);
		ffpars_scheme_write(buf, a, c, &s);
		ffconf_write(&conf, s.ptr, s.len, FFCONF_TVAL);
	}

	ffconf_writez(&conf, "Column Width", FFCONF_TKEY);
	list_cols_width_write(&conf);

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
void opts_show(const struct opts *c, ffui_view *v)
{
	union ffpars_val u;
	ffui_viewitem it = {0};
	char buf[128];

	wsync_opts_show();

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
int opts_set(struct opts *c, ffui_view *v, uint sub)
{
	int i = ffui_view_focused(v);
	int r, ret = 0;
	FF_ASSERT(i >= 0);

	ffstr text;
	ffstr_setz(&text, v->text);
	r = ffpars_arg_process(&opts_args[i], &text, c, NULL);
	if (ffpars_iserr(r))
		return 0;

	if (ffsz_matchz(opts_args[i].name, "Filter")
		|| ffsz_match(opts_args[i].name, "Show ", 5))
		ret = 1;

	ffui_view_edit_set(v, i, sub);
	return ret;
}
