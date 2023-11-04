/** fcom: startup
2022, Simon Zolin */

#include <fcom.h>
#include <exe/args.h>
#include <ffsys/dylib.h>
#include <ffsys/std.h>
#include <ffsys/path.h>
#include <ffsys/process.h>
#include <ffsys/signal.h>
#include <ffsys/thread.h>
#include <ffsys/globals.h>

struct main {
	char *core_fn;
	ffdl core_dl;
	struct fcom_coreinit *ci;
	struct fcom_core *core;
	struct args conf;
	fcom_task task;
	char binfn[4096];
	ffstr rootdir;
};
static struct main *m;

#include <exe/log.h>

static char* path(const char *fn)
{
	if (ffpath_abs(fn, ffsz_len(fn)))
		return ffsz_dup(fn);
	return ffsz_allocfmt("%S%c%s", &m->rootdir, FFPATH_SLASH, fn);
}

static int load_core()
{
	m->core_fn = path("core." FFDL_EXT);

	if (NULL == (m->core_dl = ffdl_open(m->core_fn, 0))) {
		stdlog(FCOM_LOG_ERR, "ffdl_open: %s: %s", m->core_fn, ffdl_errstr());
		return 1;
	}
	if (NULL == (m->ci = ffdl_addr(m->core_dl, "fcom_coreinit"))) {
		stdlog(FCOM_LOG_ERR, "ffdl_addr: %s: fcom_coreinit: %s", m->core_fn, ffdl_errstr());
		return 1;
	}
	m->ci->init();
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
	stdlog(FCOM_LOG_DBG, "received system signal: %u", i->sig);
	m->core->com->signal_all(i->sig);
}

static void main_free()
{
	if (m->ci != NULL)
		m->ci->destroy();
	if (m->core_dl != NULL)
		ffdl_close(m->core_dl);
	args_destroy(&m->conf);
	ffmem_free(m->core_fn);
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

	if (0 != args_read(&m->conf, argc, argv))
		goto exit;

	if (0 != load_core())
		goto exit;

	struct fcom_core_conf cconf = {
		.timer_resolution_msec = 100,
		.log = stdlogv,
		.app_path = m->rootdir.ptr,
		.debug = m->conf.debug,
		.verbose = m->conf.verbose,
	};
	if (NULL == (m->core = m->ci->conf(&cconf)))
		goto exit;

	m->core->task(&m->task, operation, NULL);

	static const uint sigs[] = { FFSIG_INT };
	ffsig_subscribe(on_signal, sigs, FF_COUNT(sigs));

	ec = m->ci->run();

exit:
	main_free();
	return ec;
}
