/** fcom: startup
2022, Simon Zolin */

#include <fcom.h>
#include <args.h>
#include <FFOS/dylib.h>
#include <FFOS/std.h>
#include <FFOS/path.h>
#include <FFOS/process.h>
#include <FFOS/signal.h>
#include <FFOS/thread.h>
#include <FFOS/ffos-extern.h>

#ifdef FF_WIN
#define FCOM_CORE_NAME "core.dll"
#else
#define FCOM_CORE_NAME "core.so"
#endif

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

// TIME :TID LEVEL MSG [: SYS-ERROR]
static void stdlogv(uint flags, const char *fmt, va_list args)
{
	uint level = flags & 0xf;
	char buf[4096];
	ffsize cap = sizeof(buf) - 1;
	ffstr d = FFSTR_INITN(buf, 0);

	static const char err_str[][8] = {
		"ERROR",
		"ERROR",
		"WARNING",
		"INFO",
		"VERBOSE",
		"DBG",
	};
	if (level == FCOM_LOG_FATAL) {
		ffstr_addfmt(&d, cap - d.len, "%s\t", err_str[level]);

	} else if (level == FCOM_LOG_INFO) {

	} else if (level != FCOM_LOG_VERBOSE) {
		fftime t;
		fftime_now(&t);
		ffdatetime dt;
		fftime_split1(&dt, &t);
		d.len = fftime_tostr1(&dt, d.ptr, cap - d.len, FFTIME_HMS_MSEC);

		uint64 tid = ffthread_curid();
		ffstr_addfmt(&d, cap - d.len, " :%U %s\t", tid, err_str[level]);
	}
	ffstr_addfmtv(&d, cap - d.len, fmt, args);

	if (flags & FCOM_LOG_SYSERR) {
		ffstr_addfmt(&d, cap - d.len, ": (%d) %s", fferr_last(), fferr_strptr(fferr_last()));
	}

	d.ptr[d.len++] = '\n';
	ffstderr_write(d.ptr, d.len);
}

void stdlog(uint flags, const char *fmt, ...)
{
	va_list args;
	va_start(args, fmt);
	stdlogv(flags, fmt, args);
	va_end(args);
}

static char* path(const char *fn)
{
	if (ffpath_abs(fn, ffsz_len(fn)))
		return ffsz_dup(fn);
	return ffsz_allocfmt("%S%c%s", &m->rootdir, FFPATH_SLASH, fn);
}

static int load_core()
{
	m->core_fn = path(FCOM_CORE_NAME);

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
