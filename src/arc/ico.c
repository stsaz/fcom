/** .ico unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/pic/pic.h>
#include <FF/pic/ico.h>
#include <FF/path.h>


extern const fcom_core *core;
extern const fcom_command *com;
extern int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);


// ICO INPUT
static void* icoi_open(fcom_cmd *cmd);
static void icoi_close(void *_p, fcom_cmd *cmd);
static int icoi_process(void *_p, fcom_cmd *cmd);
const fcom_filter icoi_filt = { &icoi_open, &icoi_close, &icoi_process };


#define FILT_NAME  "arc.ico-in"

struct icoread {
	ffico ico;
	uint state;
	uint idx, num;
	ffarr infn, outfn;
	uint skip;
	uint dsize;
};

static void* icoi_open(fcom_cmd *cmd)
{
	struct icoread *c = ffmem_new(struct icoread);
	if (c == NULL)
		return FCOM_OPEN_SYSERR;
	ffico_open(&c->ico);
	return c;
}

static void icoi_close(void *p, fcom_cmd *cmd)
{
	struct icoread *c = p;
	ffico_close(&c->ico);
	ffarr_free(&c->infn);
	ffarr_free(&c->outfn);
	ffmem_free(c);
}

/** Initialize .ico data processing chain. */
static int icoi_init_chain(struct icoread *c, fcom_cmd *cmd, uint iconfmt)
{
	int r;
	const struct ffico_file *f = ffico_fileinfo(&c->ico);

	if (cmd->output.fn == NULL || cmd->output.fn == c->outfn.ptr) {
		ffstr nm;
		ffstr_setz(&nm, cmd->input.fn);
		ffpath_split3(nm.ptr, nm.len, NULL, &nm, NULL);
		c->infn.len = 0;
		ffstr_catfmt(&c->infn, "%S-%u.%s"
			, &nm, c->idx, (iconfmt == FFICO_PNG) ? "png" : "bmp");
		if (FCOM_DATA != (r = fn_out(cmd, (ffstr*)&c->infn, &c->outfn)))
			return r;
		cmd->output.fn = c->outfn.ptr;
	}

	c->dsize = (uint)-1;
	if (iconfmt == FFICO_BMP) {
		cmd->pic.width = f->width;
		cmd->pic.height = f->height;
		cmd->pic.format = f->format;
		cmd->bmp_input_reverse = 1;
		com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, "pic.bmp-out");
		c->skip = ffico_bmphdr_size(&c->ico);
		c->dsize = f->width * f->height * ffpic_bits(f->format) / 8;
	}

	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
	return FCOM_DONE;
}

static int icoi_process(void *p, fcom_cmd *cmd)
{
	enum { R_DEF, R_NEXT };
	struct icoread *c = p;
	int r;

	if (cmd->flags & FCOM_CMD_FWD)
		ffico_input(&c->ico, cmd->in.ptr, cmd->in.len);

	for (;;) {

	switch (c->state) {
	case R_DEF:
		break;
	case R_NEXT:
		if (c->idx == c->num)
			return FCOM_FIN;

		if (cmd->members.len != 0) {
			char buf[64];
			size_t n = ffs_fromint(c->idx, buf, sizeof(buf), 0);
			if (0 > ffs_findarrz((void*)cmd->members.ptr, cmd->members.len, buf, n)) {
				c->idx++;
				continue;
			}
		}

		ffico_readfile(&c->ico, c->idx);
		c->idx++;
		c->state = R_DEF;
		break;
	}

	r = ffico_read(&c->ico);
	switch (r) {

	case FFICO_MORE:
		return FCOM_MORE;

	case FFICO_FILEINFO: {
		c->num++;
		const struct ffico_file *f = ffico_fileinfo(&c->ico);
		fcom_verblog(FILT_NAME, "icon #%u: dimensions:%ux%u  bpp:%u  size:%u  offset:%xu"
			, c->num, f->width, f->height, ffpic_bits(f->format), f->size, f->offset);
		break;
	}

	case FFICO_HDR:
		if (cmd->show)
			return FCOM_FIN;
		c->state = R_NEXT;
		continue;

	case FFICO_FILEFORMAT:
		r = icoi_init_chain(c, cmd, ffico_fileformat(&c->ico));
		if (r != FCOM_DONE)
			return r;
		break;

	case FFICO_DATA:
		if (c->dsize == 0) {
			c->state = R_NEXT;
			return FCOM_NEXTDONE;
		}
		cmd->out = ffico_data(&c->ico);
		if (c->skip != 0) {
			size_t n = ffmin(c->skip, cmd->out.len);
			ffstr_shift(&cmd->out, n);
			c->skip -= n;
		}
		size_t n = ffmin(c->dsize, cmd->out.len);
		cmd->out.len = n;
		c->dsize -= n;
		return FCOM_DATA;

	case FFICO_FILEDONE:
		c->state = R_NEXT;
		return FCOM_NEXTDONE;

	case FFICO_SEEK:
		fcom_cmd_seek(cmd, ffico_offset(&c->ico));
		return FCOM_MORE;

	case FFICO_ERR:
		fcom_errlog_ctx(cmd, FILT_NAME, "ffico_read(): (%u) %s"
			, c->ico.err, "");
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME
