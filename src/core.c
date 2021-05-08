/** fcom core.
Copyright (c) 2017 Simon Zolin */

#include <fcom.h>

#include <FF/data/conf.h>
#include <FF/path.h>
#include <FF/time.h>
#include <FFOS/dir.h>
#include <FFOS/queue.h>
#include <FFOS/process.h>
#include <FFOS/thread.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, "core", fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog("core", fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog("core", fmt, __VA_ARGS__)

FF_EXP const fcom_core* core_create(fcom_log log, char **argv, char **env);
FF_EXP void core_free(void);

extern const fcom_command core_com_iface;
extern int core_comm_sig(uint signo);
extern const fcom_mod file_mod;

enum {
	KQ_EVS = 8,
	CONF_MBUF = 4096,
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

	fftimer_queue tmrq;
	uint64 period;

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
static int core_timer(fftmrq_entry *tmr, int interval, uint flags);
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
static int coremod_conf(const char *name, ffpars_ctx *ctx);
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
static int mod_add(const ffstr *name, ffpars_ctx *ctx);
static struct mod* mod_find(const ffstr *soname);
static struct mod* mod_load(const ffstr *soname);
static void mods_destroy(void);
static void on_posted(void *);

static int conf_modconf(ffparser_schem *p, void *obj, ffpars_ctx *ctx);
static int conf_mod(ffparser_schem *p, void *obj, const ffstr *val);
int conf_workers(ffparser_schem *p, struct fcom_conf *conf, int64 *val)
{
	if (conf->workers == 0)
		conf->workers = *val;
	return 0;
}
int conf_codepage(ffparser_schem *p, struct fcom_conf *conf, const ffstr *val)
{
	static const char *const cp_str[] = {
		"win1251", // FFUNICODE_WIN1251
		"win1252", // FFUNICODE_WIN1252
		"win866", // FFUNICODE_WIN866
	};
	int cp = ffszarr_ifindsorted(cp_str, FF_COUNT(cp_str), val->ptr, val->len);
	if (cp < 0)
		return FFPARS_EBADVAL;
	conf->codepage = cp + _FFUNICODE_CP_BEGIN;
	return 0;
}

static const ffpars_arg conf_args[] = {
	{ "codepage",  FFPARS_TSTR, FFPARS_DST(conf_codepage) },
	{ "workers",  FFPARS_TINT8, FFPARS_DST(conf_workers) },
	{ "mod_conf",	FFPARS_TOBJ | FFPARS_FOBJ1 | FFPARS_FNOTEMPTY | FFPARS_FMULTI, FFPARS_DST(&conf_modconf) },
	{ "mod",	FFPARS_TSTR | FFPARS_FNOTEMPTY | FFPARS_FMULTI, FFPARS_DST(&conf_mod) },
};

static int conf_modconf(ffparser_schem *p, void *obj, ffpars_ctx *ctx)
{
	const ffstr *name = &p->vals[0];
	if (0 != mod_add(name, ctx))
		return FFPARS_ESYS;
	return 0;
}

static int conf_mod(ffparser_schem *p, void *obj, const ffstr *val)
{
	if (0 != mod_add(val, NULL))
		return FFPARS_ESYS;
	return 0;
}


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
	ffmem_init();
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
	fftime_init();

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
	FFARR_WALKT(&g->workers, w, struct worker) {
		if (w->init)
			work_destroy(w);
	}
	ffarr_free(&g->workers);

