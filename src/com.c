/** Execute commands.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/sys/dir.h>
#include <FF/list.h>
#include <FF/path.h>
#include <FFOS/process.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, "com", fmt, __VA_ARGS__)
#define verblog(fmt, ...)  fcom_verblog("com", fmt, __VA_ARGS__)
#define inflog(fmt, ...)  fcom_infolog("com", fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog("com", fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog("com", fmt, __VA_ARGS__)

extern const fcom_core *core;

struct cmd {
	const char *op;
	const char *mod;
};

struct gcomm {
	ffarr cmds; //struct cmd[]
	ffuint stop_all :1;
};

static struct gcomm *g;

typedef struct {
	ffchain_item sib;
	void *ptr;
	const fcom_filter *filter;
	const char *name;
	uint done :1;
	uint done_prev :1;
	uint data_empty :1;
} filter;

typedef struct {
	fcom_cmd cmd;
	uint wid; //associated worker ID
	fffd kq; //worker's kqueue
	struct ffps_perf psperf;

	ffarr filters; //filter[]
	ffchain chain;
	fflist_cursor cur;
	ffchain_item chain_stored;
	const struct fcom_cmd_mon *mon;

	ffchain in_list; //struct in_ent[]
	ffchain_item *in_next;

	void *udata;
	fftask tsk;
} comm;

struct in_ent {
	ffchain_item sib;
	char fn[0];
};


// COMMAND
static void* com_create(fcom_cmd *cmd);
static void com_close(void *p);
static int com_run(void *p);
static size_t com_ctrl(fcom_cmd *c, uint cmd, ...);
static int com_arg_add(fcom_cmd *c, const ffstr *arg, uint flags);
static char* com_arg_next(fcom_cmd *c, uint flags);
static int com_reg(const char *op, const char *mod);
const fcom_command core_com_iface = {
	.create = &com_create, .close = &com_close, .run = &com_run, .ctrl = &com_ctrl,
	.arg_add = &com_arg_add, .arg_next = &com_arg_next,
	.reg = &com_reg,
};

static const char* getmod_bycmd(const char *cmdname);
static filter* filt_add(comm *c, uint cmd, const char *name, filter *neigh);
static int filt_call(comm *c, filter *f);
static void filt_close(comm *c, filter *f);
static ffbool file_matches(comm *c, const char *full_fn, ffbool dir);
static int dir_scan(comm *c, char *name);
static char* chain_print(comm *c, const ffchain_item *mark, char *buf, size_t cap);


int core_comm_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT:
		if (NULL == (g = ffmem_new(struct gcomm)))
			return -1;
		break;
	case FCOM_SIGFREE:
		ffarr_free(&g->cmds);
		ffmem_safefree0(g);
		break;
	}
	return 0;
}

static const char* getmod_bycmd(const char *cmdname)
{
	struct cmd *c;
	FFARR_WALKT(&g->cmds, c, struct cmd) {
		if (ffsz_eq(cmdname, c->op))
			return c->mod;
	}
	return NULL;
}


/** Create context for a new command.  Add filter associated with command name. */
static void* com_create(fcom_cmd *cmd)
{
	dbglog(0, "creating '%s'", cmd->name);

	comm *c;
	if (NULL == (c = ffmem_new(comm)))
		return NULL;

	ffmemcpy(&c->cmd, cmd, sizeof(fcom_cmd));
	ffchain_init(&c->chain);
	ffchain_init(&c->in_list);
	c->in_next = ffchain_first(&c->in_list);
	c->cur = ffchain_first(&c->chain);

	if (cmd->out_fn_copy && NULL == (c->cmd.output.fn = ffsz_alcopyz(cmd->output.fn)))
		goto err;

	if (c->cmd.benchmark)
		ffps_perf(&c->psperf, FFPS_PERF_REALTIME | FFPS_PERF_CPUTIME | FFPS_PERF_RUSAGE);

	if (NULL == ffarr_allocT(&c->filters, 8, filter))
		goto err;

	if (!(cmd->flags & FCOM_CMD_EMPTY)) {
		const char *name;
		if (NULL == (name = getmod_bycmd(cmd->name))) {
			errlog("unknown operation: %s", cmd->name);
			goto err;
		}
		if (NULL == filt_add(c, FCOM_CMD_FILTADD_LAST, name, NULL))
			goto err;
	} else
		c->cmd.flags &= ~FCOM_CMD_EMPTY;

	c->cmd.flags |= FCOM_CMD_FWD;
	c->wid = core->cmd(FCOM_WORKER_ASSIGN, &c->kq, (c->cmd.flags & FCOM_CMD_INTENSE));
	return &c->cmd;

err:
	com_close(c);
	return NULL;
}

