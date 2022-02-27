/** fcom core.
Copyright (c) 2017 Simon Zolin */

#include <fcom.h>

#include <FF/path.h>
#include <FF/time.h>
#include <FFOS/dir.h>
#include <FFOS/queue.h>
#include <FFOS/process.h>
#include <FFOS/thread.h>
#include <FFOS/timer.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, "core", fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog("core", fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog("core", fmt, ##__VA_ARGS__)

FF_EXP const fcom_core* core_create(fcom_log log, char **argv, char **env);
FF_EXP void core_free(void);
static void on_timer(void *param);

extern const fcom_command core_com_iface;
extern int core_comm_sig(uint signo);
extern const fcom_mod file_mod;

enum {
	KQ_EVS = 8,
};

#define FCOM_ASSERT(expr) \
while (!(expr)) { \
	FF_ASSERT(0); \
	ffps_exit(1); \
}

struct worker {
	ffthd thd;
	ffthd_id id;
	fffd kq;
	ffkevpost kqpost;
	ffkevent evposted;

	fftaskmgr tskmgr;

	fftimer timer;
	fftimerqueue timerq;
	uint64 timer_period;
	ffkevent timer_kev;

	ffatomic njobs;
	uint init :1;
};

struct fcom {
	struct fcom_conf conf;
	fcom_log log;

	ffarr mods; //struct mod[]
	ffarr ifaces; //struct iface[]
	ffenv env;
	ffstr rootdir;

	ffarr workers; //struct worker[]

	ffkqu_time kqtime;
	uint stopped;
};

struct mod {
	ffdl dl;
	const fcom_mod *mod;
	char *name;
};

struct iface {
	struct mod *m;
	const void *iface;
	char *name;
};

static struct fcom *g;

// CORE
static int core_cmd(uint cmd, ...);
static void core_log(uint flags, const char *fmt, ...);
static void core_logex(uint flags, void *ctx, const char *fmt, ...);
static char* core_getpath(const char *name, size_t len);
static char* core_env_expand(char *dst, size_t cap, const char *src);
static const void* core_iface(const char *name);
static void core_task(uint cmd, fftask *tsk);
static int core_timer(fftimerqueue_node *tmr, int interval, uint flags);
static fcom_core gcore = {
	.cmd = core_cmd,
	.log = core_log, .logex = core_logex,
	.getpath = core_getpath,
	.env_expand = core_env_expand,
	.iface = core_iface,
	.task = core_task,
	.timer = core_timer,
};
fcom_core *core = &gcore;

// FCOM MODULE
static const fcom_mod* coremod_getmod(const fcom_core *_core);
static int coremod_sig(uint signo);
static const void* coremod_iface(const char *name);
static int coremod_conf(const char *name, ffconf_scheme *cs);
static const fcom_mod core_mod = {
	.sig = &coremod_sig, .iface = &coremod_iface, .conf = &coremod_conf,
};

static int work_init(struct worker *w, uint thread);
static void work_destroy(struct worker *w);
static int FFTHDCALL work_loop(void *param);
static uint work_assign(uint flags);
static void work_release(uint wid, uint flags);
static uint work_avail();
static ffbool work_ismain(void);

static int mods_sig(uint sig);
static int mod_add(const ffstr *name, ffconf_scheme *cs);
static struct mod* mod_find(const ffstr *soname);
static struct mod* mod_load(const ffstr *soname);
static void mods_destroy(void);
static void on_posted(void *);

#include <core/core-conf.h>
#include <core/core-mod.h>

static int set_rootdir(char **argv)
{
	char fn[FF_MAXPATH];
	ffstr path;
	const char *p;
	if (NULL == (p = ffps_filename(fn, sizeof(fn), argv[0])))
		return -1;
	if (NULL == ffpath_split2(p, ffsz_len(p), &path, NULL))
		return -1;
	if (NULL == ffstr_dup(&g->rootdir, path.ptr, path.len + FFSLEN("/")))
		return -1;
	return 0;
}

