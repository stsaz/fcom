/** .bmp read/write.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/pic/pic.h>
#include <FF/pic/bmp.h>


extern const fcom_core *core;
extern const fcom_command *com;


// BMP INPUT
static void* bmpi_open(fcom_cmd *cmd);
static void bmpi_close(void *p, fcom_cmd *cmd);
static int bmpi_process(void *p, fcom_cmd *cmd);
const fcom_filter bmpi_filt = { &bmpi_open, &bmpi_close, &bmpi_process };

// BMP OUTPUT
static void* bmpo_open(fcom_cmd *cmd);
static void bmpo_close(void *p, fcom_cmd *cmd);
static int bmpo_process(void *p, fcom_cmd *cmd);
const fcom_filter bmpo_filt = { &bmpo_open, &bmpo_close, &bmpo_process };


#define FILT_NAME  "pic.bmp-in"

struct bmpi {
	ffbmp bmp;
};

static void* bmpi_open(fcom_cmd *cmd)
{
	struct bmpi *b;
	if (NULL == (b = ffmem_new(struct bmpi)))
		return FCOM_OPEN_SYSERR;
	ffbmp_open(&b->bmp);
	cmd->in_backward = 1;
	return b;
}

static void bmpi_close(void *p, fcom_cmd *cmd)
{
	struct bmpi *b = p;
	ffbmp_close(&b->bmp);
	ffmem_free(b);
}

static int bmpi_process(void *p, fcom_cmd *cmd)
{
	struct bmpi *b = p;

	if (cmd->flags & FCOM_CMD_FWD) {
		ffbmp_input(&b->bmp, cmd->in.ptr, cmd->in.len);
	}

	for (;;) {
	int r = ffbmp_read(&b->bmp);

	switch (r) {
	case FFBMP_SEEK:
		fcom_cmd_seek(cmd, ffbmp_seekoff(&b->bmp));
		return FCOM_MORE;

	case FFBMP_MORE:
		return FCOM_MORE;

	case FFBMP_HDR:
		fcom_dbglog(0, FILT_NAME, "%u/%u %s"
			, b->bmp.info.width, b->bmp.info.height, ffpic_fmtstr(b->bmp.info.format));

		if (cmd->show)
			return FCOM_OUTPUTDONE;

		cmd->pic.width = b->bmp.info.width;
		cmd->pic.height = b->bmp.info.height;
		cmd->pic.format = b->bmp.info.format;
		break;

	case FFBMP_DATA:
		goto data;

	case FFBMP_DONE:
		return FCOM_OUTPUTDONE;

	case FFBMP_ERR:
		fcom_errlog(FILT_NAME, "ffbmp_read(): %s", ffbmp_errstr(&b->bmp));
		return FCOM_ERR;
	}
	}

data:
	fcom_dbglog(0, FILT_NAME, "line %u/%u"
		, (int)ffbmp_line(&b->bmp), b->bmp.info.height);
	cmd->out = ffbmp_output(&b->bmp);
	return FCOM_DATA;
}

#undef FILT_NAME


#define FILT_NAME  "pic.bmp-out"

struct bmpo {
	uint state;
	ffbmp_cook bmp;
};

static void* bmpo_open(fcom_cmd *cmd)
{
	struct bmpo *b;
	if (NULL == (b = ffmem_new(struct bmpo)))
		return FCOM_OPEN_SYSERR;
	return b;
}

static void bmpo_close(void *p, fcom_cmd *cmd)
{
	struct bmpo *b = p;
	ffbmp_wclose(&b->bmp);
	ffmem_free(b);
}

static int bmpo_process(void *p, fcom_cmd *cmd)
{
	struct bmpo *b = p;
	int r;
	enum { W_FMT, W_FMT2, W_DATA };

	if (cmd->flags & FCOM_CMD_FWD) {
		ffbmp_winput(&b->bmp, cmd->in.ptr, cmd->in.len);
	}

	switch (b->state) {
	case W_FMT2:
	case W_FMT: {
		ffpic_info info;
		info.width = cmd->pic.width;
		info.height = cmd->pic.height;

		info.format = cmd->pic.format;
		if (cmd->pic_colors == 24)
			info.format = 24 | (cmd->pic.format & _FFPIC_BGR);
		else if (cmd->pic_colors != (byte)(char)-1) {
			fcom_errlog(FILT_NAME, "--colors: unsupported value %u", cmd->pic_colors);
			return FCOM_ERR;
		}

		r = ffbmp_create(&b->bmp, &info);
		if (r == FFBMP_EFMT && b->state == W_FMT) {
			cmd->pic.out_format = info.format;
			com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, "pic.pxconv");
			b->state = W_FMT2;
			cmd->out = cmd->in;
			return FCOM_BACK;
		} else if (r != 0) {
			fcom_errlog(FILT_NAME, "ffbmp_create()", 0);
			return FCOM_ERR;
		}

		if (b->state == W_FMT && cmd->pic_colors != (byte)(char)-1) {
			cmd->pic.out_format = info.format;
			com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, "pic.pxconv");
			cmd->out = cmd->in;
			b->state = W_DATA;
			return FCOM_BACK;
		}

		b->state = W_DATA;
		break;
	}

	case W_DATA:
		break;
	}

	for (;;) {
	r = ffbmp_write(&b->bmp);

	switch (r) {
	case FFBMP_DATA:
		b->bmp.input_reverse = cmd->bmp_input_reverse;
		goto data;

	case FFBMP_MORE:
		return FCOM_MORE;

	case FFBMP_DONE:
		return FCOM_DONE;

	case FFBMP_SEEK:
		fcom_cmd_outseek(cmd, ffbmp_seekoff(&b->bmp));
		break;

	case FFBMP_ERR:
		fcom_errlog(FILT_NAME, "ffbmp_write(): %s", ffbmp_errstr(&b->bmp));
		return FCOM_ERR;
	}
	}

data:
	cmd->out = ffbmp_woutput(&b->bmp);
	return FCOM_DATA;
}

#undef FILT_NAME