/** Close command context and all its filters. */
static void com_close(void *p)
{
	comm *c = FF_GETPTR(comm, cmd, p);

	core->cmd(FCOM_TASK_XDEL, &c->tsk, c->wid);
	core->cmd(FCOM_WORKER_RELEASE, c->wid, (c->cmd.flags & FCOM_CMD_INTENSE));

	if (c->cmd.benchmark) {
		struct ffps_perf p2 = {};
		ffps_perf(&p2, FFPS_PERF_REALTIME | FFPS_PERF_CPUTIME | FFPS_PERF_RUSAGE);
		ffps_perf_diff(&c->psperf, &p2);
		inflog("'%s' processing time: real:%u.%06u  cpu:%u.%06u (user:%u.%06u system:%u.%06u)"
			"  resources: pagefaults:%u  maxrss:%u  I/O:%u  ctxsw:%u"
			, c->cmd.name
			, (int)fftime_sec(&p2.realtime), (int)fftime_usec(&p2.realtime)
			, (int)fftime_sec(&p2.cputime), (int)fftime_usec(&p2.cputime)
			, (int)fftime_sec(&p2.usertime), (int)fftime_usec(&p2.usertime)
			, (int)fftime_sec(&p2.systime), (int)fftime_usec(&p2.systime)
			, p2.pagefaults, p2.maxrss, p2.inblock + p2.outblock, p2.vctxsw + p2.ivctxsw
			);
	}

	filter *f;
	FFARR_WALKT(&c->filters, f, filter) {
		if (f->ptr != NULL && f->ptr != FCOM_SKIP) {
			c->cur = &f->sib;
			dbglog(0, "closing %s", f->name);
			f->filter->close(f->ptr, &c->cmd);
		}
	}
	ffarr_free(&c->filters);

	ffchain_item *it;
	FFCHAIN_FOR(&c->in_list, it) {
		struct in_ent *e = FF_GETPTR(struct in_ent, sib, it);
		it = it->next;
		ffmem_free(e);
	}

	if (c->cmd.out_fn_copy)
		ffmem_free((char*)c->cmd.output.fn);

	if (c->mon != NULL)
		c->mon->onsig(&c->cmd, 0);

	dbglog(0, "'%s' finished", c->cmd.name);
	ffmem_free(c);
}

static const char* const filt_rstr[] = {
	"more", "back", "data", "done", "output-done", "next-done", "err", "syserr", "fin", "async",
};

static int filt_call(comm *c, filter *f)
{
	int r;

	dbglog(0, "%2s calling %s, input:%L  flags:%xu"
		, (c->cmd.flags & FCOM_CMD_FWD) ? ">>" : "<<"
		, f->name, c->cmd.in.len, c->cmd.flags);

	if (f->ptr == NULL) {
		dbglog(0, "creating context for %s", f->name);
		f->ptr = f->filter->open(&c->cmd);

		if (f->ptr == NULL)
			return FCOM_ERR;

		else if (f->ptr == FCOM_OPEN_SYSERR)
			return FCOM_SYSERR;

		else if (f->ptr == FCOM_SKIP) {
			dbglog(0, "%s is skipped", f->name);
			return FCOM_DONE;
		}
	}

	r = f->filter->process(f->ptr, &c->cmd);

	dbglog(0, "  %s returned %s, output:%L", f->name, filt_rstr[r], c->cmd.out.len);
	return r;
}

