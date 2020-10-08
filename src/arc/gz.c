/** .gz pack/unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/path.h>
#include <FF/number.h>


extern const fcom_core *core;
extern const fcom_command *com;
extern int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);


// GZIP
static void* gzip_open(fcom_cmd *cmd);
static void gzip_close(void *p, fcom_cmd *cmd);
static int gzip_process(void *p, fcom_cmd *cmd);
const fcom_filter gzip_filt = { &gzip_open, &gzip_close, &gzip_process };

// GZIP1
static void* gzip1_open(fcom_cmd *cmd);
static void gzip1_close(void *p, fcom_cmd *cmd);
static int gzip1_process(void *p, fcom_cmd *cmd);
const fcom_filter gzip1_filt = { &gzip1_open, &gzip1_close, &gzip1_process };

// UNGZ
static void* ungz_open(fcom_cmd *cmd);
static void ungz_close(void *p, fcom_cmd *cmd);
static int ungz_process(void *p, fcom_cmd *cmd);
const fcom_filter ungz_filt = { &ungz_open, &ungz_close, &ungz_process };

static void* ungz1_open(fcom_cmd *cmd);
static void ungz1_close(void *p, fcom_cmd *cmd);
static int ungz1_process(void *p, fcom_cmd *cmd);
const fcom_filter ungz1_filt = { &ungz1_open, &ungz1_close, &ungz1_process };

#include <arc/gz-read.h>
#include <arc/gz-write.h>

#define FILT_NAME  "arc.gz"

typedef struct gzip {
	ffarr fn;
	fcom_cmd *cmd;
	ffatomic nsubtasks;
	uint close :1;
} gzip;

static void* gzip_open(fcom_cmd *cmd)
{
	gzip *g;
	if (NULL == (g = ffmem_new(gzip)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&g->fn, 1024)) {
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

	if (0 != ffatom_get(&g->nsubtasks)) {
		// wait until the last subtask is finished
		g->close = 1;
		return;
	}

	ffarr_free(&g->fn);
	ffmem_free(g);
}

static int gzip_process1(gzip *g, fcom_cmd *cmd, const char *ifn, const char *ofn);

static int gzip_process(void *p, fcom_cmd *cmd)
{
	gzip *g = p;
	const char *ifn, *ofn;

	for (;;) {
		if (NULL == (ifn = com->arg_next(cmd, 0))) {
			if (0 != ffatom_get(&g->nsubtasks))
				return FCOM_ASYNC;
			return FCOM_DONE;
		}

		ofn = cmd->output.fn;
		if (cmd->output.fn == NULL) {
			ffstr outdir, name;
			ffstr_setz(&name, ifn);
			if (cmd->outdir != NULL)
				ffstr_setz(&outdir, cmd->outdir);
			else
				ffstr_setz(&outdir, ".");
			ffpath_split2(name.ptr, name.len, NULL, &name);
			g->fn.len = 0;
			if (0 == ffstr_catfmt(&g->fn, "%S/%S.gz%Z", &outdir, &name))
				return FCOM_SYSERR;
			ofn = g->fn.ptr;
		}

		int r = gzip_process1(g, cmd, ifn, ofn);
		if (r != FCOM_MORE)
			return r;

		if (0 == core->cmd(FCOM_WORKER_AVAIL))
			break;
	}

	return FCOM_ASYNC;
}

static void gzip_task_done(fcom_cmd *cmd, uint sig);
static const struct fcom_cmd_mon gzip_mon_iface = { &gzip_task_done };

static void gzip_task_done(fcom_cmd *cmd, uint sig)
{
	struct gzip *g = (void*)com->ctrl(cmd, FCOM_CMD_UDATA);
	if (0 == ffatom_decret(&g->nsubtasks) && g->close) {
		gzip_close(g, NULL);
		return;
	}

	com->ctrl(g->cmd, FCOM_CMD_RUNASYNC);
}

static void fcom_cmd_set(fcom_cmd *dst, const fcom_cmd *src)
{
	ffmemcpy(dst, src, sizeof(*dst));
	ffstr_null(&dst->in);
	ffstr_null(&dst->out);
}

static int gzip_process1(gzip *g, fcom_cmd *cmd, const char *ifn, const char *ofn)
{
	void *nc;

	fcom_cmd ncmd = {};
	fcom_cmd_set(&ncmd, cmd);
	ncmd.name = "arc.gz1";
	ncmd.flags = FCOM_CMD_EMPTY | FCOM_CMD_INTENSE;
	ncmd.input.fn = ifn;
	ncmd.output.fn = ofn;
	ncmd.out_fn_copy = 1;

	if (NULL == (nc = com->create(&ncmd)))
		return FCOM_ERR;

	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(&ncmd));
	if (NULL == (void*)com->ctrl(nc, FCOM_CMD_FILTADD_LAST, "arc.gz1")) {
		com->close(nc);
		return FCOM_ERR;
	}
	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(&ncmd));

	g->cmd = cmd;
	com->fcom_cmd_monitor(nc, &gzip_mon_iface);
	com->ctrl(nc, FCOM_CMD_SETUDATA, g);
	ffatom_inc(&g->nsubtasks);

	com->ctrl(nc, FCOM_CMD_RUNASYNC);
	return FCOM_MORE;
}

#undef FILT_NAME


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
