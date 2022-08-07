/** Settings for file synchronization GUI.
Copyright (c) 2019 Simon Zolin
*/

#include <util/path.h>
#include <FFOS/dir.h>


int col_width(ffconf_scheme *cs, void *obj, int64 val)
{
	*ffvec_pushT(&gg->opts.list_col_width, int) = val;
	return 0;
}

int wsync_pos(ffconf_scheme *cs, void *obj, int64 val)
{
	*ffvec_pushT(&gg->wndpos, int) = val;
	if (gg->wndpos.len == 4) {
		const int *p = gg->wndpos.ptr;
		ffui_setpos(&gg->wsync.wsync, p[0], p[1], p[2], p[3], 0);
		ffvec_free(&gg->wndpos);
	}
	return 0;
}

#define OFF(m)  FF_OFF(struct opts, m)
static const ffconf_arg opts_args_conf[] = {
	{ "Source path",	FFCONF_TSTRZ, OFF(srcfn) },
	{ "Target path",	FFCONF_TSTRZ, OFF(dstfn) },

	{ "Filter Name",	FFCONF_TSTR, OFF(filter_name) },
	{ "Filter",	FFCONF_TSTR, OFF(filter) },
	{ "file_display_mask",	FFCONF_TINT32, OFF(showmask) },
	// { "Show Modified by Date",	FFCONF_TINT | FFPARS_SETBIT(0), OFF(show_modmask) },
	// { "Show Modified by Size",	FFCONF_TINT | FFPARS_SETBIT(1), OFF(show_modmask) },
	// { "Show Modified by Attr",	FFCONF_TINT | FFPARS_SETBIT(2), OFF(show_modmask) },
	{ "Show Only Directories",	FFCONF_TINT8, OFF(show_dirs_only) },
	{ "Column Width",	FFCONF_TINT16 | FFCONF_FLIST, (ffsize)(col_width) },
	{ "wsync_pos",	FFCONF_TINT16 | FFCONF_FLIST, (ffsize)wsync_pos },
	{}
};
#undef OFF

int opts_init(struct opts *c)
{
	c->srcfn = ffsz_alcopyz("");
	c->dstfn = ffsz_alcopyz("");
	c->showmask = (1<<FSYNC_ST_SRC) | (1<<FSYNC_ST_DEST) | (1<<FSYNC_ST_MOVED) | (1<<FSYNC_ST_NEQ)
		| SHOWMASK_DIRS | SHOWMASK_OLDER | SHOWMASK_NEWER;
	c->show_modmask = -1;
	c->show_done = 1;
	return 0;
}

void opts_destroy(struct opts *o)
{
	ffmem_safefree0(o->srcfn);
	ffmem_safefree0(o->dstfn);
	ffstr_free(&o->filter_name);
	ffstr_free(&o->filter);
	ffstr_free(&o->exclude);
	ffstr_free(&o->include);
	ffvec_free(&o->list_col_width);
}

/** Load options from file. */
int opts_load(struct opts *c)
{
	if (NULL == (c->fn = core->env_expand(NULL, 0, "%APPDATA%\\fcom\\gsync.conf")))
		return -1;
	dbglog(0, "reading %s", c->fn);

	ffstr errmsg = {};
	int r = ffconf_parse_file(opts_args_conf, c, c->fn, 0, &errmsg);
	if (r != 0 && !fferr_nofile(fferr_last())) {
		errlog("%S", &errmsg);
	}
	ffstr_free(&errmsg);
	return r;
}

/** Save options to file. */
void opts_save(struct opts *c)
{
	ffconfw conf = {};
	ffconfw_init(&conf, 0);

	const ffconf_arg *a;
	FFARRS_FOREACH(opts_args_conf, a) {
		if (a->name == NULL)
			break;

		ffconfw_addkeyz(&conf, a->name);

		if (ffsz_eq(a->name, "Column Width")) {
			list_cols_width_write(&conf);
			continue;
		} else if (ffsz_eq(a->name, "wsync_pos")) {
			wsync_pos_write(&conf);
			continue;
		}

		void *ptr = FF_PTR(c, a->dst);
		int i;

		switch (a->flags & 0x0f) {
		case FFCONF_TSTRZ:
			ffconfw_addstrz(&conf, *(char**)ptr);
			break;
		case FFCONF_TSTR:
			ffconfw_addstr(&conf, ptr);
			break;
		case _FFCONF_TINT:
			if (a->flags & FFCONF_F32BIT)
				i = *(int*)ptr;
			else if (a->flags & FFCONF_F8BIT)
				i = *(ffbyte*)ptr;
			ffconfw_addint(&conf, i);
			break;
		default:
			dbglog(0, "name:%s flags:%xu", a->name, a->flags);
			FF_ASSERT(0);
		}
	}

	ffconfw_fin(&conf);
	ffstr d;
	ffconfw_output(&conf, &d);
	if (0 != fffile_writewhole(c->fn, d.ptr, d.len, 0) && fferr_nofile(fferr_last())) {
		if (0 != ffdir_make_path(c->fn, 0) && fferr_last() != EEXIST) {
			syserrlog("Can't create directory for the file: %s", c->fn);
			goto done;
		}
		if (0 != fffile_writewhole(c->fn, d.ptr, d.len, 0)) {
			syserrlog("Can't write configuration file: %s", c->fn);
			goto done;
		}
	}

	dbglog(0, "saved settings to %s", c->fn);

done:
	ffconfw_close(&conf);
}

/* struct opts -> GUI */
void opts_show(const struct opts *c, ffui_view *v)
{
}

/* GUI -> struct opts */
int opts_set(struct opts *c, ffui_view *v, uint sub)
{
	/*int i = ffui_view_focused(v);
	int r, ret = 0;
	FF_ASSERT(i >= 0);

	ffstr text;
	ffstr_setz(&text, v->text);
	r = ffconf_arg_process(&opts_args[i], &text, c, NULL);
	if (ffpars_iserr(r))
		return 0;

	if (ffsz_matchz(opts_args[i].name, "Filter")
		|| ffsz_match(opts_args[i].name, "Show ", 5))
		ret = 1;

	ffui_view_edit_set(v, i, sub);
	return ret;*/
	return 0;
}
