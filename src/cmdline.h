/** fcom: command-line arguments
2021, Simon Zolin */

#include <FF/data/cmdarg-scheme.h>

#define CMD_LAST 100

static int arg_infile(ffcmdarg_scheme *as, void *obj, const ffstr *val)
{
	if (0 != in_add(g, val, 0))
		return FFCMDARG_ERROR;
	return 0;
}

/** Read file line by line and add filenames as input arguments. */
static int arg_flist(ffcmdarg_scheme *as, void *obj, const char *fn)
{
	int r = FFCMDARG_ERROR;
	uint cnt = 0;
	ssize_t n;
	fffd f = FF_BADFD;
	ffarr buf = {0};
	ffstr line;
	const char *d, *end, *lf, *ln_end;

	dbglog(0, "opening file %s", fn);

	if (FF_BADFD == (f = fffile_open(fn, FFO_RDONLY | FFO_NOATIME)))
		goto done;
	if (NULL == ffarr_alloc(&buf, fffile_size(f)))
		goto done;
	if (0 > (n = fffile_read(f, buf.ptr, buf.cap)))
		goto done;
	d = buf.ptr;
	end = buf.ptr + n;
	while (d != end) {
		lf = ffs_find(d, end - d, '\n');
		d = ffs_skipof(d, lf - d, " \t", 2);
		ln_end = ffs_rskipof(d, lf - d, " \t\r", 3);
		ffstr_set(&line, d, ln_end - d);
		if (lf != end)
			d = lf + 1;
		else
			d = lf;
		if (line.len == 0)
			continue;
		if (0 != in_add(g, &line, 0))
			goto done;
		cnt++;
	}

	dbglog(0, "added %u filenames from %s", cnt, fn);

	r = 0;

done:
	FF_SAFECLOSE(f, FF_BADFD, fffile_close);
	ffarr_free(&buf);
	return r;
}

// "WILDCARD[;WILDCARD]"
static int arg_finclude(ffcmdarg_scheme *as, void *obj, const ffstr *val)
{
	int rc = FFCMDARG_ERROR;
	struct cmdconf *c = obj;
	ffstr *dst, s, wc;
	char *sz = ffsz_dupstr(val);
	ffstr_setz(&s, sz);
	ffarr a = {};
	while (s.len != 0) {
		ffstr_nextval3(&s, &wc, ';');
		if (NULL == (dst = ffarr_pushgrowT(&a, 4, ffstr)))
			goto end;
		*dst = wc;
	}

	if (ffsz_eq(as->arg->long_name, "include")) {
		ffarr_set(&c->include_files, a.ptr, a.len);
		c->include_files_data = sz;
	} else {
		ffarr_set(&c->exclude_files, a.ptr, a.len);
		c->exclude_files_data = sz;
	}
	sz = NULL;
	ffarr_null(&a);
	rc = 0;

end:
	ffmem_free(sz);
	ffarr_free(&a);
	return rc;
}

// "DNS_SERVER_ADDR[;DNS_SERVER_ADDR]"
static int arg_servers(ffcmdarg_scheme *as, void *obj, const ffstr *val)
{
	int rc = FFCMDARG_ERROR;
	struct cmdconf *c = obj;
	ffstr *dst, s, wc;
	c->servers_data = ffsz_dupstr(val);
	ffstr_setz(&s, c->servers_data);
	ffarr a = {};
	while (s.len != 0) {
		ffstr_nextval3(&s, &wc, ';');
		if (NULL == (dst = ffarr_pushgrowT(&a, 4, ffstr)))
			goto end;
		*dst = wc;
	}

	ffarr_set(&c->servers, a.ptr, a.len);
	ffarr_null(&a);
	rc = 0;

end:
	ffarr_free(&a);
	return rc;
}

static int arg_member(ffcmdarg_scheme *as, void *obj, const ffstr *val)
{
	ffstr s = *val, wc;
	char **dst;
	while (s.len != 0) {
		ffstr_nextval3(&s, &wc, ';');
		if (NULL == (dst = ffarr_pushgrowT(&g->conf.members, 4, char*)))
			return FFCMDARG_ERROR;
		if (NULL == (*dst = ffsz_alcopystr(&wc)))
			return FFCMDARG_ERROR;
	}
	return 0;
}

#define CMDHELP_FN  "help.txt"

static int arg_help(ffcmdarg_scheme *as, void *obj)
{
	ffarr buf = {0};
	ssize_t n;
	char *fn = NULL;
	fffd f = FF_BADFD;
	int r = FFCMDARG_ERROR;

	if (NULL == (fn = core->getpath(FFSTR(CMDHELP_FN))))
		goto done;

	if (FF_BADFD == (f = fffile_open(fn, FFO_RDONLY | FFO_NOATIME)))
		goto done;

	if (NULL == ffarr_alloc(&buf, 64 + fffile_size(f)))
		goto done;

	ffstr_catfmt(&buf, "fcom v%s\n", FCOM_VER_STR);

	n = fffile_read(f, ffarr_end(&buf), ffarr_unused(&buf));
	if (n <= 0)
		goto done;
	buf.len += n;

	fffile_write(ffstdout, buf.ptr, buf.len);
	r = CMD_LAST;

done:
	ffmem_safefree(fn);
	FF_SAFECLOSE(f, FF_BADFD, fffile_close);
	ffarr_free(&buf);
	return r;
}

static int arg_date(ffcmdarg_scheme *as, void *obj, const ffstr *val)
{
	struct cmdconf *c = obj;
	ffdtm dt;
	if (val->len == fftime_fromstr(&dt, val->ptr, val->len, FFTIME_YMD))
	{}
	else if (val->len == fftime_fromstr(&dt, val->ptr, val->len, FFTIME_DATE_YMD))
	{}
	else
		return FFCMDARG_ERROR;
	fftime_join(&c->mtime, &dt, FFTIME_TZLOCAL);
	return 0;
}

