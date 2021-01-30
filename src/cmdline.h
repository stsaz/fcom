/** fcom: command-line arguments
2021, Simon Zolin */

#include <FF/data/psarg.h>

static int arg_infile(ffparser_schem *p, void *obj, const ffstr *val)
{
	if (0 != in_add(g, val, 0))
		return FFPARS_ESYS;
	return 0;
}

/** Read file line by line and add filenames as input arguments. */
static int arg_flist(ffparser_schem *p, void *obj, const char *fn)
{
	int r = FFPARS_ESYS;
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
static int arg_finclude(ffparser_schem *p, void *obj, const ffstr *val)
{
	int rc = FFPARS_ESYS;
	struct cmdconf *c = obj;
	ffstr *dst, s = *val, wc;
	ffarr a = {};
	while (s.len != 0) {
		ffstr_nextval3(&s, &wc, ';');
		if (NULL == (dst = ffarr_pushgrowT(&a, 4, ffstr)))
			goto end;
		*dst = wc;
	}

	if (ffsz_eq(p->curarg->name, "include"))
		ffarr_set(&c->include_files, a.ptr, a.len);
	else
		ffarr_set(&c->exclude_files, a.ptr, a.len);
	ffarr_null(&a);
	rc = 0;

end:
	ffarr_free(&a);
	return rc;
}

// "DNS_SERVER_ADDR[;DNS_SERVER_ADDR]"
static int arg_servers(ffparser_schem *p, void *obj, const ffstr *val)
{
	int rc = FFPARS_ESYS;
	struct cmdconf *c = obj;
	ffstr *dst, s = *val, wc;
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

static int arg_member(ffparser_schem *p, void *obj, const ffstr *val)
{
	ffstr s = *val, wc;
	char **dst;
	while (s.len != 0) {
		ffstr_nextval3(&s, &wc, ';');
		if (NULL == (dst = ffarr_pushgrowT(&g->conf.members, 4, char*)))
			return FFPARS_ESYS;
		if (NULL == (*dst = ffsz_alcopystr(&wc)))
			return FFPARS_ESYS;
	}
	return 0;
}

#define CMDHELP_FN  "help.txt"

static int arg_help(ffparser_schem *p, void *obj)
{
	ffarr buf = {0};
	ssize_t n;
	char *fn = NULL;
	fffd f = FF_BADFD;
	int r = FFPARS_ESYS;

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
	r = FFPARS_ELAST;

done:
	ffmem_safefree(fn);
	FF_SAFECLOSE(f, FF_BADFD, fffile_close);
	ffarr_free(&buf);
	return r;
}

static int arg_date(ffparser_schem *p, void *obj, const ffstr *val)
{
	struct cmdconf *c = obj;
	ffdtm dt;
	if (val->len == fftime_fromstr(&dt, val->ptr, val->len, FFTIME_YMD))
	{}
	else if (val->len == fftime_fromstr(&dt, val->ptr, val->len, FFTIME_DATE_YMD))
	{}
	else
		return FFPARS_EBADVAL;
	fftime_join(&c->mtime, &dt, FFTIME_TZLOCAL);
	return 0;
}

static int arg_replace(ffparser_schem *p, void *obj, const ffstr *val)
{
	struct cmdconf *c = obj;
	ffstr sch, rpl;
	const char *div = ffs_split2by(val->ptr, val->len, '/', &sch, &rpl);
	if (div == NULL || sch.len == 0) {
		errlog("replace: invalid data: the correct pattern is SEARCH/REPLACE: %S"
			, val);
		return FFPARS_EBADVAL;
	}
	c->search = sch;
	c->replace = rpl;
	return 0;
}

// "width:height"
static int arg_crop(ffparser_schem *p, void *obj, const ffstr *val)
{
	struct cmdconf *c = obj;
	ffstr w, h;
	if (NULL == ffs_split2by(val->ptr, val->len, ':', &w, &h)
		|| (w.len == 0 && h.len == 0))
		return FFPARS_EBADVAL;

	uint ww, hh;
	if (w.len != 0) {
		if (!ffstr_toint(&w, &ww, FFS_INT32) || ww == 0)
			return FFPARS_EBADVAL;
		c->crop_width = ww;
	}
	if (h.len != 0) {
		if (!ffstr_toint(&h, &hh, FFS_INT32) || hh == 0)
			return FFPARS_EBADVAL;
		c->crop_height = hh;
	}
	return 0;
}

static int arg_comp_method(ffparser_schem *p, void *obj, const ffstr *val)
{
	struct cmdconf *c = obj;
	static const ffstr methods[] = {
		FFSTR_INIT("store"), FFSTR_INIT("deflate"), FFSTR_INIT("lzma"),
	};
	ffslice s;
	ffslice_set(&s, methods, FF_COUNT(methods));
	int i = ffslicestr_find(&s, val);
	if (i < 0)
		return FFPARS_EBADVAL;
	c->comp_method = i;
	return 0;
}

#define OFF(member)  FFPARS_DSTOFF(struct cmdconf, member)
static const ffpars_arg cmdline_args[] = {
	// INPUT
	{ "",	FFPARS_TSTR | FFPARS_FMULTI, FFPARS_DST(&arg_infile) },
	{ "flist",	FFPARS_TCHARPTR | FFPARS_FSTRZ | FFPARS_FCOPY | FFPARS_FNOTEMPTY | FFPARS_FMULTI, FFPARS_DST(&arg_flist) },
	{ "include",	FFPARS_TSTR | FFPARS_FCOPY | FFPARS_FNOTEMPTY, FFPARS_DST(&arg_finclude) },
	{ "exclude",	FFPARS_TSTR | FFPARS_FCOPY | FFPARS_FNOTEMPTY, FFPARS_DST(&arg_finclude) },
	{ "servers",	FFPARS_TSTR | FFPARS_FCOPY | FFPARS_FNOTEMPTY, FFPARS_DST(&arg_servers) },
	{ "recurse",	FFPARS_SETVAL('R') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(recurse) },

	// ARCHIVE READING
	{ "member",	FFPARS_TSTR | FFPARS_FNOTEMPTY | FFPARS_FMULTI, FFPARS_DST(&arg_member) },
	{ "show",	FFPARS_TBOOL8 | FFPARS_FALONE, OFF(show) },

	// ARCHIVE WRITING
	{ "deflate-level",	FFPARS_TINT8, OFF(deflate_level) },
	{ "compression-method",	FFPARS_TSTR | FFPARS_FNOTEMPTY, FFPARS_DST(&arg_comp_method) },

	// TEXT PROCESSING
	{ "replace",	FFPARS_TSTR | FFPARS_FCOPY | FFPARS_FNOTEMPTY, FFPARS_DST(&arg_replace) },

	// IMAGE PROCESSING
	{ "jpeg-quality",	FFPARS_TINT8, OFF(jpeg_quality) },
	{ "png-compression",	FFPARS_TINT8, OFF(png_comp) },
	{ "colors",	FFPARS_TINT8, OFF(colors) },
	{ "crop",	FFPARS_TSTR | FFPARS_FNOTEMPTY, FFPARS_DST(&arg_crop) },

	{ "password",	FFPARS_TCHARPTR | FFPARS_FSTRZ | FFPARS_FCOPY | FFPARS_FNOTEMPTY, OFF(passwd) },

	// OUTPUT
	{ "out",	FFPARS_SETVAL('o') | FFPARS_TCHARPTR | FFPARS_FCOPY | FFPARS_FSTRZ, OFF(out) },
	{ "outdir",	FFPARS_SETVAL('C') | FFPARS_TCHARPTR | FFPARS_FCOPY | FFPARS_FSTRZ, OFF(outdir) },
	{ "force",	FFPARS_SETVAL('f') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(force) },
	{ "test",	FFPARS_SETVAL('t') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(test) },
	{ "date",	FFPARS_TSTR, FFPARS_DST(&arg_date) },
	{ "date-as",	FFPARS_TCHARPTR | FFPARS_FSTRZ | FFPARS_FCOPY | FFPARS_FNOTEMPTY, OFF(date_as_fn) },
	{ "preserve-date",	FFPARS_TBOOL8 | FFPARS_FALONE, OFF(preserve_date) },

	// MISC
	{ "workers",	FFPARS_SETVAL('w') | FFPARS_TINT8, OFF(workers) },
	{ "skip-errors",	FFPARS_SETVAL('k') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(skip_errors) },
	{ "verbose",	FFPARS_SETVAL('v') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(verbose) },
	{ "debug",	FFPARS_SETVAL('D') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(debug) },
	{ "benchmark",	FFPARS_TBOOL8 | FFPARS_FALONE, OFF(benchmark) },
	{ "help",	FFPARS_TBOOL8 | FFPARS_FALONE, FFPARS_DST(arg_help) },
};
#undef OFF

static int cmdline(int argc, char **argv)
{
	ffparser_schem ps;
	ffpsarg_parser p;
	ffpars_ctx ctx = {0};
	int r = 0;
	int ret = 1;
	const char *arg;
	ffpsarg a;

	ffpsarg_init(&a, (void*)argv, argc);
	ffpars_setargs(&ctx, &g->conf, cmdline_args, FF_COUNT(cmdline_args));

	if (0 != ffpsarg_scheminit(&ps, &p, &ctx)) {
		errlog("cmd line parser", NULL);
		return 1;
	}

	ffpsarg_next(&a); //skip argv[0]

	arg = ffpsarg_next(&a);
	while (arg != NULL) {
		int n = 0;
		r = ffpsarg_parse(&p, arg, &n);
		if (n != 0)
			arg = ffpsarg_next(&a);

		r = ffpsarg_schemrun(&ps);

		if (r == FFPARS_ELAST)
			goto fail;

		if (ffpars_iserr(r))
			break;
	}

	if (!ffpars_iserr(r))
		r = ffpsarg_schemfin(&ps);

	if (ffpars_iserr(r)) {
		errlog("cmd line parser: near \"%S\": %s"
			, &p.val, (r == FFPARS_ESYS) ? fferr_strp(fferr_last()) : ffpars_errstr(r));
		goto fail;
	}

	ret = 0;

fail:
	ffpsarg_destroy(&a);
	ffpars_schemfree(&ps);
	ffpsarg_parseclose(&p);
	return ret;
}
