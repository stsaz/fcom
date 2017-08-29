/** fcom startup.
Copyright (c) 2017 Simon Zolin */

#include <fcom.h>

#include <FF/data/psarg.h>
#include <FF/time.h>
#include <FFOS/file.h>
#include <FFOS/sig.h>


#define errlog(fmt, ...)  fcom_errlog("main", fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog("main", fmt, __VA_ARGS__)

struct cmdconf {
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

// global command wrapper
static void* op_open(fcom_cmd *cmd);
static void op_close(void *p, fcom_cmd *cmd);
static int op_process(void *p, fcom_cmd *cmd);
const fcom_filter op_filt = {
	&op_open, &op_close, &op_process,
};


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

	if (flags & FCOM_LOGSYS)
		e = fferr_last();

	fftime_now(&t);
	fftime_split(&dt, &t, FFTIME_TZLOCAL);
	tm.len = fftime_tostr(&dt, stime, sizeof(stime), FFTIME_HMS_MSEC);
	tm.ptr = stime;

	s = buf;
	end = buf + FFCNT(buf) - FFSLEN("\n");
	s += ffs_fmt(s, end, "%S fcom: [%s] ", &tm, log_levs[lev]);
	s += ffs_fmtv(s, end, fmt, va);

	if (flags & FCOM_LOGSYS)
		s += ffs_fmt(s, end, ": %E", e);

	s = ffs_copyc(s, end, '\n');

	fffd fd = (lev <= FCOM_LOGWARN) ? ffstderr : ffstdout;
	fffile_write(fd, buf, s - buf);
	return 0;
}

static int arg_infile(ffparser_schem *p, void *obj, const ffstr *val);
static int arg_help(ffparser_schem *p, void *obj);


#define OFF(member)  FFPARS_DSTOFF(struct cmdconf, member)
static const ffpars_arg cmdline_args[] = {
	{ "",	FFPARS_TSTR | FFPARS_FNOTEMPTY | FFPARS_FMULTI, FFPARS_DST(&arg_infile) },

	{ "verbose",	FFPARS_SETVAL('v') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(verbose) },
	{ "debug",	FFPARS_SETVAL('D') | FFPARS_TBOOL8 | FFPARS_FALONE, OFF(debug) },
	{ "benchmark",	FFPARS_TBOOL8 | FFPARS_FALONE, OFF(benchmark) },
	{ "help",	FFPARS_TBOOL8 | FFPARS_FALONE, FFPARS_DST(arg_help) },
};
#undef OFF


static int in_add(struct job *c, const ffstr *s, uint flags)
{
	struct in_ent *e;
	if (NULL == (e = ffmem_calloc(1, sizeof(struct in_ent) + s->len + 1)))
		return -1;
	ffchain_add(&c->in_list, &e->sib);
	if (c->in_next == NULL)
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

	ffmem_safefree(g);
}


static void* op_open(fcom_cmd *cmd)
{
	return (void*)1;
}

static void op_close(void *p, fcom_cmd *cmd)
{
}

static int op_process(void *p, fcom_cmd *cmd)
{
	g->retcode = 0;
	core->cmd(FCOM_STOP);
	return FCOM_DONE;
}

static void cmd_add(void *param)
{
	struct job *c = param;
	const char *op;
	fcom_cmd *m;

	if (NULL == (op = in_next(c, 0)))
		goto done;
	fcom_cmd cmd = {0};
	cmd.name = op;
	cmd.benchmark = c->conf.benchmark;
	com = core->iface("core.com");
	if (NULL == (m = com->create(&cmd)))
		goto done;

	if (0 != com->fcom_cmd_filtadd_prev(m, "core.globop"))
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
	ffsig_mask(SIG_BLOCK, sigs_block, FFCNT(sigs_block));

	if (NULL == (g = ffmem_new(struct job)))
		return 1;
	g->retcode = 1;
	ffchain_init(&g->in_list);

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