/** Return TRUE if:
 . if it's the first filter
 . if all previous filters are marked as "done" */
static int filt_isfirst(comm *c, fflist_item *item)
{
	for (fflist_item *it = item->prev;  ;  it = it->prev) {
		if (it == ffchain_sentl(&c->chain))
			return 1;

		filter *f = FF_GETPTR(filter, sib, it);
		if (!f->done)
			return 0;
	}
	return 0;
}

/** Call filters within the chain. */
static int com_run(void *p)
{
	comm *c = FF_GETPTR(comm, cmd, p);
	int r, op;
	filter *f;

	for (;;) {

		if (g->stop_all) {
			dbglog(0, "forced to stop processing", 0);
			goto err;
		}

		FF_ASSERT(c->cur != ffchain_sentl(&c->chain));
		f = FF_GETPTR(filter, sib, c->cur);

		c->cmd.flags &= ~(FCOM_CMD_FIRST | FCOM_CMD_LAST);
		if (filt_isfirst(c, c->cur))
			c->cmd.flags |= FCOM_CMD_FIRST;
		if (c->cur->next == ffchain_sentl(&c->chain))
			c->cmd.flags |= FCOM_CMD_LAST;

		r = filt_call(c, f);

		switch ((enum FCOM_FILT_R)r) {
		case FCOM_DATA:
			op = FFLIST_CUR_NEXT;
			if (f->data_empty && c->cmd.out.len == 0) {
				errlog("filter %s: keeps returning empty data", f->name);
				goto err;
			}
			f->data_empty = (c->cmd.out.len == 0);
			break;

		case FCOM_DONE:
			if (c->cur->next == ffchain_sentl(&c->chain)) {
				filt_close(c, f);
				op = FFLIST_CUR_NEXT | FFLIST_CUR_RM | FFLIST_CUR_BOUNCE;
			} else {
				// close filter after next filters are done with its data
				f->done = 1;
				op = FFLIST_CUR_NEXT;
			}
			break;

		case FCOM_OUTPUTDONE:
			if (c->cur->next == ffchain_sentl(&c->chain)) {
				goto ok;
			} else {
				// close all previous filters after the next filters are done with the current filter's data
				f->done_prev = 1;
				f->done = 1;
				op = FFLIST_CUR_NEXT;
			}
			break;

		case FCOM_NEXTDONE: {
/*
Split the chain into 2, finish the second chain, then continue with the first one.

1) ... -> cur --(NEXTDONE)-> next    ->   ...
2)                        -> next <-...-> ...
3) ... -> cur <-
*/
			FF_ASSERT(c->chain_stored.next == NULL); //only one NEXTDONE is supported per chain
			FF_ASSERT(c->cur->next != ffchain_sentl(&c->chain)); //@ handle as noop
			ffchain_item *nxt;
			nxt = c->cur->next;
			ffchain_split(c->cur, ffchain_sentl(&c->chain));
			c->chain_stored.next = c->chain.next;
			c->chain_stored.prev = c->cur;
			c->chain.next = nxt;
			c->cur = nxt;
			f = FF_GETPTR(filter, sib, c->cur);
			c->cmd.in = c->cmd.out;
			c->cmd.out.len = 0;
			c->cmd.flags |= FCOM_CMD_FWD;
			continue;
		}

		case FCOM_MORE:
			FF_ASSERT(c->cmd.out.len == 0);
			op = FFLIST_CUR_PREV;
			break;

		case FCOM_BACK:
			op = FFLIST_CUR_PREV;
			break;

		case FCOM_ASYNC:
			return 0;

		case FCOM_FIN:
			goto ok;

		case FCOM_ERR:
			goto err;

		case FCOM_SYSERR:
			syserrlog("%s", f->name);
			goto err;

		default:
			errlog("filter %s: unknown return code %u", f->name, r);
			goto err;
		}

shift:
		op = fflist_curshift(&c->cur, op, ffchain_sentl(&c->chain));

		switch (op) {
		case FFLIST_CUR_NEXT:
			c->cmd.in = c->cmd.out;
			c->cmd.out.len = 0;
			c->cmd.flags |= FCOM_CMD_FWD;
			break;

		case FFLIST_CUR_SAME:
			if (f->sib.next == ffchain_sentl(&c->chain)
				&& r == FCOM_DATA) {
				errlog("filter %s, the last in chain, outputs more data", f->name);
				goto err;
			}
			//fallthrough

		case FFLIST_CUR_PREV:
			f->data_empty = 0;
			f = FF_GETPTR(filter, sib, c->cur);
			if (f->done_prev) {
				f->done_prev = 0;
				ffchain_item *it;
				FFCHAIN_FOR(&c->chain, it) {
					if (it == c->cur)
						break;
					filter *ftmp = FF_GETPTR(filter, sib, it);
					it = it->next;
					filt_close(c, ftmp);
					ffchain_rm(&c->chain, &ftmp->sib);
				}
			}
			if (f->done) {
				f->done = 0;
				filt_close(c, f);
				op = FFLIST_CUR_NEXT | FFLIST_CUR_RM | FFLIST_CUR_BOUNCE;
				goto shift;
			}

			if (r == FCOM_BACK) {
				c->cmd.in = c->cmd.out;
				c->cmd.out.len = 0;
				c->cmd.flags |= FCOM_CMD_FWD;
			} else {
				c->cmd.in.len = 0;
				c->cmd.flags &= ~FCOM_CMD_FWD;
			}
			break;

		case FFLIST_CUR_NONEXT:
			if (c->chain_stored.next != NULL) {
				c->chain = c->chain_stored;
				c->chain_stored.next = c->chain_stored.prev = NULL;
				c->cur = c->chain.prev;
				c->cmd.in.len = 0;
				c->cmd.flags &= ~FCOM_CMD_FWD;
				break;
			}
			if (ffchain_empty(&c->chain))
				goto ok;
			errlog("the last filter %s returned data", f->name);
			goto err;

		case FFLIST_CUR_NOPREV:
			errlog("%s requested more data", f->name);
			goto err;
		}
	}

err:
	c->cmd.err = 1;

ok:
	com_close(c);
	return 0;
}

