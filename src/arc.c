/** Archives.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/pack/gz.h>
#include <FF/pack/xz.h>
#include <FF/path.h>


static const fcom_core *core;
static const fcom_command *com;

// MODULE
static int arc_sig(uint signo);
static const void* arc_iface(const char *name);
static int arc_conf(const char *name, ffpars_ctx *ctx);
static const fcom_mod arc_mod = {
	.sig = &arc_sig, .iface = &arc_iface, .conf = &arc_conf,
	.name = "Archiver", .desc = "",
};

// UNGZ
static void* ungz_open(fcom_cmd *cmd);
static void ungz_close(void *p, fcom_cmd *cmd);
static int ungz_process(void *p, fcom_cmd *cmd);
static const fcom_filter ungz_filt = { &ungz_open, &ungz_close, &ungz_process };
static void* ungz1_open(fcom_cmd *cmd);
static void ungz1_close(void *p, fcom_cmd *cmd);
static int ungz1_process(void *p, fcom_cmd *cmd);
static const fcom_filter ungz1_filt = { &ungz1_open, &ungz1_close, &ungz1_process };

// UNXZ
static void* unxz_open(fcom_cmd *cmd);
static void unxz_close(void *p, fcom_cmd *cmd);
static int unxz_process(void *p, fcom_cmd *cmd);
static const fcom_filter unxz_filt = {
	&unxz_open, &unxz_close, &unxz_process,
};
static void* unxz1_open(fcom_cmd *cmd);
static void unxz1_close(void *p, fcom_cmd *cmd);
static int unxz1_process(void *p, fcom_cmd *cmd);
static const fcom_filter unxz1_filt = { &unxz1_open, &unxz1_close, &unxz1_process };


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &arc_mod;
}

struct cmd {
	const char *name;
	const char *mod;
	const fcom_filter *iface;
};

static const struct cmd cmds[] = {
	{ "ungz", "arc.ungz", &ungz_filt },
	{ "ungz1", NULL, &ungz1_filt },
	{ "unxz", "arc.unxz", &unxz_filt },
	{ "unxz1", NULL, &unxz1_filt },
};

static int arc_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		ffmem_init();
		com = core->iface("core.com");
		const struct cmd *c;
		FFARRS_FOREACH(cmds, c) {
			if (c->mod != NULL && 0 != com->reg(c->name, c->mod))
				return -1;
		}
		break;
	}
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}

static const void* arc_iface(const char *name)
{
	const struct cmd *cmd;
	FFARRS_FOREACH(cmds, cmd) {
		if (ffsz_eq(name, cmd->name))
			return cmd->iface;
	}
	return NULL;
}

static int arc_conf(const char *name, ffpars_ctx *ctx)
{
	return 0;
}


#define FILT_NAME  "arc.ungz"

static void* ungz_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void ungz_close(void *p, fcom_cmd *cmd)
{
}

static int ungz_process(void *p, fcom_cmd *cmd)
{
	if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
		return FCOM_DONE;

	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(cmd));
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, "arc.ungz1");
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
	return FCOM_NEXTDONE;
}

typedef struct ungz {
	uint state;
	ffgz gz;
	ffarr buf;
	ffarr fn;
} ungz;

enum {
	BUFSIZE = 64 * 1024,
};

static void* ungz1_open(fcom_cmd *cmd)
{
	ungz *g;
	if (NULL == (g = ffmem_new(ungz)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&g->buf, BUFSIZE)) {
		ungz_close(g, cmd);
		return FCOM_OPEN_SYSERR;
	}

	return g;
}

static void ungz1_close(void *p, fcom_cmd *cmd)
{
	ungz *g = p;
	ffarr_free(&g->buf);
	ffarr_free(&g->fn);
	ffgz_close(&g->gz);
	ffmem_free(g);
}

static int ungz1_process(void *p, fcom_cmd *cmd)
{
	ungz *g = p;
	int r;
	enum E { R_FIRST, R_INIT, R_DATA, R_EOF, };

	switch ((enum E)g->state) {
	case R_FIRST:
		if (cmd->in.len == 0) {
			g->state = R_INIT;
			return FCOM_MORE;
		}
		//fall through

	case R_INIT:
		cmd->output.mtime = cmd->input.mtime;
		ffgz_init(&g->gz, cmd->input.size);
		g->state = R_DATA;
		break;

	case R_DATA:
		break;

	case R_EOF:
		if (cmd->in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%U", cmd->input.offset);
			return FCOM_ERR;
		}
		return FCOM_DONE;
	}

	if (cmd->flags & FCOM_CMD_FWD)
		g->gz.in = cmd->in;

	for (;;) {

	r = ffgz_read(&g->gz, ffarr_end(&g->buf), ffarr_unused(&g->buf));
	switch (r) {

	case FFGZ_INFO: {
		uint mtime;
		if (0 != (mtime = ffgz_mtime(&g->gz)))
			cmd->output.mtime.s = mtime;

		const char *gzfn = ffgz_fname(&g->gz);
		cmd->output.size = ffgz_size64(&g->gz, cmd->input.size);

		if (cmd->output.fn == NULL) {
			ffstr name;
			if (gzfn == NULL || *gzfn == '\0') {
				// "/path/file.txt.gz" -> "file.txt"
				ffstr_setz(&name, cmd->input.fn);
				ffpath_split3(name.ptr, name.len, NULL, &name, NULL);
			} else {
				ffpath_split2(gzfn, ffsz_len(gzfn), NULL, &name);
			}

			if (FCOM_DATA != (r = fn_out(cmd, &name, &g->fn)))
				return r;
			cmd->output.fn = g->fn.ptr;
		}

		fcom_dbglog(0, FILT_NAME, "info: name:%s  mtime:%u  osize:%u  crc32:%xu"
			, (gzfn != NULL) ? gzfn : "", mtime, ffgz_size(&g->gz), ffgz_crc(&g->gz));
		continue;
	}

	case FFGZ_DATA:
		cmd->out = g->gz.out;
		return FCOM_DATA;

	case FFGZ_DONE:
		fcom_verblog(FILT_NAME, "finished: %U => %U (%u%%)"
			, g->gz.insize, ffgz_size(&g->gz)
			, (int)(g->gz.insize * 100 / ffgz_size(&g->gz)));

		if (g->gz.in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%U", cmd->input.offset);
			return FCOM_ERR;
		}
		FF_CMPSET(&cmd->output.fn, g->fn.ptr, NULL);
		g->state = R_EOF;
		return FCOM_MORE;

	case FFGZ_MORE:
		return FCOM_MORE;

	case FFGZ_SEEK:
		cmd->input.offset = ffgz_offset(&g->gz);
		cmd->in_seek = 1;
		return FCOM_MORE;

	case FFGZ_ERR:
		fcom_errlog(FILT_NAME, "%s  offset:0x%xU", ffgz_errstr(&g->gz), cmd->input.offset);
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME


#define FILT_NAME  "arc.unxz"

static void* unxz_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void unxz_close(void *p, fcom_cmd *cmd)
{
}

static int unxz_process(void *p, fcom_cmd *cmd)
{
	if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
		return FCOM_DONE;

	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(cmd));
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, "arc.unxz1");
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
	return FCOM_NEXTDONE;
}

typedef struct unxz {
	uint state;
	ffxz xz;
	ffarr buf;
	ffarr fn;
} unxz;

enum {
	UNXZ_BUFSIZE = 64 * 1024,
};

static void* unxz1_open(fcom_cmd *cmd)
{
	unxz *x;
	if (NULL == (x = ffmem_new(unxz)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&x->buf, UNXZ_BUFSIZE)) {
		unxz_close(x, cmd);
		return FCOM_OPEN_SYSERR;
	}

	return x;
}

static void unxz1_close(void *p, fcom_cmd *cmd)
{
	unxz *x = p;
	ffarr_free(&x->buf);
	ffarr_free(&x->fn);
	ffxz_close(&x->xz);
	ffmem_free(x);
}

static int unxz1_process(void *p, fcom_cmd *cmd)
{
	unxz *x = p;
	int r;
	enum E { R_FIRST, R_INIT, R_DATA, R_EOF, };

	switch ((enum E)x->state) {
	case R_FIRST:
		if (cmd->in.len == 0) {
			x->state = R_INIT;
			return FCOM_MORE;
		}
		//fall through

	case R_INIT:
		ffxz_init(&x->xz, cmd->input.size);
		cmd->output.mtime = cmd->input.mtime;
		x->state = R_DATA;
		break;

	case R_DATA:
		break;

	case R_EOF:
		if (cmd->in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%U", cmd->input.offset);
			return FCOM_ERR;
		}
		return FCOM_DONE;
	}

	if (cmd->flags & FCOM_CMD_FWD)
		x->xz.in = cmd->in;

	for (;;) {

	r = ffxz_read(&x->xz, ffarr_end(&x->buf), ffarr_unused(&x->buf));
	switch (r) {

	case FFXZ_INFO:
		cmd->output.size = ffxz_size(&x->xz);
		cmd->output.mtime = cmd->input.mtime;
		cmd->output.attr = cmd->input.attr;

		if (cmd->output.fn == NULL) {
			// "/path/file.txt.xz" -> "file.txt"
			ffstr name;
			ffstr_setz(&name, cmd->input.fn);
			ffpath_split3(name.ptr, name.len, NULL, &name, NULL);
			if (FCOM_DATA != (r = fn_out(cmd, &name, &x->fn)))
				return r;
			cmd->output.fn = x->fn.ptr;
		}
		continue;

	case FFXZ_DATA:
		cmd->out = x->xz.out;
		return FCOM_DATA;

	case FFXZ_DONE:
		fcom_verblog(FILT_NAME, "finished: %U => %U (%u%%)"
			, x->xz.insize, x->xz.outsize
			, (int)(x->xz.insize * 100 / x->xz.outsize));

		if (x->xz.in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%U", cmd->input.offset);
			return FCOM_ERR;
		}
		FF_CMPSET(&cmd->output.fn, x->fn.ptr, NULL);
		x->state = R_EOF;
		return FCOM_MORE;

	case FFXZ_MORE:
		return FCOM_MORE;

	case FFXZ_SEEK:
		cmd->input.offset = ffxz_offset(&x->xz);
		cmd->in_seek = 1;
		return FCOM_MORE;

	case FFXZ_ERR:
		fcom_errlog(FILT_NAME, "%s  offset:0x%xU", ffxz_errstr(&x->xz), cmd->input.offset);
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME
