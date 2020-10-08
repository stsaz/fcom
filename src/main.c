/** fcom startup.
Copyright (c) 2017 Simon Zolin */

#include <fcom.h>

#include <FF/data/psarg.h>
#include <FF/sys/dir.h>
#include <FF/time.h>
#include <FF/path.h>
#include <FFOS/process.h>
#include <FFOS/file.h>
#include <FFOS/thread.h>
#include <FFOS/sig.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, "main", fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog("main", fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog("main", fmt, __VA_ARGS__)

struct cmdconf {
	char *out;
	char *outdir;
	char *date_as_fn;
	byte force;
	byte test;
	fftime mtime;

	ffarr members; //char*[]
	ffarr2 include_files; //ffstr[]
	ffarr2 exclude_files; //ffstr[]

	ffarr2 servers; //ffstr[]

	ffstr search;
	ffstr replace;

	uint crop_width;
	uint crop_height;

	byte recurse;
	byte show;
	byte skip_errors;
	byte deflate_level;
	byte jpeg_quality;
	byte png_comp;
	byte preserve_date;
	byte colors;

	byte debug;
	byte verbose;
	byte benchmark;
	byte workers;
};

struct job {
	struct cmdconf conf;
	fcom_conf gconf;
	ffsignal sigs_task;
	fftask tsk;
	uint retcode;
	uint stdout_busy :1;

	ffchain in_list;
	ffchain_item *in_next;

	ffdl core_dl;
	core_create_t core_create;
	core_free_t core_free;
};

struct in_ent {
	ffchain_item sib;
	char fn[0];
};

static struct job *g;
static const fcom_core *core;
static const fcom_command *com;

enum {
	LOG_BUF = 4096,
};

#define CMDHELP_FN  "help.txt"

static int std_log(uint flags, const char *fmt, va_list va);
static int cmdline(int argc, char **argv);
static int in_add(struct job *c, const ffstr *s, uint flags);
static const char* in_next(struct job *c, uint flags);

static void mon_onsig(fcom_cmd *cmd, uint sig);
static const struct fcom_cmd_mon mon = { &mon_onsig };


static const char *const log_levs[] = {
	"error", "warning", "info", "verbose", "debug",
};

/*
TIME fcom: \[LEV\] MSG [: SYSERR]
*/
static int std_log(uint flags, const char *fmt, va_list va)
{
	ffdtm dt;
	fftime t;
	char stime[32], buf[LOG_BUF], *s, *end;
	ffstr tm;
	uint lev = flags & _FCOM_LEVMASK;
	int e;

	s = buf;
	end = buf + FFCNT(buf) - FFSLEN("\n");

	if (lev != FCOM_LOGVERB && !(flags & FCOM_LOGNOPFX)) {

		if (flags & FCOM_LOGSYS)
			e = fferr_last();

		fftime_now(&t);
		fftime_split(&dt, &t, FFTIME_TZLOCAL);
		tm.len = fftime_tostr(&dt, stime, sizeof(stime), FFTIME_HMS_MSEC);
		tm.ptr = stime;

		s += ffs_fmt(s, end, "%S fcom: #%xU [%s] "
			, &tm, (int64)ffthd_curid(), log_levs[lev]);
	}

	s += ffs_fmtv(s, end, fmt, va);

	if (flags & FCOM_LOGSYS)
		s += ffs_fmt(s, end, ": %E", e);

	s = ffs_copyc(s, end, '\n');

	fffd fd = (lev <= FCOM_LOGWARN || g->stdout_busy) ? ffstderr : ffstdout;
	fffile_write(fd, buf, s - buf);
	return 0;
}

static int arg_infile(ffparser_schem *p, void *obj, const ffstr *val);
static int arg_flist(ffparser_schem *p, void *obj, const char *fn);
static int arg_finclude(ffparser_schem *p, void *obj, const ffstr *val);
static int arg_servers(ffparser_schem *p, void *obj, const ffstr *val);
static int arg_member(ffparser_schem *p, void *obj, const char *fn);
static int arg_help(ffparser_schem *p, void *obj);
static int arg_date(ffparser_schem *p, void *obj, const ffstr *val);
static int arg_replace(ffparser_schem *p, void *obj, const ffstr *val);
static int arg_crop(ffparser_schem *p, void *obj, const ffstr *val);


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
	{ "member",	FFPARS_TCHARPTR | FFPARS_FSTRZ | FFPARS_FCOPY | FFPARS_FNOTEMPTY | FFPARS_FMULTI, FFPARS_DST(&arg_member) },
	{ "show",	FFPARS_TBOOL8 | FFPARS_FALONE, OFF(show) },

	// ARCHIVE WRITING
	{ "deflate-level",	FFPARS_TINT8, OFF(deflate_level) },

	// TEXT PROCESSING
	{ "replace",	FFPARS_TSTR | FFPARS_FCOPY | FFPARS_FNOTEMPTY, FFPARS_DST(&arg_replace) },

	// IMAGE PROCESSING
	{ "jpeg-quality",	FFPARS_TINT8, OFF(jpeg_quality) },
	{ "png-compression",	FFPARS_TINT8, OFF(png_comp) },
	{ "colors",	FFPARS_TINT8, OFF(colors) },
	{ "crop",	FFPARS_TSTR | FFPARS_FNOTEMPTY, FFPARS_DST(&arg_crop) },

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


