/** zstd pack/unpack
2021, Simon Zolin
*/

#include <fcom.h>
#include <arc/arc.h>
#include <util/path.h>
#include <zstd/zstd-ff.h>

extern const fcom_core *core;
extern const fcom_command *com;

#include <arc/zstd-read.h>
#include <arc/zstd-write.h>

struct unzstd_wctx {
	ffarr buf;
};

static void* unzstd_open(fcom_cmd *cmd)
{
	struct unzstd_wctx *c = ffmem_new(struct unzstd_wctx);
	return c;
}

static void unzstd_close(void *p, fcom_cmd *cmd)
{
	struct unzstd_wctx *c = p;
	ffarr_free(&c->buf);
	ffmem_free(c);
}

static int unzstd_process(void *p, fcom_cmd *cmd)
{
	struct unzstd_wctx *c = p;
	if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
		return FCOM_DONE;

	if (cmd->output.fn == NULL) {
		// "/path/file.txt.zst" -> "file.txt"
		ffstr name;
		ffstr_setz(&name, cmd->input.fn);
		ffpath_split3(name.ptr, name.len, NULL, &name, NULL);
		int r;
		if (FCOM_DATA != (r = fn_out(cmd, &name, &c->buf)))
			return r;
		cmd->output.fn = c->buf.ptr;
	}

	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(cmd));
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, "arc.unzstd1");
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
	return FCOM_NEXTDONE;
}

const fcom_filter unzstd_filt = { unzstd_open, unzstd_close, unzstd_process };


struct zstd_wctx {
	ffarr buf;
};

static void* zstd_open(fcom_cmd *cmd)
{
	struct zstd_wctx *z = ffmem_new(struct zstd_wctx);
	return z;
}

static void zstd_close(void *p, fcom_cmd *cmd)
{
	struct zstd_wctx *z = p;
	ffarr_free(&z->buf);
	ffmem_free(z);
}

// "/path/file.txt" -> "file.txt.zst"
static int zstd_outfn(struct zstd_wctx *z, fcom_cmd *cmd)
{
	ffstr name;
	ffstr_setz(&name, cmd->input.fn);
	ffpath_splitpath(name.ptr, name.len, NULL, &name);
	int r;
	if (FCOM_DATA != (r = fn_out(cmd, &name, &z->buf)))
		return r;
	z->buf.len = ffsz_len(z->buf.ptr);
	ffvec_addfmt((ffvec*)&z->buf, ".zst%Z");
	return FCOM_DATA;
}

static int zstd_process(void *p, fcom_cmd *cmd)
{
	struct zstd_wctx *z = p;
	if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
		return FCOM_DONE;

	if (cmd->output.fn == NULL) {
		int r;
		if (FCOM_DATA != (r = zstd_outfn(z, cmd)))
			return r;
		cmd->output.fn = z->buf.ptr;
	}

	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(cmd));
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, "arc.zstd1");
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
	return FCOM_NEXTDONE;
}

const fcom_filter zstd_filt = { zstd_open, zstd_close, zstd_process };