/** Associate a command name with filter. */
static int com_reg(const char *op, const char *mod)
{
	struct cmd *c;
	if (NULL == (c = ffarr_pushgrowT(&g->cmds, 8, struct cmd)))
		return -1;
	c->op = op;
	c->mod = mod;
	return 0;
}

/** Add an argument. */
static int com_arg_add(fcom_cmd *_c, const ffstr *arg, uint flags)
{
	dbglog(0, "adding arg '%S'", arg);
	comm *c = FF_GETPTR(comm, cmd, _c);
	struct in_ent *e;

	if (NULL == (e = ffmem_alloc(sizeof(struct in_ent) + arg->len + 1)))
		return -1;

	ffchain_add(&c->in_list, &e->sib);
	if (c->in_next == ffchain_sentl(&c->in_list))
		c->in_next = ffchain_first(&c->in_list);

	ffsz_fcopy(e->fn, arg->ptr, arg->len);
	return 0;
}

/** Get next argument. */
static char* com_arg_next(fcom_cmd *_c, uint flags)
{
	comm *c = FF_GETPTR(comm, cmd, _c);
	struct in_ent *e;
	fffileinfo fi;

	for (;;) {
		if (c->in_next == ffchain_sentl(&c->in_list))
			return NULL;

		e = FF_GETPTR(struct in_ent, sib, c->in_next);

		if (c->cmd.recurse) {
			if (0 == fffile_infofn(e->fn, &fi)
				&& fffile_isdir(fffile_infoattr(&fi))) {
				dir_scan(c, e->fn);

				ffstr name;
				ffpath_split2(e->fn, ffsz_len(e->fn), NULL, &name);

				if (ffstr_eqz(&name, ".")
					|| ffstr_eqz(&name, "..")
					|| !file_matches(c, e->fn, 0)) {
					FF_ASSERT(!(flags & FCOM_CMD_ARG_PEEK));
					c->in_next = e->sib.next;
					continue;
				}
			}
		}

		if ((flags & FCOM_CMD_ARG_FILE)
			&& 0 == fffile_infofn(e->fn, &fi)
			&& fffile_isdir(fffile_infoattr(&fi))) {
			c->in_next = e->sib.next;
			continue;
		}

		break;
	}

	if (!(flags & FCOM_CMD_ARG_PEEK))
		c->in_next = e->sib.next;
	return e->fn;
}

/**
'include' filter matches files only.
'exclude' filter matches files & directories (names or full paths).
Return TRUE if filename matches user's filename wildcards. */
static ffbool file_matches(comm *c, const char *full_fn, ffbool dir)
{
	const ffstr *wc;
	ffbool ok = 1;
	ffstr fn;
	ffstr_setz(&fn, full_fn);

	if (!dir) {
		ok = (c->cmd.include_files.len == 0);
		FFARR_WALKT(&c->cmd.include_files, wc, ffstr) {

			if (0 == ffs_wildcard(wc->ptr, wc->len, fn.ptr, fn.len, FFS_WC_ICASE)
				|| ffpath_match(&fn, wc, FFPATH_CASE_ISENS)) {
				ok = 1;
				break;
			}
		}
		if (!ok)
			return 0;
	}

	FFARR_WALKT(&c->cmd.exclude_files, wc, ffstr) {

		if (0 == ffs_wildcard(wc->ptr, wc->len, fn.ptr, fn.len, FFS_WC_ICASE)
			|| ffpath_match(&fn, wc, FFPATH_CASE_ISENS)) {
			return 0;
		}
	}

	return ok;
}

#ifdef FCOM_TEST
#include <FFOS/test.h>
#define x  FFTEST_BOOL
void test_file_matches()
{
	comm *c = ffmem_new(comm);

	ffstr *dst, s, wc;
	ffarr aincl = {};
	ffstr_setz(&s, "/path");
	while (s.len != 0) {
		ffstr_nextval3(&s, &wc, ';');
		dst = ffarr_pushgrowT(&aincl, 4, ffstr);
		*dst = wc;
	}
	ffarr_set(&c->cmd.include_files, aincl.ptr, aincl.len);

	ffarr aexcl = {};
	ffstr_setz(&s, "*/.git;/path/bin;/path/dir/_bin;*.zip");
	while (s.len != 0) {
		ffstr_nextval3(&s, &wc, ';');
		dst = ffarr_pushgrowT(&aexcl, 4, ffstr);
		*dst = wc;
	}
	ffarr_set(&c->cmd.exclude_files, aexcl.ptr, aexcl.len);

	x(!file_matches(c, "/path2", 0));

	x(file_matches(c, "/path/file", 0));
	x(file_matches(c, "/path/dir", 1));

	x(!file_matches(c, "/path/.git", 1));
	x(file_matches(c, "/path/.git2", 1));
	x(file_matches(c, "/path/1.git", 1));
	x(!file_matches(c, "/path/dir/.git", 1));

	x(!file_matches(c, "/path/bin", 1));
	x(file_matches(c, "/path/binn", 1));
	x(file_matches(c, "/path/dir/bin", 1));

	x(!file_matches(c, "/path/dir/_bin", 1));
	x(file_matches(c, "/path/ddir/_bin", 1));

	x(!file_matches(c, "/path/dir/1.zip", 1));
	x(file_matches(c, "/path/dir/1.zip2", 1));
}
#endif