enum IN_ADD {
	ADD_NO_WCARD = 1,
};

#ifdef FF_WIN
/** Add filenames expanded by wildcard. */
static int wcard_open(struct job *c, const ffstr *s)
{
	int r = -1;
	ffdirexp de;
	char *wc = NULL;
	const char *fn;

	if (ffstr_matchz(s, "\\\\?\\"))
		return 0;

	if (ffarr_end(s) == ffs_findof(s->ptr, s->len, "*?", 2))
		return 0;

	if (NULL == (wc = ffsz_alcopy(s->ptr, s->len)))
		return -1;

	if (0 != ffdir_expopen(&de, wc, 0))
		goto done;

	while (NULL != (fn = ffdir_expread(&de))) {
		ffstr sfn;
		ffstr_setz(&sfn, fn);
		if (0 != in_add(c, &sfn, ADD_NO_WCARD))
			goto done;
	}
	r = 1;

done:
	ffdir_expclose(&de);
	ffmem_safefree(wc);
	return r;
}
#else
static int wcard_open(struct job *c, const ffstr *s)
{
	return 0;
}
#endif //FF_WIN

/**
@flags: enum IN_ADD */
static int in_add(struct job *c, const ffstr *s, uint flags)
{
	int r;
	if (!(flags & ADD_NO_WCARD) && 0 != (r = wcard_open(c, s)))
		return (r > 0) ? 0 : -1;

	struct in_ent *e;
	if (NULL == (e = ffmem_calloc(1, sizeof(struct in_ent) + s->len + 1)))
		return -1;
	ffchain_add(&c->in_list, &e->sib);
	if (c->in_next == ffchain_sentl(&c->in_list))
		c->in_next = ffchain_first(&c->in_list);
	ffsz_fcopy(e->fn, s->ptr, s->len);
	return 0;
}

static const char* in_next(struct job *c, uint flags)
{
	if (c->in_next == ffchain_sentl(&c->in_list))
		return NULL;

	struct in_ent *e = FF_GETPTR(struct in_ent, sib, c->in_next);
	c->in_next = c->in_next->next;
	return e->fn;
}

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

static int arg_member(ffparser_schem *p, void *obj, const char *fn)
{
	char **ps;
	if (NULL == (ps = ffarr_pushgrowT(&g->conf.members, 8, char*)))
		return FFPARS_ESYS;
	if (NULL == (*ps = ffsz_alcopyz(fn)))
		return FFPARS_ESYS;
	return 0;
}

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
	ffpars_setargs(&ctx, &g->conf, cmdline_args, FFCNT(cmdline_args));

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

static const int sigs[] = { SIGINT };
static const int sigs_block[] = { SIGINT };

static void cmds_free(void)
{
	ffchain_item *it;
	FFCHAIN_FOR(&g->in_list, it) {
		struct in_ent *ent = FF_GETPTR(struct in_ent, sib, it);
		it = it->next;
		ffmem_free(ent);
	}

	ffmem_safefree(g->conf.out);
	ffmem_safefree(g->conf.outdir);
	ffmem_safefree(g->conf.date_as_fn);
	ffslice_free(&g->conf.include_files);
	ffslice_free(&g->conf.exclude_files);
	ffslice_free(&g->conf.servers);
	ffstr_free(&g->conf.search);

	char **ps;
	FFARR_WALKT(&g->conf.members, ps, char*)
		ffmem_safefree(*ps);
	ffarr_free(&g->conf.members);

	ffmem_safefree(g);
}


static void mon_onsig(fcom_cmd *cmd, uint sig)
{
	g->retcode = 0;
	core->cmd(FCOM_STOP);
}

