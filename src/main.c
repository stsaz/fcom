/** fcom startup.
Copyright (c) 2017 Simon Zolin */

#include <fcom.h>

#include <FF/data/psarg.h>
#include <FF/sys/dir.h>
#include <FF/time.h>
#include <FFOS/file.h>
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

	byte recurse;
	byte show;
	byte skip_errors;
	byte deflate_level;
	byte jpeg_quality;
	byte png_comp;
	byte preserve_date;

	byte debug;
	byte verbose;
	byte benchmark;
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

		s += ffs_fmt(s, end, "%S fcom: [%s] ", &tm, log_levs[lev]);
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
static int arg_member(ffparser_schem *p, void *obj, const char *fn);
static int arg_help(ffparser_schem *p, void *obj);
static int arg_date(ffparser_schem *p, void *obj, const ffstr *val);


#define OFF(member)  FFPARS_DSTOFF(struct cmdconf, member)
static const ffpars_arg cmdline_args[] = {
	{ "",	FFPARS_TSTR | FFPARS_FNOTEMPTY | FFPARS_FMULTI, FFPARS_DST(&arg_infile) },
	{ "flist",	FFPARS_TCHARPTR | FFPARS_FSTRZ | FFPARS_FCOPY | FFPARS_FNOTEMPTY | FFPARS_FMULTI, FFPARS_DST(&arg_flist) },
	{ "recurse",	FFPARS_SETVAL('R') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(recurse) },
	{ "member",	FFPARS_TCHARPTR | FFPARS_FSTRZ | FFPARS_FCOPY | FFPARS_FNOTEMPTY | FFPARS_FMULTI, FFPARS_DST(&arg_member) },
	{ "show",	FFPARS_TBOOL8 | FFPARS_FALONE, OFF(show) },

	{ "skip-errors",	FFPARS_SETVAL('k') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(skip_errors) },
	{ "deflate-level",	FFPARS_TINT8, OFF(deflate_level) },
	{ "jpeg-quality",	FFPARS_TINT8, OFF(jpeg_quality) },
	{ "png-compression",	FFPARS_TINT8, OFF(png_comp) },

	// OUTPUT
	{ "out",	FFPARS_SETVAL('o') | FFPARS_TCHARPTR | FFPARS_FCOPY | FFPARS_FSTRZ, OFF(out) },
	{ "outdir",	FFPARS_SETVAL('C') | FFPARS_TCHARPTR | FFPARS_FCOPY | FFPARS_FSTRZ, OFF(outdir) },
	{ "force",	FFPARS_SETVAL('f') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(force) },
	{ "test",	FFPARS_SETVAL('t') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(test) },
	{ "date",	FFPARS_TSTR, FFPARS_DST(&arg_date) },
	{ "date-as",	FFPARS_TCHARPTR | FFPARS_FSTRZ | FFPARS_FCOPY | FFPARS_FNOTEMPTY, OFF(date_as_fn) },
	{ "preserve-date",	FFPARS_TBOOL8 | FFPARS_FALONE, OFF(preserve_date) },

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

	if (FF_BADFD == (f = fffile_open(fn, O_RDONLY | O_NOATIME)))
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
		if (lf == end)
			break;
		d = lf + 1;
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

	if (FF_BADFD == (f = fffile_open(fn, O_RDONLY | O_NOATIME)))
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
	if (0 == fftime_fromstr(&dt, val->ptr, val->len, FFTIME_YMD))
		return FFPARS_EBADVAL;
	fftime_join(&c->mtime, &dt, FFTIME_TZLOCAL);
	return 0;
}

static int cmdline(int argc, char **argv)
{
	ffparser_schem ps;
	ffparser p;
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
	ffpars_free(&p);
	return ret;
}

static const int sigs[] = { SIGINT };
static const int sigs_block[] = { SIGINT };

static void cmds_free(void)
{
	if (core != NULL)
		ffsig_ctl(&g->sigs_task, core->kq, sigs, FFCNT(sigs), NULL);

	ffchain_item *it;
	FFCHAIN_FOR(&g->in_list, it) {
		struct in_ent *ent = FF_GETPTR(struct in_ent, sib, it);
		it = it->next;
		ffmem_free(ent);
	}

	ffmem_safefree(g->conf.out);
	ffmem_safefree(g->conf.outdir);
	ffmem_safefree(g->conf.date_as_fn);

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
	cmd.recurse = c->conf.recurse;
	cmd.output.fn = c->conf.out;
	g->stdout_busy = cmd.out_std = (c->conf.out != NULL && ffsz_eq(c->conf.out, "@stdout"));
	cmd.outdir = c->conf.outdir;
	cmd.out_overwrite = c->conf.force;
	cmd.skip_err = c->conf.skip_errors;
	cmd.deflate_level = c->conf.deflate_level;
	cmd.jpeg_quality = c->conf.jpeg_quality;
	cmd.png_comp = c->conf.png_comp;
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

	if (NULL == (core = core_create(&std_log, argv, env)))
		return 1;

	if (argc == 1) {
		arg_help(NULL, NULL);
		return 1;
	}

	if (0 != cmdline(argc, argv))
		goto done;

	g->gconf.loglev = FCOM_LOGINFO;
	if (g->conf.debug)
		g->gconf.loglev = FCOM_LOGDBG;
	else if (g->conf.verbose)
		g->gconf.loglev = FCOM_LOGVERB;

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
	core_free();
	cmds_free();
	return r;
}