	struct iface *pif;
	FFARR_WALKT(&g->ifaces, pif, struct iface) {
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

static int readconf(const char *fn)
{
	ffconf pconf;
	ffparser_schem ps;
	ffpars_ctx ctx = {0};
	int r = FFPARS_ESYS;
	ffstr s;
	char *buf = NULL, *fullfn = NULL;
	size_t n;
	fffd f = FF_BADFD;

	if (NULL == (fullfn = core_getpath(fn, ffsz_len(fn))))
		goto fail;
	fn = fullfn;

	ffpars_setargs(&ctx, &g->conf, conf_args, FFCNT(conf_args));

	ffconf_scheminit(&ps, &pconf, &ctx);
	if (FF_BADFD == (f = fffile_open(fn, O_RDONLY))) {
		syserrlog("%s: %s", fffile_open_S, fn);
		goto fail;
	}

	dbglog(0, "reading config file %s", fn);

	if (NULL == (buf = ffmem_alloc(CONF_MBUF))) {
		goto err;
	}

	for (;;) {
		n = fffile_read(f, buf, CONF_MBUF);
		if (n == (size_t)-1) {
			goto err;
		} else if (n == 0)
			break;
		ffstr_set(&s, buf, n);

		while (s.len != 0) {
			r = ffconf_parsestr(&pconf, &s);
			r = ffconf_schemrun(&ps);

			if (ffpars_iserr(r))
				goto err;
		}
	}

	r = ffconf_schemfin(&ps);

err:
	if (ffpars_iserr(r)) {
		const char *ser = ffpars_schemerrstr(&ps, r, NULL, 0);
		errlog("parse config: %s: %u:%u: near \"%S\": \"%s\": %s"
			, fn
			, pconf.line, pconf.ch
			, &pconf.val, (ps.curarg != NULL) ? ps.curarg->name : ""
			, (r == FFPARS_ESYS) ? fferr_strp(fferr_last()) : ser);
		goto fail;
	}

	r = 0;

fail:
	ffconf_parseclose(&pconf);
	ffpars_schemfree(&ps);
	ffmem_safefree(buf);
	if (f != FF_BADFD)
		fffile_close(f);
	ffmem_safefree(fullfn);
	return r;
}

static int setconf(fcom_conf *conf)
{
	g->conf.loglev = conf->loglev;
	g->conf.workers = conf->workers;
	return 0;
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

static const void* core_iface(const char *nm)
{
	struct iface *pif;
	FFARR_WALKT(&g->ifaces, pif, struct iface) {
		if (ffsz_eq(pif->name, nm))
			return pif->iface;
	}
	errlog("no such interface: %s", nm);
	return NULL;
}

static void mod_destroy(struct mod *m)
{
	FF_SAFECLOSE(m->dl, NULL, ffdl_close);
	ffmem_safefree(m->name);
}

static void mods_destroy(void)
{
	struct mod *m;
	FFARR_WALKT(&g->mods, m, struct mod) {
		mod_destroy(m);
	}
	ffarr_free(&g->mods);
}

static int mods_sig(uint sig)
{
	int r = 0;
	struct mod *m;
	FFARR_WALKT(&g->mods, m, struct mod) {
		if (0 != m->mod->sig(sig))
			r = 1;
	}
	return r;
}

static struct mod* mod_find(const ffstr *soname)
{
	struct mod *m;
	FFARR_WALKT(&g->mods, m, struct mod) {
		if (ffstr_eqz(soname, m->name))
			return m;
	}
	return NULL;
}

static struct mod* mod_load(const ffstr *soname)
{
	ffdl dl = NULL;
	fcom_getmod_t getmod;
	struct mod *m, *rc = NULL;
	char *fn = NULL;

	if (NULL == (m = ffarr_pushgrowT(&g->mods, 16, struct mod)))
		goto fail;
	ffmem_tzero(m);
	if (NULL == (m->name = ffsz_alcopy(soname->ptr, soname->len)))
		goto fail;

	if (ffstr_eqcz(soname, "core")) {
		getmod = &coremod_getmod;

	} else {
		if (NULL == (fn = ffsz_alfmt("%Smod%c%S." FFDL_EXT, &g->rootdir, FFPATH_SLASH, soname)))
			goto fail;

		dbglog(0, "loading module %s", fn);
		if (NULL == (dl = ffdl_open(fn, FFDL_SELFDIR))) {
			errlog("loading %s: %s", fn, ffdl_errstr());
			goto fail;
		}
		if (NULL == (getmod = (void*)ffdl_addr(dl, FCOM_MODFUNCNAME))) {
			errlog("resolving '%s' from %s: %s", FCOM_MODFUNCNAME, fn, ffdl_errstr());
			goto fail;
		}
	}

	if (NULL == (m->mod = getmod(core)))
		goto fail;

	if (0 != m->mod->sig(FCOM_SIGINIT))
		goto fail;
	m->dl = dl;
	rc = m;

fail:
	if (rc == NULL) {
		mod_destroy(m);
		g->mods.len--;
		FF_SAFECLOSE(dl, NULL, ffdl_close);
	}
	ffmem_safefree(fn);
	return rc;
}

static int mod_add(const ffstr *name, ffpars_ctx *ctx)
{
	ffstr soname, iface;
	char fn[128];
	struct mod *m;
	struct iface *pif = NULL;

	ffs_split2by(name->ptr, name->len, '.', &soname, &iface);
	if (soname.len == 0 || iface.len == 0) {
		fferr_set(EINVAL);
		goto fail;
	}

	if (NULL == (m = mod_find(&soname))) {
		if (NULL == (m = mod_load(&soname)))
			goto fail;
	}

	if (0 == ffs_fmt(fn, fn + sizeof(fn), "%S%Z", &iface))
		goto fail;

	if (ctx != NULL) {
		if (m->mod->conf == NULL)
			goto fail;
		if (0 != m->mod->conf(fn, ctx))
			goto fail;
	}

	if (NULL == (pif = ffarr_pushgrowT(&g->ifaces, 16, struct iface)))
		goto fail;
	ffmem_tzero(pif);

	if (NULL == (pif->iface = m->mod->iface(fn)))
		goto fail;

	if (NULL == (pif->name = ffsz_alcopy(name->ptr, name->len)))
		goto fail;
	pif->m = m;
	return 0;

fail:
	return -1;
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
		ffpars_ctx *ctx = va_arg(va, ffpars_ctx*);
		r = mod_add(name, ctx);
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

static int core_timer(fftmrq_entry *tmr, int interval, uint flags)
{
	struct worker *w = (void*)g->workers.ptr;

	dbglog(0, "timer:%p  interval:%u  handler:%p  param:%p"
		, tmr, interval, tmr->handler, tmr->param);

	if (fftmrq_active(&w->tmrq, tmr))
		fftmrq_rm(&w->tmrq, tmr);

	if (interval == 0) {
		if (fftmrq_empty(&w->tmrq)) {
			fftmrq_destroy(&w->tmrq, w->kq);
			dbglog(0, "stopped kernel timer", 0);
		}
		return 0;
	}

	if (fftmrq_started(&w->tmrq) && (uint64)ffabs(interval) < w->period) {
		fftmrq_destroy(&w->tmrq, w->kq);
		dbglog(0, "stopped kernel timer", 0);
	}

	if (!fftmrq_started(&w->tmrq)) {
		fftmrq_init(&w->tmrq);
		if (0 != fftmrq_start(&w->tmrq, w->kq, ffabs(interval))) {
			syserrlog("fftmrq_start()", 0);
			return -1;
		}
		w->period = ffabs(interval);
		dbglog(0, "started kernel timer  interval:%u", ffabs(interval));
	}

	fftmrq_add(&w->tmrq, tmr, interval);
	return 0;
}

/** Initialize worker object. */
static int work_init(struct worker *w, uint thread)
{
	fftask_init(&w->tskmgr);
	fftmrq_init(&w->tmrq);

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
	fftmrq_destroy(&w->tmrq, w->kq);
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

	FFARR_WALKT(&g->workers, w, struct worker) {
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
	FFARR_WALKT(&g->workers, w, struct worker) {
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

static int coremod_conf(const char *name, ffpars_ctx *ctx)
{
	int r;

	if (0 >= (r = file_mod.conf(name, ctx)))
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