static void cmd_add(void *param)
{
	struct job *c = param;
	const char *op;
	fcom_cmd *m = NULL;

	if (NULL == (op = in_next(c, 0)))
		goto done;
	fcom_cmd cmd = {0};
	cmd.name = op;
	ffarr_set(&cmd.members, c->conf.members.ptr, c->conf.members.len);
	cmd.include_files = c->conf.include_files;
	cmd.exclude_files = c->conf.exclude_files;
	cmd.servers = c->conf.servers;
	cmd.search = c->conf.search;
	cmd.replace = c->conf.replace;
	cmd.recurse = c->conf.recurse;

	cmd.crop.width = c->conf.crop_width;
	cmd.crop.height = c->conf.crop_height;

	cmd.output.fn = c->conf.out;
	g->stdout_busy = cmd.out_std = (c->conf.out != NULL && ffsz_eq(c->conf.out, "@stdout"));
	cmd.outdir = c->conf.outdir;
	cmd.out_overwrite = c->conf.force;
	cmd.skip_err = c->conf.skip_errors;
	cmd.deflate_level = c->conf.deflate_level;
	cmd.jpeg_quality = c->conf.jpeg_quality;
	cmd.png_comp = c->conf.png_comp;
	cmd.pic_colors = c->conf.colors;
	cmd.read_only = c->conf.test;
	cmd.benchmark = c->conf.benchmark;
	cmd.mtime = c->conf.mtime;
	cmd.date_as_fn = c->conf.date_as_fn;
	cmd.show = c->conf.show;
	cmd.out_preserve_date = c->conf.preserve_date;
	com = core->iface("core.com");
	if (NULL == (m = com->create(&cmd)))
		goto done;

	if (0 != com->fcom_cmd_monitor(m, &mon))
		goto done;

	// set arguments
	ffchain_item *li;
	FFCHAIN_WALK(&c->in_list, li) {
		if (li == ffchain_first(&c->in_list))
			continue;
		struct in_ent *ent = FF_GETPTR(struct in_ent, sib, li);
		ffstr s;
		ffstr_setz(&s, ent->fn);
		if (0 != com->arg_add(m, &s, 0))
			goto done;
	}

	com->run(m);
	return;

done:
	FF_SAFECLOSE(m, NULL, com->close);
	core->cmd(FCOM_STOP);
}

static void onsig(void *udata)
{
	int sig;

	if (-1 == (sig = ffsig_read(&g->sigs_task, NULL)))
		return;

	core->cmd(FCOM_STOP);
}

/** Load core.so and import its functions. */
static int loadcore(char *argv0)
{
	int rc = -1;
	char buf[FF_MAXPATH];
	const char *path;
	ffdl dl = NULL;
	ffarr a = {0};

	if (NULL == (path = ffps_filename(buf, sizeof(buf), argv0)))
		goto end;
	if (0 == ffstr_catfmt(&a, "%s/../mod/core.%s%Z", path, FFDL_EXT))
		goto end;
	a.len = ffpath_norm(a.ptr, a.cap, a.ptr, a.len - 1, 0);
	a.ptr[a.len] = '\0';

	if (NULL == (dl = ffdl_open(a.ptr, 0))) {
		fffile_fmt(ffstderr, NULL, "can't load %s: %s\n", a.ptr, ffdl_errstr());
		goto end;
	}

	g->core_create = (void*)ffdl_addr(dl, "core_create");
	g->core_free = (void*)ffdl_addr(dl, "core_free");
	if (g->core_create == NULL || g->core_free == NULL) {
		fffile_fmt(ffstderr, NULL, "can't resolve functions from %s: %s\n"
			, a.ptr, ffdl_errstr());
		goto end;
	}

	g->core_dl = dl;
	dl = NULL;
	rc = 0;

end:
	FF_SAFECLOSE(dl, NULL, ffdl_close);
	ffarr_free(&a);
	return rc;
}

int main(int argc, char **argv, char **env)
{
	int r = 1;

	ffmem_init();
	(void)sigs_block;
#ifndef _DEBUG
	ffsig_mask(SIG_BLOCK, sigs_block, FFCNT(sigs_block));
#endif

	if (NULL == (g = ffmem_new(struct job)))
		return 1;
	g->retcode = 1;
	ffchain_init(&g->in_list);
	g->in_next = ffchain_sentl(&g->in_list);
	g->conf.deflate_level = 255;
	g->conf.jpeg_quality = 255;
	g->conf.png_comp = 255;
	g->conf.colors = 255;

	if (0 != loadcore(argv[0]))
		goto done;

	if (NULL == (core = g->core_create(&std_log, argv, env)))
		goto done;

	if (argc == 1) {
		arg_help(NULL, NULL);
		goto done;
	}

	if (0 != cmdline(argc, argv))
		goto done;

	g->gconf.loglev = FCOM_LOGINFO;
	if (g->conf.debug)
		g->gconf.loglev = FCOM_LOGDBG;
	else if (g->conf.verbose)
		g->gconf.loglev = FCOM_LOGVERB;

	if (g->conf.workers != 0)
		g->gconf.workers = g->conf.workers;

	if (0 != core->fcom_core_setconf(&g->gconf))
		goto done;

	if (0 != core->fcom_core_readconf(FCOM_CONF_FN))
		goto done;

	g->sigs_task.udata = g;
	if (0 != ffsig_ctl(&g->sigs_task, core->kq, sigs, FFCNT(sigs), &onsig)) {
		syserrlog("%s", "ffsig_ctl()");
		goto done;
	}

	g->tsk.handler = &cmd_add;
	g->tsk.param = g;
	core->task(FCOM_TASK_ADD, &g->tsk);

	core->cmd(FCOM_RUN);

	r = g->retcode;

done:
	if (core != NULL) {
		ffsig_ctl(&g->sigs_task, core->kq, sigs, FFCNT(sigs), NULL);
		g->core_free();
	}
	FF_SAFECLOSE(g->core_dl, NULL, ffdl_close);
	cmds_free();
	return r;
}
