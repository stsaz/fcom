/** fcom: convert pixels
2021, Simon Zolin
*/

struct conv {
	uint state;
	uint in_fmt;
	uint out_fmt;
	uint in_size;
	uint out_size;
	ffarr out;
	ffstr in;
};

static void* pxconv_open(fcom_cmd *cmd)
{
	struct conv *c;
	if (NULL == (c = ffmem_new(struct conv)))
		return FCOM_OPEN_SYSERR;

	return c;
}

static void pxconv_close(void *p, fcom_cmd *cmd)
{
	struct conv *c = p;
	ffarr_free(&c->out);
	ffmem_free(c);
}

static int pxconv_process(void *p, fcom_cmd *cmd)
{
	struct conv *c = p;
	uint pixels;
	enum { CONV_PREP, CONV_PROC };

	if (cmd->flags & FCOM_CMD_FWD) {
		c->in = cmd->in;
	}

	switch (c->state) {
	case CONV_PREP: {
		uint linesize;
		c->in_fmt = cmd->pic.format;
		c->in_size = ffpic_bits(c->in_fmt) / 8;
		c->out_fmt = cmd->pic.out_format;
		c->out_size = ffpic_bits(c->out_fmt) / 8;
		cmd->pic.format = c->out_fmt;
		linesize = cmd->pic.width * c->out_size;

		if (0 != ffpic_convert(c->in_fmt, NULL, c->out_fmt, NULL, 0)) {
			fcom_errlog("pic.pxconv", "unsupported conversion: %s -> %s"
				, ffpic_fmtstr(c->in_fmt), ffpic_fmtstr(c->out_fmt));
			return FCOM_ERR;
		}

		fcom_dbglog(0, "pic.pxconv", "conversion: %s -> %s"
			, ffpic_fmtstr(c->in_fmt), ffpic_fmtstr(c->out_fmt));

		if (NULL == ffarr_alloc(&c->out, linesize))
			return FCOM_SYSERR;
		c->state = CONV_PROC;
		break;
	}

	case CONV_PROC:
		break;
	}

	if (c->in.len == 0) {
		if (cmd->flags & FCOM_CMD_FIRST)
			return FCOM_DONE;
		return FCOM_MORE;
	}

	pixels = ffmin(c->in.len / c->in_size, c->out.cap / c->out_size);
	ffpic_convert(c->in_fmt, c->in.ptr, c->out_fmt, c->out.ptr, pixels);

	ffstr_shift(&c->in, pixels * c->in_size);
	ffstr_set(&cmd->out, c->out.ptr, pixels * c->out_size);
	return FCOM_DATA;
}

static const fcom_filter pxconv_filt = { &pxconv_open, &pxconv_close, &pxconv_process };
