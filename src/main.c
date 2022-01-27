/** fcom startup.
Copyright (c) 2017 Simon Zolin */

#include <fcom.h>

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
	char *passwd;
	byte force;
	byte test;
	fftime mtime;

	ffarr members; //char*[]
	ffarr2 include_files; //ffstr[]
	ffarr2 exclude_files; //ffstr[]
	char *include_files_data, *exclude_files_data;

	ffarr2 servers; //ffstr[]
	char *servers_data;

	ffstr search;
	ffstr replace;
	char *sr_data;

	uint crop_width;
	uint crop_height;

	byte recurse;
	byte del_source;
	byte show;
	byte skip_errors;
	byte jpeg_quality;
	byte png_comp;
	byte preserve_date;
	byte colors;

	byte deflate_level;
	char zstd_level;
	byte zstd_workers;
	byte comp_method;

	byte debug;
	byte verbose;
	byte benchmark;
	byte workers;
};

struct job {
	struct cmdconf conf;
	fcom_conf gconf;
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

static int std_log(uint flags, void *ctx, const char *fmt, va_list va);
static int in_add(struct job *c, const ffstr *s, uint flags);
static const char* in_next(struct job *c, uint flags);

static void mon_onsig(fcom_cmd *cmd, uint sig);
static const struct fcom_cmd_mon mon = { &mon_onsig };

#include <cmdline.h>

static const char *const log_levs[] = {
	"error", "warning", "info", "verbose", "debug",
};

#define LOG_BUF 4096

/*
TIME fcom: \[LEV\] [CTX:] MSG [: SYSERR]
*/
static int std_log(uint flags, void *ctx, const char *fmt, va_list va)
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

		const fcom_cmd *cmd = ctx;
		if (cmd != NULL && cmd->input.fn != NULL)
			s += ffs_fmt(s, end, "%s: ", cmd->input.fn);
	}

	s += ffs_fmtv(s, end, fmt, va);

	if (flags & FCOM_LOGSYS)
		s += ffs_fmt(s, end, ": %E", e);

	s = ffs_copyc(s, end, '\n');

	fffd fd = (lev <= FCOM_LOGWARN || g->stdout_busy) ? ffstderr : ffstdout;
	fffile_write(fd, buf, s - buf);
	return 0;
}

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

static const ffuint signals[] = { SIGINT };

static void cmds_free(void)
{
	ffchain_item *it;
	FFCHAIN_FOR(&g->in_list, it) {
		struct in_ent *ent = FF_GETPTR(struct in_ent, sib, it);
		it = it->next;
		ffmem_free(ent);
	}

	struct cmdconf *c = &g->conf;
	ffmem_safefree(g->conf.out);
	ffmem_safefree(g->conf.outdir);
	ffmem_safefree(g->conf.date_as_fn);
	ffmem_free(g->conf.passwd);
	ffslice_free(&g->conf.include_files);
	ffslice_free(&g->conf.exclude_files);
	ffslice_free(&g->conf.servers);
	ffmem_free(c->servers_data);
	ffmem_free(c->sr_data);
	ffmem_free(c->include_files_data);
	ffmem_free(c->exclude_files_data);

	char **ps;
	FFARR_WALKT(&g->conf.members, ps, char*)
		ffmem_safefree(*ps);
	ffarr_free(&g->conf.members);

	ffmem_safefree(g);
}


static void mon_onsig(fcom_cmd *cmd, uint sig)
{
	if (!cmd->err)
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
	cmd.del_source = c->conf.del_source;

	cmd.crop.width = c->conf.crop_width;
	cmd.crop.height = c->conf.crop_height;

	cmd.output.fn = c->conf.out;
	g->stdout_busy = cmd.out_std = (c->conf.out != NULL && ffsz_eq(c->conf.out, "@stdout"));
	cmd.outdir = c->conf.outdir;
	cmd.out_overwrite = c->conf.force;
	cmd.skip_err = c->conf.skip_errors;
	cmd.deflate_level = c->conf.deflate_level;
	cmd.zstd_level = c->conf.zstd_level;
	cmd.zstd_workers = c->conf.zstd_workers;
	cmd.comp_method = c->conf.comp_method;
	cmd.jpeg_quality = c->conf.jpeg_quality;
	cmd.png_comp = c->conf.png_comp;
	cmd.pic_colors = c->conf.colors;
	cmd.read_only = c->conf.test;
	cmd.benchmark = c->conf.benchmark;
	cmd.mtime = c->conf.mtime;
	cmd.date_as_fn = c->conf.date_as_fn;
	cmd.passwd = c->conf.passwd;
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

static void signal_handler(struct ffsig_info *i)
{
	dbglog(0, "received signal:%d", i->sig);
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
	a.len = ffpath_normalize(a.ptr, a.cap, a.ptr, a.len - 1, 0);
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

	if (NULL == (g = ffmem_new(struct job)))
		return 1;
	g->retcode = 1;
	ffchain_init(&g->in_list);
	g->in_next = ffchain_sentl(&g->in_list);
	g->conf.deflate_level = 255;
	g->conf.comp_method = 255;
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

	if (0 != ffsig_subscribe(signal_handler, signals, FF_COUNT(signals))) {
		syserrlog("%s", "ffsig_subscribe()");
		goto done;
	}

	g->tsk.handler = &cmd_add;
	g->tsk.param = g;
	core->task(FCOM_TASK_ADD, &g->tsk);

	core->cmd(FCOM_RUN);

	r = g->retcode;

done:
	dbglog(0, "return code: %d", r);
	if (core != NULL) {
		g->core_free();
	}
	FF_SAFECLOSE(g->core_dl, NULL, ffdl_close);
	cmds_free();
	return r;
}