const fcom_core* core_create(fcom_log log, char **argv, char **env)
{
	fflk_setup();
	if (NULL == (g = ffmem_new(struct fcom)))
		return NULL;
	g->log = log;

	if (0 != ffenv_init(&g->env, env)) {
		syserrlog("%s", "ffenv_init");
		goto err;
	}

	fftime_local(&g->conf.tz);
	fftime_storelocal(&g->conf.tz);

	if (0 != set_rootdir(argv))
		goto err;

	g->conf.loglev = FCOM_LOGINFO;
	gcore.conf = &g->conf;

#if defined FF_WIN && FF_WIN < 0x0600
	ffkqu_init();
#endif
	ffkqu_settm(&g->kqtime, (uint)-1);

	ffstr nm;
	ffstr_setcz(&nm, "core.com");
	if (0 != mod_add(&nm, NULL))
		goto err;
	//sub-modules may be initialized only after the core itself
	core_comm_sig(FCOM_SIGINIT);
	file_mod.sig(FCOM_SIGINIT);

	return &gcore;

err:
	core_free();
	return NULL;
}

/** Create main worker */
int core_prepare()
{
	uint n = g->conf.workers;
	if (n == 0) {
		ffsysconf sc;
		ffsc_init(&sc);
		n = ffsc_get(&sc, FFSYSCONF_NPROCESSORS_ONLN);
		g->conf.workers = n;
	}

	if (NULL == ffarr_alloczT(&g->workers, n, struct worker))
		return -1;
	g->workers.len = n;
	dbglog(0, "workers: %u", n);

	struct worker *w = (void*)g->workers.ptr;
	if (0 != work_init(w, 0))
		return -1;
	core->kq = w->kq;
	return 0;
}

static void on_posted(void *param)
{}

static void conf_destroy(struct fcom_conf *c)
{
}

void core_free(void)
{
	struct worker *w;
	FFSLICE_WALK(&g->workers, w) {
		if (w->init)
			work_destroy(w);
	}
	ffarr_free(&g->workers);

	struct iface *pif;
	FFSLICE_WALK(&g->ifaces, pif) {
		ffmem_safefree(pif->name);
	}
	ffarr_free(&g->ifaces);
	mods_destroy();
	ffenv_destroy(&g->env);
	conf_destroy(&g->conf);
	ffstr_free(&g->rootdir);
	ffmem_free0(g);
}

static void core_logex(uint flags, void *ctx, const char *fmt, ...)
{
	if (!fcom_logchk(g->conf.loglev, flags))
		return;

	va_list va;
	va_start(va, fmt);
	g->log(flags, ctx, fmt, va);
	va_end(va);
}

static void core_log(uint flags, const char *fmt, ...)
{
	if (!fcom_logchk(g->conf.loglev, flags))
		return;

	va_list va;
	va_start(va, fmt);
	g->log(flags, NULL, fmt, va);
	va_end(va);
}

static char* core_getpath(const char *name, size_t len)
{
	ffarr s = {0};

	if (ffpath_abs(name, len)) {
		if (0 == ffstr_catfmt(&s, "%*s%Z", len, name))
			goto err;

	} else {

		if (0 == ffstr_catfmt(&s, "%S%*s%Z", &g->rootdir, len, name))
			goto err;
	}

	return s.ptr;

err:
	ffarr_free(&s);
	return NULL;
}

static char* core_env_expand(char *dst, size_t cap, const char *src)
{
	return ffenv_expand(&g->env, dst, cap, src);
}

static const char* const cmd_str[] = {
	"FCOM_READCONF",
	"FCOM_SETCONF",
	"FCOM_MODADD",
	"FCOM_RUN",
	"FCOM_STOP",
	"FCOM_WORKER_ASSIGN",
	"FCOM_WORKER_RELEASE",
	"FCOM_WORKER_AVAIL",
	"FCOM_TASK_XPOST",
	"FCOM_TASK_XDEL",
};

