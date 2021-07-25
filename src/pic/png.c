/** .png read/write.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/pic/pic.h>
#include <FF/pic/png.h>


extern const fcom_core *core;
extern const fcom_command *com;


// PNG INPUT
static void* pngi_open(fcom_cmd *cmd);
static void pngi_close(void *_p, fcom_cmd *cmd);
static int pngi_process(void *_p, fcom_cmd *cmd);
const fcom_filter pngi_filt = { &pngi_open, &pngi_close, &pngi_process };

// PNG OUTPUT
struct pngo_conf;
struct pngo_conf *pngo_conf;
static void* pngo_open(fcom_cmd *cmd);
static void pngo_close(void *_p, fcom_cmd *cmd);
static int pngo_process(void *_p, fcom_cmd *cmd);
const fcom_filter pngo_filt = { &pngo_open, &pngo_close, &pngo_process };


#define FILT_NAME  "pic.png-in"

struct pngi {
	uint state;
	ffpng png;
};

static void* pngi_open(fcom_cmd *cmd)
{
	struct pngi *p;
	if (NULL == (p = ffmem_tcalloc1(struct pngi)))
		return NULL;
	ffpng_open(&p->png);
	p->png.info.total_size = cmd->input.size;
	return p;
}

static void pngi_close(void *_p, fcom_cmd *cmd)
{
	struct pngi *p = _p;
	ffpng_close(&p->png);
	ffmem_free(p);
}

static int pngi_process(void *_p, fcom_cmd *cmd)
{
	struct pngi *p = _p;
	int r;

	if (cmd->flags & FCOM_CMD_FWD) {
		ffpng_input(&p->png, cmd->in.ptr, cmd->in.len);
	}

	for (;;) {
	r = ffpng_read(&p->png);

	switch (r) {
	case FFPNG_MORE:
		p->state = 0;
		return FCOM_MORE;

	case FFPNG_HDR:
		fcom_dbglog(0, FILT_NAME, "%u/%u %s"
			, p->png.info.width, p->png.info.height, ffpic_fmtstr(p->png.info.format));

		cmd->pic.width = p->png.info.width;
		cmd->pic.height = p->png.info.height;
		cmd->pic.format = p->png.info.format;
		break;

	case FFPNG_DATA:
		goto data;

	case FFPNG_DONE:
		return FCOM_OUTPUTDONE;

	case FFPNG_ERR:
		fcom_errlog_ctx(cmd, FILT_NAME, "ffpng_read(): %s", ffpng_errstr(&p->png));
		return FCOM_ERR;
	}
	}

data:
	cmd->out = ffpng_output(&p->png);
	return FCOM_DATA;
}

#undef FILT_NAME


#define FILT_NAME  "pic.png-out"

struct pngo_conf {
	uint compression;
};

#define OFF(member)  FF_OFF(struct pngo_conf, member)
static const ffconf_arg pngo_conf_args[] = {
	{ "compression",	FFCONF_TINT32,  OFF(compression) },
	{}
};
#undef OFF

int pngo_config(ffconf_scheme *cs)
{
	if (NULL == (pngo_conf = ffmem_new(struct pngo_conf)))
		return -1;
	pngo_conf->compression = 9;
	ffconf_scheme_addctx(cs, pngo_conf_args, pngo_conf);
	return 0;
}

struct pngo {
	uint state;
	ffpng_cook png;
};

static void* pngo_open(fcom_cmd *cmd)
{
	struct pngo *p;
	if (NULL == (p = ffmem_new(struct pngo)))
		return FCOM_OPEN_SYSERR;
	return p;
}

static void pngo_close(void *_p, fcom_cmd *cmd)
{
	struct pngo *p = _p;
	ffpng_wclose(&p->png);
	ffmem_free(p);
}

static int pngo_process(void *_p, fcom_cmd *cmd)
{
	struct pngo *p = _p;
	int r;
	enum { W_FMT, W_FMT2, W_DATA };

	if (cmd->flags & FCOM_CMD_FWD) {
		ffpng_winput(&p->png, cmd->in.ptr, cmd->in.len);
	}

	switch (p->state) {
	case W_FMT:
	case W_FMT2: {
		ffpic_info info;
		info.width = cmd->pic.width;
		info.height = cmd->pic.height;
		info.format = cmd->pic.format;
		r = ffpng_create(&p->png, &info);
		if (r == FFPNG_EFMT && p->state == W_FMT) {
			cmd->pic.out_format = info.format;
			com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, "pic.pxconv");
			p->state = W_FMT2;
			cmd->out = cmd->in;
			return FCOM_BACK;
		} else if (r != 0) {
			fcom_errlog(FILT_NAME, "ffpng_create()", 0);
			return FCOM_ERR;
		}

		p->png.info.complevel = (cmd->png_comp != 255) ? cmd->png_comp : pngo_conf->compression;
		p->state = W_DATA;
		break;
	}

	case W_DATA:
		break;
	}

	for (;;) {
	r = ffpng_write(&p->png);

	switch (r) {
	case FFPNG_DATA:
		goto data;

	case FFPNG_MORE:
		return FCOM_MORE;

	case FFPNG_DONE:
		return FCOM_DONE;

	case FFPNG_ERR:
		fcom_errlog(FILT_NAME, "ffpng_write(): %s", ffpng_werrstr(&p->png));
		return FCOM_ERR;
	}
	}

data:
	cmd->out = ffpng_woutput(&p->png);
	return FCOM_DATA;
}

#undef FILT_NAME
