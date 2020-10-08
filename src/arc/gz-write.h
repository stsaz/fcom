/** fcom: .gz writer
2020, Simon Zolin
*/

#include <ffpack/gzwrite.h>

#define FILT_NAME "arc.gz1"

struct gzip1 {
	ffgzwrite gz;
	ffstr in;
	ffuint64 insize, outsize;
};

static void* gzip1_open(fcom_cmd *cmd)
{
	struct gzip1 *g = ffmem_new(struct gzip1);

	uint lev = (cmd->deflate_level != 255) ? cmd->deflate_level : 6;
	ffgzwrite_conf gzconf = {};
	gzconf.deflate_level = lev;
	if (cmd->input.fn != NULL)
		ffstr_setz(&gzconf.name, cmd->input.fn);
	gzconf.mtime = cmd->input.mtime.sec;
	if (0 != ffgzwrite_init(&g->gz, &gzconf)) {
		fcom_errlog(FILT_NAME, "%s", ffgzwrite_error(&g->gz));
		gzip1_close(g, cmd);
		return FCOM_OPEN_ERR;
	}

	return g;
}

static void gzip1_close(void *p, fcom_cmd *cmd)
{
	struct gzip1 *g = p;
	ffgzwrite_destroy(&g->gz);
	ffmem_free(g);
}

static int gzip1_process(void *p, fcom_cmd *cmd)
{
	struct gzip1 *g = p;
	int r;

	if (cmd->flags & FCOM_CMD_FWD) {
		if (cmd->flags & FCOM_CMD_FIRST)
			ffgzwrite_finish(&g->gz);
		g->in = cmd->in;
		g->insize += cmd->in.len;
		cmd->in.len = 0;
	}

	r = ffgzwrite_process(&g->gz, &g->in, &cmd->out);

	switch ((enum FFGZWRITE_R)r) {
	case FFGZWRITE_DATA:
		g->outsize += cmd->out.len;
		return FCOM_DATA;

	case FFGZWRITE_DONE:
		fcom_verblog(FILT_NAME, "%U => %U (%u%%)"
			, g->insize, g->outsize, (uint)FFINT_DIVSAFE(g->outsize * 100, g->insize));
		return FCOM_OUTPUTDONE;

	case FFGZWRITE_MORE:
		return FCOM_MORE;

	case FFGZWRITE_ERROR:
		fcom_errlog(FILT_NAME, "%s", ffgzwrite_error(&g->gz));
		return FCOM_ERR;
	}
	return FCOM_ERR;
}

#undef FILT_NAME
