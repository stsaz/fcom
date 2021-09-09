/** fcom: crop picture
2021, Simon Zolin
*/

#define FILT_NAME  "pic.crop"

struct piccrop {
	uint height;
};

static void* piccrop_open(fcom_cmd *cmd)
{
	struct piccrop *c = ffmem_new(struct piccrop);
	if (c == NULL)
		return FCOM_OPEN_SYSERR;
	if (cmd->crop.width != 0)
		cmd->pic.width = cmd->crop.width;
	if (cmd->crop.height != 0)
		cmd->pic.height = cmd->crop.height;
	return c;
}

static void piccrop_close(void *p, fcom_cmd *cmd)
{
	struct piccrop *c = p;
	ffmem_free(c);
}

static int piccrop_process(void *p, fcom_cmd *cmd)
{
	struct piccrop *c = p;

	if (!(cmd->flags & FCOM_CMD_FWD)) {
		if (cmd->flags & FCOM_CMD_FIRST)
			return FCOM_OUTPUTDONE;
		if (c->height == cmd->crop.height)
			return FCOM_OUTPUTDONE;
		return FCOM_MORE;
	}

	if (cmd->in.len == 0)
		return FCOM_OUTPUTDONE;

	ffstr d;
	if (cmd->crop.width != 0) {
		if (0 != ffpic_cut(cmd->pic.format, cmd->in.ptr, cmd->in.len, 0, cmd->crop.width, &d)) {
			fcom_errlog(FILT_NAME, "ffpic_cut() failed", 0);
			return FCOM_ERR;
		}
		cmd->out = d;
	} else
		cmd->out = cmd->in;
	c->height++;
	fcom_dbglog(0, FILT_NAME, "%u/%u", c->height, cmd->crop.height);
	return FCOM_DATA;
}

#undef FILT_NAME

static const fcom_filter piccrop_filt = { &piccrop_open, &piccrop_close, &piccrop_process };