static int core_cmd(uint cmd, ...)
{
	dbglog(0, "received command %s"
		, (cmd < FF_COUNT(cmd_str)) ? cmd_str[cmd] : "");

	int r = 0;
	va_list va;
	va_start(va, cmd);

	switch ((enum FCOM_CMD)cmd) {
	case FCOM_READCONF:
		r = readconf(va_arg(va, char*));
		if (r != 0)
			break;
		r = core_prepare();
		break;

	case FCOM_SETCONF:
		r = setconf(va_arg(va, fcom_conf*));
		break;

	case FCOM_MODADD: {
		ffstr *name = va_arg(va, ffstr*);
		r = mod_add(name, NULL);
		break;
	}

	case FCOM_RUN: {
		if (0 != mods_sig(FCOM_SIGSTART)) {
			r = 1;
			break;
		}
		work_loop(g->workers.ptr);

		struct worker *w;
		FFSLICE_WALK_T(&g->workers, w, struct worker) {
			if (w != (struct worker*)g->workers.ptr && w->init)
				ffkqu_post(&w->kqpost, &w->evposted);
		}

		mods_sig(FCOM_SIGFREE);
		break;
	}

	case FCOM_STOP: {
		FF_WRITEONCE(g->stopped, 1);
		core_com_iface.ctrl(NULL, FCOM_CMD_STOPALL);
		struct worker *w = (void*)g->workers.ptr;
		ffkqu_post(&w->kqpost, &w->evposted);
		break;
	}

	case FCOM_WORKER_ASSIGN: {
		fffd *pkq = va_arg(va, fffd*);
		uint flags = va_arg(va, uint);
		r = work_assign(flags);
		struct worker *w = ffarr_itemT(&g->workers, r, struct worker);
		*pkq = w->kq;
		break;
	}

	case FCOM_WORKER_RELEASE: {
		uint wid = va_arg(va, uint);
		uint flags = va_arg(va, uint);
		work_release(wid, flags);
		break;
	}

	case FCOM_WORKER_AVAIL:
		r = work_avail();
		break;

	case FCOM_TASK_XPOST:
	case FCOM_TASK_XDEL: {
		fftask *task = va_arg(va, fftask*);
		uint wid = va_arg(va, uint);
		struct worker *w = (void*)g->workers.ptr;
		FCOM_ASSERT(wid < g->workers.len);
		w = &w[wid];

		dbglog(0, "task:%p, cmd:%u, active:%u, handler:%p, param:%p"
			, task, cmd, fftask_active(&w->taskmgr, task), task->handler, task->param);

		if (cmd == FCOM_TASK_XPOST) {
			if (1 == fftask_post(&w->tskmgr, task))
				ffkqu_post(&w->kqpost, &w->evposted);
		} else
			fftask_del(&w->tskmgr, task);
		break;
	}

	default:
		FF_ASSERT(0);
	}

	va_end(va);
	return r;
}

static void core_task(uint cmd, fftask *tsk)
{
	struct worker *w = (void*)g->workers.ptr;

	dbglog(0, "task:%p, cmd:%u, active:%u, handler:%p, param:%p"
		, tsk, cmd, fftask_active(&w->tskmgr, tsk), tsk->handler, tsk->param);

	switch ((enum FCOM_TASK)cmd) {
	case FCOM_TASK_ADD:
		if (1 == fftask_post(&w->tskmgr, tsk))
			ffkqu_post(&w->kqpost, &w->evposted);
		break;

	case FCOM_TASK_DEL:
		fftask_del(&w->tskmgr, tsk);
		break;

	default:
		FF_ASSERT(0);
	}
}

static int core_timer(fftimerqueue_node *t, int interval, uint flags)
{
	struct worker *w = (void*)g->workers.ptr;

	dbglog(0, "timer:%p  interval:%u  handler:%p  param:%p"
		, t, interval, t->func, t->param);

	if (interval == 0) {
		fftimerqueue_remove(&w->timerq, t);
		return 0;
	}

	uint period = ffabs(interval);

	if (period < w->timer_period) {
		fftimer_stop(w->timer, w->kq);
		w->timer_period = 0;
		dbglog(0, "stopped kernel timer", 0);
	}

	if (w->timer_period == 0) {
		w->timer_kev.handler = on_timer;
		w->timer_kev.udata = w;
		if (0 != fftimer_start(w->timer, w->kq, &w->timer_kev, period)) {
			syserrlog("fftimer_start()", 0);
			return -1;
		}
		w->timer_period = period;
		dbglog(0, "started kernel timer  interval:%u", period);
	}

	fftime now = fftime_monotonic();
	ffuint now_msec = now.sec*1000 + now.nsec/1000000;
	fftimerqueue_add(&w->timerq, t, now_msec, interval, t->func, t->param);
	return 0;
}

static void on_timer(void *param)
{
	struct worker *w = param;
	fftime now = fftime_monotonic();
	ffuint now_msec = now.sec*1000 + now.nsec/1000000;
	fftimerqueue_process(&w->timerq, now_msec);
	fftimer_consume(w->timer);
}

/** Initialize worker object. */
static int work_init(struct worker *w, uint thread)
{
	fftask_init(&w->tskmgr);
	fftimerqueue_init(&w->timerq);
	if (FFTIMER_NULL == (w->timer = fftimer_create(0))) {
		syserrlog("fftimer_create");
		return 1;
	}

	if (FF_BADFD == (w->kq = ffkqu_create())) {
		syserrlog("%s", ffkqu_create_S);
		return 1;
	}
	ffkqu_post_attach(&w->kqpost, w->kq);

	ffkev_init(&w->evposted);
	w->evposted.oneshot = 0;
	w->evposted.handler = &on_posted;

	if (thread) {
		w->thd = ffthd_create(&work_loop, w, 0);
		if (w->thd == FFTHD_INV) {
			syserrlog("%s", ffthd_create_S);
			work_destroy(w);
			return 1;
		}
		// w->id is set inside a new thread
	} else {
		w->id = ffthd_curid();
	}

	w->init = 1;
	return 0;
}

/** Destroy worker object. */
static void work_destroy(struct worker *w)
{
	if (w->thd != FFTHD_INV) {
		ffthd_join(w->thd, -1, NULL);
		dbglog(0, "thread %xU exited", (int64)w->id);
		w->thd = FFTHD_INV;
	}
	fftimer_close(w->timer, w->kq);
	if (w->kq != FF_BADFD) {
		ffkqu_post_detach(&w->kqpost, w->kq);
		ffkqu_close(w->kq);
		w->kq = FF_BADFD;
	}
}

/** Find the worker with the least number of active jobs.
Initialize data and create a thread if necessary.
Return worker ID. */
static uint work_assign(uint flags)
{
	FCOM_ASSERT(work_ismain());
	struct worker *w, *ww = (void*)g->workers.ptr;
	uint id = 0, j = -1;

	FFSLICE_WALK(&g->workers, w) {
		uint nj = ffatom_get(&w->njobs);
		if (nj < j) {
			id = w - ww;
			j = nj;
			if (nj == 0)
				break;
		}
	}
	w = &ww[id];

	if (!w->init
		&& 0 != work_init(w, 1)) {
		id = 0;
		w = &ww[0];
		goto done;
	}

done:
	if (flags != 0)
		ffatom_inc(&w->njobs);
	return id;
}

/** A job is completed. */
static void work_release(uint wid, uint flags)
{
	struct worker *w = ffarr_itemT(&g->workers, wid, struct worker);
	if (flags != 0) {
		ssize_t n = ffatom_decret(&w->njobs);
		FCOM_ASSERT(n >= 0);
	}
}

/** Get the number of available workers. */
static uint work_avail()
{
	struct worker *w;
	FFSLICE_WALK(&g->workers, w) {
		if (ffatom_get(&w->njobs) == 0)
			return 1;
	}
	return 0;
}

static ffbool work_ismain(void)
{
	struct worker *w = ffarr_itemT(&g->workers, 0, struct worker);
	return (w->id == ffthd_curid());
}

/** Worker's event loop. */
static int FFTHDCALL work_loop(void *param)
{
	struct worker *w = param;
	w->id = ffthd_curid();
	ffkqu_entry *ents;
	if (NULL == (ents = ffmem_callocT(KQ_EVS, ffkqu_entry)))
		return -1;

	dbglog(0, "entering kqueue loop", 0);

	while (!FF_READONCE(g->stopped)) {

		uint nevents = ffkqu_wait(w->kq, ents, KQ_EVS, &g->kqtime);

		if ((int)nevents < 0) {
			if (fferr_last() != EINTR) {
				syserrlog("%s", ffkqu_wait_S);
				break;
			}
			continue;
		}

		for (uint i = 0;  i != nevents;  i++) {
			ffkqu_entry *ev = &ents[i];
			ffkev_call(ev);

			fftask_run(&w->tskmgr);
		}
	}

	dbglog(0, "left kqueue loop", 0);
	ffmem_free(ents);
	return 0;
}


static const fcom_mod* coremod_getmod(const fcom_core *_core)
{
	return &core_mod;
}

static const void* coremod_iface(const char *name)
{
	const void *pif;
	if (ffsz_eq(name, "com"))
		return &core_com_iface;
	if (NULL != (pif = file_mod.iface(name)))
		return pif;
	return NULL;
}

static int coremod_conf(const char *name, ffconf_scheme *cs)
{
	int r;

	if (0 >= (r = file_mod.conf(name, cs)))
		return r;

	return 0;
}

static int coremod_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT:
		break;
	case FCOM_SIGSTART:
	case FCOM_SIGFREE:
		core_comm_sig(signo);
		file_mod.sig(signo);
		break;
	}
	return 0;
}
