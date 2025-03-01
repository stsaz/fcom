/** fcom: startup
2022, Simon Zolin */

#include <fcom.h>
#include <exe/args.h>
#include <util/log.h>
#include <ffsys/std.h>
#include <ffsys/path.h>
#include <ffsys/process.h>
#include <ffsys/signal.h>
#include <ffsys/thread.h>
#include <ffsys/globals.h>

extern struct fcom_coreinit fcom_coreinit;

struct main {
	struct fcom_coreinit *ci;
	struct fcom_core *core;

	struct zzlog	log;
	fftime			time_last;
	char			log_date[32];

	struct args conf;
	char *cmd_line;
	fcom_task task;
	char binfn[4096];
	ffstr rootdir;
};
static struct main *m;

#include <exe/log.h>

char* path(const char *fn)
{
	if (ffpath_abs(fn, ffsz_len(fn)))
		return ffsz_dup(fn);
	return ffsz_allocfmt("%S%c%s", &m->rootdir, FFPATH_SLASH, fn);
}

static int load_core()
{
	log_init(&m->log);

	fcom_coreinit.init();
	struct fcom_core_conf cconf = {
		.timer_resolution_msec = 100,
		.log = exe_logv,
		.app_path = m->rootdir.ptr,
		.debug = m->conf.debug,
		.verbose = m->conf.verbose,
		.stdout_color = !ffstd_attr(ffstdout, FFSTD_VTERM, FFSTD_VTERM),
	};
	if (!(m->core = fcom_coreinit.conf(&cconf)))
		return 1;
	if (m->core->stdout_busy)
		log_init(&m->log);
	return 0;
}

static char** argv_copy(char **a, uint n)
{
	ffvec v = {};
	ffvec_allocT(&v, n + 1, char*);
	for (uint i = 0;  i != n;  i++) {
		*ffvec_pushT(&v, char*) = ffsz_dup(a[i]);
	}
	ffvec_zpushT(&v, char*);
	return v.ptr;
}

static void operation(void *param)
{
	fcom_cominfo *cmd = m->core->com->create();
	cmd->argv = argv_copy(m->conf.argv, m->conf.argc);
	cmd->argc = m->conf.argc;

	if (0 != m->core->com->run(cmd))
		m->core->exit(1);
	return;
}

static void on_signal(struct ffsig_info *i)
{
	exe_log(FCOM_LOG_DBG, "received system signal: %u", i->sig);
	m->core->com->signal_all(i->sig);
}

static void main_free()
{
	fcom_coreinit.destroy();
	args_destroy(&m->conf);
	ffmem_free(m->cmd_line);
	ffmem_free(m);
}

int main(int argc, char **argv)
{
	int ec = 1;

	m = ffmem_new(struct main);

	if (NULL == ffps_filename(m->binfn, sizeof(m->binfn), argv[0]))
		goto exit;
	ffpath_splitpath(m->binfn, ffsz_len(m->binfn), &m->rootdir, NULL);
	m->rootdir.ptr[m->rootdir.len] = '\0';

#ifdef FF_WIN
	m->cmd_line = ffsz_alloc_wtou(GetCommandLineW());
#endif
	int r;
	if ((r = args_read(&m->conf, argc, argv, m->cmd_line))) {
		if (r > 0)
			ec = 0;
		goto exit;
	}

	if (load_core())
		goto exit;
	m->core->env = (const char**)environ;

	m->core->task(&m->task, operation, NULL);

	static const uint sigs[] = { FFSIG_INT };
	ffsig_subscribe(on_signal, sigs, FF_COUNT(sigs));

	ec = fcom_coreinit.run();

exit:
	main_free();
	return ec;
}