/** List directory contents and add its filenames to the arguments list. */
static int dir_scan(comm *c, char *name)
{
	ffdirexp dr = {0};
	fffileinfo fi;
	const char *fn;
	int r = -1;
	ffchain files, dirs;
	ffchain_item *last = ffchain_last(&c->in_list);

	ffchain_init(&files);
	ffchain_init(&dirs);

	dbglog(0, "opening directory %s", name);

	if (0 != ffdir_expopen(&dr, name, FFDIR_EXP_NOWILDCARD)) {
		if (fferr_last() != ENOMOREFILES) {
			syserrlog("%s", ffdir_open_S);
			return -1;
		}
		return 0;
	}

	while (NULL != (fn = ffdir_expread(&dr))) {
		ffstr s;
		ffstr_setz(&s, fn);
		struct in_ent *e;

		if (0 != fffile_infofn(fn, &fi))
			ffmem_tzero(&fi);
		uint isdir = fffile_isdir(fffile_infoattr(&fi));
		if (!file_matches(c, fn, isdir))
			continue;

		if (NULL == (e = ffmem_alloc(sizeof(struct in_ent) + s.len + 1)))
			goto done;

		if (c->cmd.fsort == FCOM_CMD_SORT_ALPHA) {
			ffchain_append(&e->sib, last);
			last = &e->sib;
		} else {
			if (isdir) {
				ffchain_add(&dirs, &e->sib);
			} else {
				ffchain_add(&files, &e->sib);
			}
		}

		ffsz_fcopy(e->fn, s.ptr, s.len);
	}

	if (c->cmd.fsort == FCOM_CMD_SORT_FILES_DIRS) {
		//args -> files -> dirs -> ...args
		_ffchain_link2(ffchain_last(&dirs), c->in_next->next);
		_ffchain_link2(ffchain_last(&files), ffchain_first(&dirs));
		_ffchain_link2(c->in_next, ffchain_first(&files));
	} else if (c->cmd.fsort == FCOM_CMD_SORT_DIRS_FILES) {
		//args -> dirs -> files -> ...args
		_ffchain_link2(ffchain_last(&files), c->in_next->next);
		_ffchain_link2(ffchain_last(&dirs), ffchain_first(&files));
		_ffchain_link2(c->in_next, ffchain_first(&dirs));
	}

	r = 0;

done:
	ffdir_expclose(&dr);
	return r;
}

/** Add filter to chain. */
static filter* filt_add(comm *c, uint cmd, const char *name, filter *neigh)
{
	filter *f, *p;
	if (ffarr_isfull(&c->filters)) {
		errlog("can't add more filters", 0);
		return NULL;
	}
	f = ffarr_endT(&c->filters, filter);
	ffmem_tzero(f);
	f->name = name;
	if (NULL == (f->filter = core->iface(f->name))) {
		errlog("no such interface %s", f->name);
		return NULL;
	}

	switch (cmd) {

	case FCOM_CMD_FILTADD:
		p = FF_GETPTR(filter, sib, c->cur);
		ffchain_append(&f->sib, &p->sib);
		break;

	case FCOM_CMD_FILTADD_AFTER:
		p = neigh;
		ffchain_append(&f->sib, &p->sib);
		break;

	case FCOM_CMD_FILTADD_LAST:
		p = FF_GETPTR(filter, sib, ffchain_last(&c->chain));
		ffchain_append(&f->sib, &p->sib);
		break;

	case FCOM_CMD_FILTADD_PREV:
		p = FF_GETPTR(filter, sib, c->cur);
		ffchain_prepend(&f->sib, &p->sib);
		break;
	}

	if (c->cur == ffchain_sentl(&c->chain))
		c->cur = ffchain_first(&c->chain);

	char buf[255];
	dbglog(0, "added %s to chain [%s]"
		, f->name, chain_print(c, &f->sib, buf, sizeof(buf)));
	c->filters.len++;
	return f;
}

