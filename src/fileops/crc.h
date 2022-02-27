/** fcom: compute CRC
2021, Simon Zolin
*/

/** Fast CRC32 implementation using 8k table. */
extern uint crc32(const void *buf, size_t size, uint crc);

#define FILT_NAME  "f-crc"

struct f_crc {
	uint state;
	uint cur;
};

static void* f_crc_open(fcom_cmd *cmd)
{
	struct f_crc *c;
	if (NULL == (c = ffmem_new(struct f_crc)))
		return NULL;
	return c;
}

static void f_crc_close(void *p, fcom_cmd *cmd)
{
	struct f_crc *c = p;
	ffmem_free(c);
}

static int f_crc_process(void *p, fcom_cmd *cmd)
{
	enum { I_NEXTFILE, I_DATA, };
	struct f_crc *c = p;

	switch (c->state) {
	case I_NEXTFILE:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, FCOM_CMD_ARG_FILE)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		c->state = I_DATA;
		c->cur = 0;
		return FCOM_MORE;

	case I_DATA:
		break;
	}

	c->cur = crc32((void*)cmd->in.ptr, cmd->in.len, c->cur);

	if (cmd->in_last) {
		fcom_infolog(FILT_NAME, "%s: CRC32:%xu"
			, cmd->input.fn, c->cur);
		c->state = I_NEXTFILE;
	}

	return FCOM_MORE;
}

#undef FILT_NAME

static const fcom_filter f_crc_filt = { &f_crc_open, &f_crc_close, &f_crc_process };
