/** Archives.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

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

// UNXZ
static void* unxz_open(fcom_cmd *cmd);
static void unxz_close(void *p, fcom_cmd *cmd);
static int unxz_process(void *p, fcom_cmd *cmd);
const fcom_filter unxz_filt = {
	&unxz_open, &unxz_close, &unxz_process,
};


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
	{ "unxz", "arc.unxz", &unxz_filt },
};

static int arc_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		ffmem_init();
		com = core->iface("core.com");
		const struct cmd *c;
		FFARRS_FOREACH(cmds, c) {
			if (0 != com->reg(c->name, c->mod))
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


#define FILT_NAME  "arc.unxz"

typedef struct unxz {
	uint state;
	ffxz xz;
	ffarr buf;
	ffarr fn;
} unxz;

enum {
	UNXZ_BUFSIZE = 64 * 1024,
};

static void* unxz_open(fcom_cmd *cmd)
{
	unxz *x;
	if (NULL == (x = ffmem_new(unxz)))
		return NULL;

	if (NULL == ffarr_alloc(&x->buf, UNXZ_BUFSIZE)) {
		fcom_syserrlog(FILT_NAME, "%s", ffmem_alloc_S);
		goto err;
	}

	return x;

err:
	unxz_close(x, cmd);
	return NULL;
}

static void unxz_close(void *p, fcom_cmd *cmd)
{
	unxz *x = p;
	ffarr_free(&x->buf);
	ffarr_free(&x->fn);
	ffxz_close(&x->xz);
	ffmem_free(x);
}

static int unxz_process(void *p, fcom_cmd *cmd)
{
	unxz *x = p;
	int r;
	enum { R_FIRST, R_INIT, R_DATA, R_EOF, };

	switch (x->state) {
	case R_EOF:
		if (!(cmd->flags & FCOM_CMD_FWD))
			return FCOM_MORE;
		if (cmd->in.len != 0)
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%U", cmd->input.offset);
		FF_CMPSET(&cmd->output.fn, x->fn.ptr, NULL);
		x->state = R_FIRST;
		//fall through

	case R_FIRST:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, "core.file-in");
		x->state = R_INIT;
		return FCOM_MORE;

	case R_INIT:
		ffxz_init(&x->xz, cmd->input.size);
		cmd->output.mtime = cmd->input.mtime;
		x->state = R_DATA;
		break;
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
		if (cmd->flags & FCOM_CMD_LAST) {
			// "/path/file.txt.xz" -> "file.txt"
			ffstr name;
			ffpath_split2(cmd->input.fn, ffsz_len(cmd->input.fn), NULL, &name);
			ffpath_splitname(name.ptr, name.len, &name, NULL);
			x->fn.len = 0;
			if (cmd->output.fn == NULL) {
				if (0 == ffstr_catfmt(&x->fn, "%S%Z", &name))
					return FCOM_SYSERR;
				cmd->output.fn = x->fn.ptr;
			}
			com->ctrl(cmd, FCOM_CMD_FILTADD, "core.file-out");
		}
		continue;

	case FFXZ_DATA:
		cmd->out = x->xz.out;
		return FCOM_DATA;

	case FFXZ_DONE:
		fcom_verblog(FILT_NAME, "finished: %U => %U", x->xz.insize, x->xz.outsize);
		ffxz_close(&x->xz);
		ffmem_tzero(&x->xz);
		x->state = R_EOF;
		return FCOM_NEXTDONE;

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
