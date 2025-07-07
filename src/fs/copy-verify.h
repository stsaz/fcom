/** fcom: copy: verify data consistency
2022, Simon Zolin */

#include <ffsys/std.h>

static int verify_init(struct copy *c)
{
	if (!(c->verify || c->print_md5)) return 0;

	if (c->verify && c->cmd->stdout) {
		fcom_errlog("STDOUT output can't be used with --verify");
		return -1;
	}

	if (!(c->vf.md5 = (fcom_hash*)core->com->provide("md5.fcom_md5", 0)))
		return -1;

	return 0;
}

static int verify_open(struct copy *c)
{
	if (!(c->verify || c->print_md5)) return 0;

	c->vf.md5_obj = c->vf.md5->create();
	return 0;
}

static void verify_reset(struct copy *c)
{
	if (!c->vf.md5_obj) return;

	c->vf.md5->close(c->vf.md5_obj),  c->vf.md5_obj = NULL;
}

static int verify_result(struct copy *c)
{
	byte result_w[16];
	c->vf.md5->fin(c->vf.md5_obj, result_w, 16);

	if (ffmem_cmp(c->vf.md5_result_r, result_w, 16)) {
		fcom_errlog("MD5 verification failed.  '%s': %*xb  '%s': %*xb"
			, c->iname, (ffsize)16, c->vf.md5_result_r
			, c->o.name, (ffsize)16, result_w);
		return 1;
	}

	ffstdout_fmt("%*xb *%s\n", (ffsize)16, result_w, c->o.name);
	return 0;
}

static void verify_process(struct copy *c, ffstr data)
{
	if (!c->vf.md5_obj) return;

	c->vf.md5->update(c->vf.md5_obj, data.ptr, data.len);
}

static int verify_read_fin(struct copy *c)
{
	if (!c->vf.md5_obj) return 0;

	c->vf.md5->fin(c->vf.md5_obj, c->vf.md5_result_r, 16);
	c->vf.md5->close(c->vf.md5_obj),  c->vf.md5_obj = NULL;

	if (!c->verify) {
		ffstdout_fmt("%*xb *%s\n", (ffsize)16, c->vf.md5_result_r, c->iname);
		return 0;
	}

	c->vf.md5_obj = c->vf.md5->create();
	c->o.off = 0;
	return 1;
}