static int arg_replace(ffcmdarg_scheme *as, void *obj, const ffstr *val)
{
	struct cmdconf *c = obj;
	ffstr s, sch, rpl;
	c->sr_data = ffsz_dupstr(val);
	ffstr_setz(&s, c->sr_data);
	int r = ffstr_splitby(&s, '/', &sch, &rpl);
	if (r < 0 || sch.len == 0) {
		errlog("replace: invalid data: the correct pattern is SEARCH/REPLACE: %S"
			, val);
		return FFCMDARG_ERROR;
	}
	c->search = sch;
	c->replace = rpl;
	return 0;
}

// "width:height"
static int arg_crop(ffcmdarg_scheme *as, void *obj, const ffstr *val)
{
	struct cmdconf *c = obj;
	ffstr w, h;
	if (NULL == ffs_split2by(val->ptr, val->len, ':', &w, &h)
		|| (w.len == 0 && h.len == 0))
		return FFCMDARG_ERROR;

	uint ww, hh;
	if (w.len != 0) {
		if (!ffstr_toint(&w, &ww, FFS_INT32) || ww == 0)
			return FFCMDARG_ERROR;
		c->crop_width = ww;
	}
	if (h.len != 0) {
		if (!ffstr_toint(&h, &hh, FFS_INT32) || hh == 0)
			return FFCMDARG_ERROR;
		c->crop_height = hh;
	}
	return 0;
}

static int arg_comp_method(ffcmdarg_scheme *as, void *obj, const ffstr *val)
{
	struct cmdconf *c = obj;
	static const ffstr methods[] = { // sync with enum FCOM_COMP_METH
		FFSTR_INITZ("store"), FFSTR_INITZ("deflate"), FFSTR_INITZ("lzma"), FFSTR_INITZ("zstd"),
	};
	ffslice s;
	ffslice_set(&s, methods, FF_COUNT(methods));
	int i = ffslicestr_find(&s, val);
	if (i < 0)
		return FFCMDARG_ERROR;
	c->comp_method = i;
	return 0;
}

#define O(member)  FF_OFF(struct cmdconf, member)
#define F(f)  (ffsize)f
#define TSTR  FFCMDARG_TSTR | FFCMDARG_FNOTEMPTY
#define TSTRZ  FFCMDARG_TSTRZ | FFCMDARG_FNOTEMPTY
#define TSWITCH  FFCMDARG_TSWITCH
#define TINT8  FFCMDARG_TINT8
static const ffcmdarg_arg cmdline_args[] = {
	// INPUT
	{ 0, "",	TSTR,	F(arg_infile) },
	{ 0, "flist",	TSTRZ | FFCMDARG_FMULTI,	F(arg_flist) },
	{ 0, "include",	TSTR,	F(arg_finclude) },
	{ 0, "exclude",	TSTR,	F(arg_finclude) },
	{ 0, "servers",	TSTR,	F(arg_servers) },
	{ 'R', "recurse",	TSWITCH,	O(recurse) },
	{ 0, "delete-source",	TSWITCH,	O(del_source) },

	// ARCHIVE READING
	{ 0, "member",	TSTR | FFCMDARG_FMULTI,	F(arg_member) },
	{ 0, "show",	TSWITCH,	O(show) },

	// ARCHIVE WRITING
	{ 0, "deflate-level",	TINT8,	O(deflate_level) },
	{ 0, "zstd-level",	TINT8 | FFCMDARG_FSIGN,	O(zstd_level) },
	{ 0, "zstd-workers",	TINT8,	O(zstd_workers) },
	{ 0, "compression-method",	TSTR,	F(arg_comp_method) },

	// TEXT PROCESSING
	{ 0, "replace",	TSTR,	F(arg_replace) },

	// IMAGE PROCESSING
	{ 0, "jpeg-quality",	TINT8,	O(jpeg_quality) },
	{ 0, "png-compression",	TINT8,	O(png_comp) },
	{ 0, "colors",	TINT8,	O(colors) },
	{ 0, "crop",	TSTR,	F(arg_crop) },

	{ 0, "password",	TSTRZ,	O(passwd) },

	// OUTPUT
	{ 'o', "out",	TSTRZ,	O(out) },
	{ 'C', "outdir",	TSTRZ,	O(outdir) },
	{ 'f', "force",	TSWITCH,	O(force) },
	{ 't', "test",	TSWITCH,	O(test) },
	{ 0, "date",	TSTR,	F(arg_date) },
	{ 0, "date-as",	TSTRZ,	O(date_as_fn) },
	{ 0, "preserve-date",	TSWITCH,	O(preserve_date) },

	// MISC
	{ 'w', "workers",	TINT8,	O(workers) },
	{ 'k', "skip-errors",	TSWITCH,	O(skip_errors) },
	{ 'v', "verbose",	TSWITCH,	O(verbose) },
	{ 'D', "debug",	TSWITCH,	O(debug) },
	{ 0, "benchmark",	TSWITCH,	O(benchmark) },
	{ 0, "help",	TSWITCH,	F(arg_help) },
};
#undef O
#undef F

static int cmdline(int argc, char **argv)
{
	ffstr errmsg = {};
	int r = ffcmdarg_parse_object(cmdline_args, &g->conf, (const char**)argv, argc, 0, &errmsg);
	if (r == -CMD_LAST)
	{}
	else if (r != 0)
		errlog("cmd line: %S", &errmsg);
	ffstr_free(&errmsg);
	return r;
}
