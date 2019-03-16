/** .gz pack/unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/pack/gz.h>
#include <FF/path.h>


extern const fcom_core *core;
extern const fcom_command *com;
extern int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);


// GZIP
static void* gzip_open(fcom_cmd *cmd);
static void gzip_close(void *p, fcom_cmd *cmd);
static int gzip_process(void *p, fcom_cmd *cmd);
const fcom_filter gzip_filt = { &gzip_open, &gzip_close, &gzip_process };

// UNGZ
static void* ungz_open(fcom_cmd *cmd);
static void ungz_close(void *p, fcom_cmd *cmd);
static int ungz_process(void *p, fcom_cmd *cmd);
const fcom_filter ungz_filt = { &ungz_open, &ungz_close, &ungz_process };

static void* ungz1_open(fcom_cmd *cmd);
static void ungz1_close(void *p, fcom_cmd *cmd);
static int ungz1_process(void *p, fcom_cmd *cmd);
const fcom_filter ungz1_filt = { &ungz1_open, &ungz1_close, &ungz1_process };


#define FILT_NAME  "arc.gz"

enum {
	BUFSIZE = 64 * 1024,
};

typedef struct gzip {
	uint state;
	ffgz_cook gz;
	ffarr buf;
	ffarr fn;
} gzip;

static void* gzip_open(fcom_cmd *cmd)
{
	gzip *g;
	if (NULL == (g = ffmem_new(gzip)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&g->buf, BUFSIZE)
		|| NULL == ffarr_alloc(&g->fn, 1024)) {
		fcom_syserrlog(FILT_NAME, "%s", ffmem_alloc_S);
		goto err;
	}

	return g;

err:
	gzip_close(g, cmd);
	return FCOM_OPEN_SYSERR;
}

static void gzip_close(void *p, fcom_cmd *cmd)
{
	gzip *g = p;
	ffarr_free(&g->buf);
	ffarr_free(&g->fn);
	ffgz_wclose(&g->gz);
	ffmem_free(g);
}

static int gzip_process(void *p, fcom_cmd *cmd)
{
	gzip *g = p;
	int r;
	enum E { W_NEXT, W_NEWFILE, W_EOF, W_DATA };

	switch ((enum E)g->state) {

	case W_EOF:
		if (!(cmd->flags & FCOM_CMD_FWD))
			return FCOM_MORE;
		FF_ASSERT(cmd->in.len == 0);
		g->state = W_NEXT;
		//fall through

	case W_NEXT:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));

		g->state = W_NEWFILE;
		return FCOM_MORE;

	case W_NEWFILE: {
		if (cmd->output.fn == NULL) {
			ffstr outdir, name;
			ffstr_setz(&name, cmd->input.fn);
			if (cmd->outdir != NULL)
				ffstr_setz(&outdir, cmd->outdir);
			else
				ffstr_setz(&outdir, ".");
			ffpath_split2(name.ptr, name.len, NULL, &name);
			g->fn.len = 0;
			if (0 == ffstr_catfmt(&g->fn, "%S/%S.gz%Z", &outdir, &name))
				return FCOM_SYSERR;
			cmd->output.fn = g->fn.ptr;
		}
		com->ctrl(cmd, FCOM_CMD_FILTADD, FCOM_CMD_FILT_OUT(cmd));

		uint lev = (cmd->deflate_level != 255) ? cmd->deflate_level : 6;
		if (0 != ffgz_winit(&g->gz, lev, 0)) {
			fcom_errlog(FILT_NAME, "%s", ffgz_errstr(&g->gz));
			return FCOM_ERR;
		}

		if (0 != ffgz_wfile(&g->gz, cmd->input.fn, &cmd->input.mtime)) {
			fcom_errlog(FILT_NAME, "%s", ffgz_errstr(&g->gz));
			return FCOM_ERR;
		}

		g->state = W_DATA;
		//fall through
	}

	case W_DATA:
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD) {
		if (cmd->in_last)
			ffgz_wfinish(&g->gz);
		g->gz.in = cmd->in;
	}

	r = ffgz_write(&g->gz, ffarr_end(&g->buf), ffarr_unused(&g->buf));

	switch (r) {
	case FFGZ_DATA:
		cmd->out = g->gz.out;
		return FCOM_DATA;

	case FFGZ_DONE:
		if (g->gz.insize > (uint)-1)
			fcom_warnlog(FILT_NAME, "truncated input file size", 0);
		fcom_infolog(FILT_NAME, "%U => %U (%u%%)"
			, g->gz.insize, g->gz.outsize, (uint)FFINT_DIVSAFE(g->gz.outsize * 100, g->gz.insize));
		ffgz_wclose(&g->gz);
		ffmem_tzero(&g->gz);
		FF_CMPSET(&cmd->output.fn, g->fn.ptr, NULL);
		g->state = W_EOF;
		return FCOM_NEXTDONE;

	case FFGZ_MORE:
		return FCOM_MORE;

	case FFGZ_ERR:
		fcom_errlog(FILT_NAME, "%s", ffgz_errstr(&g->gz));
		return FCOM_ERR;
	}
	return FCOM_ERR;
}

#undef FILT_NAME


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
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%xU", cmd->input.offset);
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
		mtime = ffgz_mtime(&g->gz, &cmd->output.mtime);

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
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%xU", cmd->input.offset);
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
