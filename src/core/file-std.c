/** Standard input/output.
Copyright (c) 2020 Simon Zolin
*/

#include <fcom.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)

extern const fcom_core *core;

//STDIN
static void* fistd_open(fcom_cmd *cmd);
static void fistd_close(void *ctx, fcom_cmd *cmd);
static int fistd_read(void *ctx, fcom_cmd *cmd);
const fcom_filter fistd_filt = { &fistd_open, &fistd_close, &fistd_read };

//STDOUT
static void* fostd_open(fcom_cmd *cmd);
static void fostd_close(void *ctx, fcom_cmd *cmd);
static int fostd_write(void *ctx, fcom_cmd *cmd);
const fcom_filter fostd_filt = { &fostd_open, &fostd_close, &fostd_write, };


#define FILT_NAME  "stdin"

typedef struct fistd {
	ffarr buf;
} fistd;

static void* fistd_open(fcom_cmd *cmd)
{
	fistd *f;
	if (NULL == (f = ffmem_new(fistd)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&f->buf, 64 * 1024)) {
		fistd_close(f, cmd);
		return FCOM_OPEN_SYSERR;
	}

	return f;
}

static void fistd_close(void *ctx, fcom_cmd *cmd)
{
	fistd *f = ctx;
	ffarr_free(&f->buf);
	ffmem_free(f);
}

static int fistd_read(void *ctx, fcom_cmd *cmd)
{
	fistd *f = ctx;
	size_t r;

	if (cmd->in_seek) {
		fcom_errlog(FILT_NAME, "can't seek on stdin.  offset:%U", cmd->input.offset);
		return FCOM_ERR;
	}

	r = ffstd_fread(ffstdin, f->buf.ptr, f->buf.cap);
	if (r == 0) {
		cmd->out.len = 0;
		cmd->in_last = 1;
		return FCOM_DONE;
	} else if ((ssize_t)r < 0) {
		fcom_syserrlog(FILT_NAME, "%s", fffile_read_S);
		return FCOM_ERR;
	}

	dbglog(0, "read %L bytes from stdin"
		, r);
	ffstr_set(&cmd->out, f->buf.ptr, r);
	return FCOM_DATA;
}

#undef FILT_NAME


static void* fostd_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void fostd_close(void *ctx, fcom_cmd *cmd)
{
}

static int fostd_write(void *ctx, fcom_cmd *cmd)
{
	size_t r;

	r = fffile_write(ffstdout, cmd->in.ptr, cmd->in.len);
	if (r != cmd->in.len)
		return FCOM_SYSERR;

	if (cmd->flags & FCOM_CMD_FIRST)
		return FCOM_DONE;

	return FCOM_MORE;
}
