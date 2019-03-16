/** .xz unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/pack/xz.h>
#include <FF/path.h>


extern const fcom_core *core;
extern const fcom_command *com;
extern int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);


// UNXZ
static void* unxz_open(fcom_cmd *cmd);
static void unxz_close(void *p, fcom_cmd *cmd);
static int unxz_process(void *p, fcom_cmd *cmd);
const fcom_filter unxz_filt = { &unxz_open, &unxz_close, &unxz_process };

static void* unxz1_open(fcom_cmd *cmd);
static void unxz1_close(void *p, fcom_cmd *cmd);
static int unxz1_process(void *p, fcom_cmd *cmd);
const fcom_filter unxz1_filt = { &unxz1_open, &unxz1_close, &unxz1_process };


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
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%xU", cmd->input.offset);
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
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%xU", cmd->input.offset);
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