static const char* const cmd_str[] = {
	"FCOM_CMD_MONITOR",
	"FCOM_CMD_UDATA",
	"FCOM_CMD_SETUDATA",
	"FCOM_CMD_RUNASYNC",
	"FCOM_CMD_KQ",
	"FCOM_CMD_FILTADD_PREV",
	"FCOM_CMD_FILTADD",
	"FCOM_CMD_FILTADD_AFTER",
	"FCOM_CMD_FILTADD_LAST",
	"FCOM_CMD_STOPALL",
};

/** Set command's parameters. */
static size_t com_ctrl(fcom_cmd *_c, uint cmd, ...)
{
	ssize_t r = -1;
	comm *c = FF_GETPTR(comm, cmd, _c);

	dbglog(0, "command:%s  c:%p"
		, (cmd < FF_COUNT(cmd_str)) ? cmd_str[cmd] : ""
		, c);

	va_list va;
	va_start(va, cmd);

	switch ((enum FCOM_CMD_CTL)cmd) {
	case FCOM_CMD_MONITOR:
		c->mon = va_arg(va, void*);
		dbglog(0, "set monitor iface: %p", c->mon);
		r = 0;
		break;

	case FCOM_CMD_UDATA:
		r = (size_t)c->udata;
		break;
	case FCOM_CMD_SETUDATA:
		c->udata = va_arg(va, void*);
		break;

	case FCOM_CMD_RUNASYNC:
		c->tsk.handler = (void*)&com_run;
		c->tsk.param = c;
		core->cmd(FCOM_TASK_XPOST, &c->tsk, c->wid);
		break;

	case FCOM_CMD_KQ:
		r = (size_t)c->kq;
		break;

	case FCOM_CMD_FILTADD:
	case FCOM_CMD_FILTADD_LAST:
	case FCOM_CMD_FILTADD_PREV: {
		const char *name = va_arg(va, char*);
		if (0 == (r = (size_t)filt_add(c, cmd, name, NULL)))
			goto err;
		break;
	}

	case FCOM_CMD_FILTADD_AFTER: {
		const char *name = va_arg(va, char*);
		void *p = va_arg(va, void*);
		if (0 == (r = (size_t)filt_add(c, cmd, name, p)))
			goto err;
		break;
	}

	case FCOM_CMD_STOPALL:
		g->stop_all = 1;
		break;
	}

err:
	va_end(va);
	return r;
}


static void filt_close(comm *c, filter *f)
{
	char buf[255];
	dbglog(0, "closing %s in chain [%s]"
		, f->name, chain_print(c, &f->sib, buf, sizeof(buf)));
	f->filter->close(f->ptr, &c->cmd);
	f->ptr = FCOM_SKIP;

	uint n = 0;
	FFARR_RWALKT(&c->filters, f, filter) {
		if (f->ptr != FCOM_SKIP)
			break;
		n++;
	}
	c->filters.len -= n;
}

/** Print names of all filters in chain. */
static char* chain_print(comm *c, const ffchain_item *mark, char *buf, size_t cap)
{
	FF_ASSERT(cap != 0);
	char *p = buf, *end = buf + cap - 1;
	ffchain_item *it;
	filter *f;

	if (c->chain_stored.next != NULL) {
		FFCHAIN_WALK(&c->chain_stored, it) {
			if (it == ffchain_sentl(&c->chain))
				break;
			f = FF_GETPTR(filter, sib, it);
			p += ffs_fmt(p, end, (it == mark) ? "*%s -> " : "%s -> ", f->name);
		}
	}

	FFCHAIN_WALK(&c->chain, it) {
		f = FF_GETPTR(filter, sib, it);
		p += ffs_fmt(p, end, (it == mark) ? "*%s -> " : "%s -> ", f->name);
	}

	*p = '\0';
	return buf;
}
