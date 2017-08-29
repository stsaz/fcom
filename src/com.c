/** Execute commands.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/list.h>
#include <FF/path.h>


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
};

static struct gcomm *g;

typedef struct {
	ffchain_item sib;
	void *ptr;
	const fcom_filter *filter;
	const char *name;
	uint done :1;
} filter;

typedef struct {
	fcom_cmd cmd;
	fftime tm_start;

	ffarr filters; //filter[]
	ffchain chain;
	fflist_cursor cur;
	fflist_cursor cur_stored;
	// uint out_flush :1;

	ffchain in_list; //struct in_ent[]
	ffchain_item *in_next;
} comm;

struct in_ent {
	ffchain_item sib;
	char fn[0];
};


// COMMAND
static void* com_create(fcom_cmd *cmd);
static void com_close(void *p);
static int com_run(void *p);
static int com_ctrl(fcom_cmd *c, uint cmd, ...);
static int com_arg_add(fcom_cmd *c, const ffstr *arg, uint flags);
static char* com_arg_next(fcom_cmd *c, uint flags);
static int com_reg(const char *op, const char *mod);
const fcom_command core_com_iface = {
	.create = &com_create, .close = &com_close, .run = &com_run, .ctrl = &com_ctrl,
	.arg_add = &com_arg_add, .arg_next = &com_arg_next,
	.reg = &com_reg,
};

static const char* getmod_bycmd(const char *cmdname);
static void filt_close(comm *c, filter *f);


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

	if (NULL == ffarr_allocT(&c->filters, 8, filter))
		goto err;

	filter *f;
	f = ffarr_push(&c->filters, filter);
	ffmem_tzero(f);
	if (NULL == (f->name = getmod_bycmd(cmd->name))) {
		errlog("unknown operation: %s", cmd->name);
		goto err;
	}
	if (NULL == (f->filter = core->iface(f->name))) {
		errlog("unknown filter: %s", f->name);
		goto err;
	}
	ffchain_add(&c->chain, &f->sib);

	c->cur = ffchain_first(&c->chain);

	f = FF_GETPTR(filter, sib, c->cur);
	dbglog(0, "creating context for %s", f->name);
	if (NULL == (f->ptr = f->filter->open(&c->cmd)))
		goto err;
	FF_ASSERT(f->ptr != FCOM_SKIP);
	c->cmd.flags |= FCOM_CMD_FWD;

	if (c->cmd.benchmark)
		ffclk_get(&c->tm_start);

	return &c->cmd;

err:
	com_close(c);
	return NULL;
}

/** Close command context and all its filters. */
static void com_close(void *p)
{
	comm *c = FF_GETPTR(comm, cmd, p);

	if (c->cmd.benchmark) {
		fftime t;
		ffclk_get(&t);
		ffclk_diff(&c->tm_start, &t);
		inflog("'%s' processing time: %u.%06u sec"
			, c->cmd.name, t.s, t.mcs / 1000);
	}

	filter *f;
	FFARR_WALKT(&c->filters, f, filter) {
		if (f->ptr != NULL) {
			c->cur = &f->sib;
			dbglog(0, "closing %s", f->name);
			f->filter->close(f->ptr, &c->cmd);
		}
	}
	ffarr_free(&c->filters);

	dbglog(0, "'%s' finished", c->cmd.name);
	ffmem_free(c);
}

static const char* const filt_rstr[] = {
	"more", "data", "done", "next-done", "err", "syserr", "fin", "async",
};

