/** .jpg read/write.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/pic/pic.h>
#include <FF/pic/jpeg.h>


extern const fcom_core *core;
extern const fcom_command *com;


// JPEG INPUT
static void* jpgi_open(fcom_cmd *cmd);
static void jpgi_close(void *p, fcom_cmd *cmd);
static int jpgi_process(void *p, fcom_cmd *cmd);
const fcom_filter jpgi_filt = { &jpgi_open, &jpgi_close, &jpgi_process };

// JPEG OUTPUT
struct jpgo_conf;
struct jpgo_conf *jpgo_conf;
int jpgo_config(ffpars_ctx *ctx);
static void* jpgo_open(fcom_cmd *cmd);
static void jpgo_close(void *p, fcom_cmd *cmd);
static int jpgo_process(void *p, fcom_cmd *cmd);
const fcom_filter jpgo_filt = { &jpgo_open, &jpgo_close, &jpgo_process };


#define FILT_NAME  "pic.jpg-in"

struct jpgi {
	ffjpeg jpeg;
};

static void* jpgi_open(fcom_cmd *cmd)
{
	struct jpgi *j;
	if (NULL == (j = ffmem_new(struct jpgi)))
		return FCOM_OPEN_SYSERR;
	ffjpeg_open(&j->jpeg);
	return j;
}

static void jpgi_close(void *p, fcom_cmd *cmd)
{
	struct jpgi *j = p;
	ffjpeg_close(&j->jpeg);
	ffmem_free(j);
}

static int jpgi_process(void *p, fcom_cmd *cmd)
{
	struct jpgi *j = p;

	if (cmd->flags & FCOM_CMD_FWD) {
		ffjpeg_input(&j->jpeg, cmd->in.ptr, cmd->in.len);
	}

	for (;;) {
	int r = ffjpeg_read(&j->jpeg);

	switch (r) {
	case FFJPEG_MORE:
		return FCOM_MORE;

	case FFJPEG_HDR:
		fcom_dbglog(0, FILT_NAME, "%u/%u %s"
			, j->jpeg.info.width, j->jpeg.info.height, ffpic_fmtstr(j->jpeg.info.format));

		if (cmd->show)
			return FCOM_OUTPUTDONE;

		cmd->pic.width = j->jpeg.info.width;
		cmd->pic.height = j->jpeg.info.height;
		cmd->pic.format = j->jpeg.info.format;
		break;

	case FFJPEG_DATA:
		goto data;

	case FFJPEG_DONE:
		return FCOM_OUTPUTDONE;

	case FFJPEG_ERR:
		fcom_errlog(FILT_NAME, "ffjpeg_read(): %s", ffjpeg_errstr(&j->jpeg));
		return FCOM_ERR;
	}
	}

data:
	fcom_dbglog(0, FILT_NAME, "line %u/%u"
		, (int)ffjpeg_line(&j->jpeg), j->jpeg.info.height);
	cmd->out = ffjpeg_output(&j->jpeg);
	return FCOM_DATA;
}

#undef FILT_NAME


#define FILT_NAME  "pic.jpg-out"

struct jpgo_conf {
	uint quality;
};

#define OFF(member)  FFPARS_DSTOFF(struct jpgo_conf, member)
static const ffpars_arg jpgo_conf_args[] = {
	{ "quality",	FFPARS_TINT8,  OFF(quality) },
};
#undef OFF

int jpgo_config(ffpars_ctx *ctx)
{
	if (NULL == (jpgo_conf = ffmem_new(struct jpgo_conf)))
		return -1;
	jpgo_conf->quality = 90;
	ffpars_setargs(ctx, jpgo_conf, jpgo_conf_args, FFCNT(jpgo_conf_args));
	return 0;
}

struct jpgo {
	uint state;
	ffjpeg_cook jpeg;
};

static void* jpgo_open(fcom_cmd *cmd)
{
	struct jpgo *j;
	if (NULL == (j = ffmem_new(struct jpgo)))
		return FCOM_OPEN_SYSERR;
	return j;
}

static void jpgo_close(void *p, fcom_cmd *cmd)
{
	struct jpgo *j = p;
	ffjpeg_wclose(&j->jpeg);
	ffmem_free(j);
}

static int jpgo_process(void *p, fcom_cmd *cmd)
{
	struct jpgo *j = p;
	int r;
	enum { W_FMT, W_FMT2, W_DATA };

	if (cmd->flags & FCOM_CMD_FWD) {
		ffjpeg_winput(&j->jpeg, cmd->in.ptr, cmd->in.len);
	}

	switch (j->state) {
	case W_FMT:
	case W_FMT2: {
		ffpic_info info;
		info.width = cmd->pic.width;
		info.height = cmd->pic.height;
		info.format = cmd->pic.format;
		r = ffjpeg_create(&j->jpeg, &info);
		if (r == FFJPEG_EFMT && j->state == W_FMT) {
			cmd->pic.out_format = info.format;
			com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, "pic.pxconv");
			j->state = W_FMT2;
			cmd->out = cmd->in;
			return FCOM_BACK;
		} else if (r != 0) {
			fcom_errlog(FILT_NAME, "ffjpeg_create()", 0);
			return FCOM_ERR;
		}

		j->jpeg.info.quality = (cmd->jpeg_quality != 255) ? cmd->jpeg_quality : jpgo_conf->quality;
		j->state = W_DATA;
		break;
	}

	case W_DATA:
		break;
	}

	for (;;) {
	r = ffjpeg_write(&j->jpeg);

	switch (r) {
	case FFJPEG_DATA:
		goto data;

	case FFJPEG_MORE:
		return FCOM_MORE;

	case FFJPEG_DONE:
		return FCOM_DONE;

	case FFJPEG_ERR:
		fcom_errlog(FILT_NAME, "ffjpeg_write(): %s", ffjpeg_werrstr(&j->jpeg));
		return FCOM_ERR;
	}
	}

data:
	cmd->out = ffjpeg_woutput(&j->jpeg);
	return FCOM_DATA;
}

#undef FILT_NAME