/** Call filters within the chain. */
static int com_run(void *p)
{
	comm *c = FF_GETPTR(comm, cmd, p);
	int r, op;
	filter *f;

	for (;;) {

		f = FF_GETPTR(filter, sib, c->cur);

		dbglog(0, "%2s calling %s, input:%L  flags:%xu"
			, (c->cmd.flags & FCOM_CMD_FWD) ? ">>" : "<<"
			, f->name, c->cmd.in.len, c->cmd.flags);

		if (f->ptr == NULL) {
			dbglog(0, "creating context for %s", f->name);
			if (NULL == (f->ptr = f->filter->open(&c->cmd)))
				goto err;
			if (f->ptr == FCOM_SKIP) {
				dbglog(0, "%s is skipped", f->name);
				f->ptr = NULL;
				op = FFLIST_CUR_NEXT | FFLIST_CUR_RM;
				goto shift;
			}
		}

		c->cmd.flags &= ~(FCOM_CMD_FIRST | FCOM_CMD_LAST);
		if (c->cur->prev == ffchain_sentl(&c->chain))
			c->cmd.flags |= FCOM_CMD_FIRST;
		if (c->cur->next == ffchain_sentl(&c->chain))
			c->cmd.flags |= FCOM_CMD_LAST;

		r = f->filter->process(f->ptr, &c->cmd);

		dbglog(0, "  %s returned %s, output:%L", f->name, filt_rstr[r], c->cmd.out.len);

		switch (r) {
		case FCOM_DATA:
			op = FFLIST_CUR_NEXT;
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

		case FCOM_NEXTDONE: {
/*
Split the chain into 2, finish the second chain, then continue with the first one.

1) ... -> cur --(NEXTDONE)-> next    ->   ...
2)                        -> next <-...-> ...
3) ... -> cur <-
*/
			FF_ASSERT(c->cur_stored == NULL); //only one NEXTDONE is supported per chain
			FF_ASSERT(c->cur->next != ffchain_sentl(&c->chain)); //@ handle as noop
			ffchain_item *nxt;
			nxt = c->cur->next;
			ffchain_split(c->cur, ffchain_sentl(&c->chain));
			c->cur_stored = c->cur;
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
		case FFLIST_CUR_PREV:
			f = FF_GETPTR(filter, sib, c->cur);
			if (f->done) {
				f->done = 0;
				filt_close(c, f);
				op = FFLIST_CUR_NEXT | FFLIST_CUR_RM;
				goto shift;
			}
			c->cmd.in.len = 0;
			c->cmd.flags &= ~FCOM_CMD_FWD;
			break;

		case FFLIST_CUR_NONEXT:
			if (c->cur_stored != NULL) {
				c->cur = FF_SWAP(&c->cur_stored, NULL);
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
	if (NULL == (e = ffmem_calloc(1, sizeof(struct in_ent) + arg->len + 1)))
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
	if (c->in_next == ffchain_sentl(&c->in_list))
		return NULL;

	struct in_ent *e = FF_GETPTR(struct in_ent, sib, c->in_next);
	c->in_next = c->in_next->next;
	return e->fn;
}

/** Set command's parameters. */
static int com_ctrl(fcom_cmd *_c, uint cmd, ...)
{
	int r = -1;
	comm *c = FF_GETPTR(comm, cmd, _c);
	filter *fcur = FF_GETPTR(filter, sib, c->cur);
	va_list va;
	va_start(va, cmd);

	switch (cmd) {
	case FCOM_CMD_FILTADD:
	case FCOM_CMD_FILTADD_PREV: {
		filter *f;
		const char *name = va_arg(va, char*);
		if (ffarr_isfull(&c->filters)) {
			errlog("can't add more filters", 0);
			goto err;
		}
		f = ffarr_endT(&c->filters, filter);
		ffmem_tzero(f);
		f->name = name;
		f->filter = core->iface(f->name);

		filter *p, *n;
		if (cmd == FCOM_CMD_FILTADD) {
			ffchain_append(&f->sib, &fcur->sib);
			p = fcur,  n = f;
		} else {
			ffchain_prepend(&f->sib, &fcur->sib);
			p = f,  n = fcur;
		}

		dbglog(0, "added %s to chain: %s -> %s"
			, f->name, p->name, n->name);
		c->filters.len++;
		break;
	}
	}

	r = 0;

err:
	va_end(va);
	return r;
}


static void filt_close(comm *c, filter *f)
{
	dbglog(0, "closing %s", f->name);
	f->filter->close(f->ptr, &c->cmd);
	f->ptr = NULL;
	if (ffarr_endT(&c->filters, filter) == f + 1)
		c->filters.len--;
}
