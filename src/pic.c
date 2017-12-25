/** Pictures processing.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/pic/pic.h>
#include <FF/pic/bmp.h>
#include <FF/pic/jpeg.h>
#include <FF/path.h>


static const fcom_core *core;
static const fcom_command *com;

// MODULE
static int pic_sig(uint signo);
static const void* pic_iface(const char *name);
static int pic_conf(const char *name, ffpars_ctx *ctx);
static const fcom_mod pic_mod = {
	.sig = &pic_sig, .iface = &pic_iface, .conf = &pic_conf,
	.ver = FCOM_VER,
	.name = "Pictures processing", .desc = "Convert pictures: .bmp, .jpg, .png",
};

// PICTURE CONVERTOR
static void* piconv_open(fcom_cmd *cmd);
static void piconv_close(void *p, fcom_cmd *cmd);
static int piconv_process(void *p, fcom_cmd *cmd);
static const fcom_filter piconv_filt = { &piconv_open, &piconv_close, &piconv_process };

// PIXELS CONVERTOR
static void* pxconv_open(fcom_cmd *cmd);
static void pxconv_close(void *p, fcom_cmd *cmd);
static int pxconv_process(void *p, fcom_cmd *cmd);
static const fcom_filter pxconv_filt = { &pxconv_open, &pxconv_close, &pxconv_process };

// BMP INPUT
static void* bmpi_open(fcom_cmd *cmd);
static void bmpi_close(void *p, fcom_cmd *cmd);
static int bmpi_process(void *p, fcom_cmd *cmd);
static const fcom_filter bmpi_filt = { &bmpi_open, &bmpi_close, &bmpi_process };

// BMP OUTPUT
static void* bmpo_open(fcom_cmd *cmd);
static void bmpo_close(void *p, fcom_cmd *cmd);
static int bmpo_process(void *p, fcom_cmd *cmd);
static const fcom_filter bmpo_filt = { &bmpo_open, &bmpo_close, &bmpo_process };

// JPEG INPUT
static void* jpgi_open(fcom_cmd *cmd);
static void jpgi_close(void *p, fcom_cmd *cmd);
static int jpgi_process(void *p, fcom_cmd *cmd);
static const fcom_filter jpgi_filt = { &jpgi_open, &jpgi_close, &jpgi_process };

// JPEG OUTPUT
struct jpgo_conf;
static struct jpgo_conf *jpgo_conf;
static int jpgo_config(ffpars_ctx *ctx);
static void* jpgo_open(fcom_cmd *cmd);
static void jpgo_close(void *p, fcom_cmd *cmd);
static int jpgo_process(void *p, fcom_cmd *cmd);
static const fcom_filter jpgo_filt = { &jpgo_open, &jpgo_close, &jpgo_process };


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &pic_mod;
}

struct cmd {
	const char *name;
	const char *mod;
	const fcom_filter *iface;
};
static const struct cmd commands[] = {
	{ "pic-convert", "pic.conv", NULL },
};

static const struct cmd filters[] = {
	{ "conv", NULL, &piconv_filt },
	{ "pxconv", NULL, &pxconv_filt },
	{ "bmp-in", NULL, &bmpi_filt },
	{ "bmp-out", NULL, &bmpo_filt },
	{ "jpg-in", NULL, &jpgi_filt },
	{ "jpg-out", NULL, &jpgo_filt },
};

static int pic_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		ffmem_init();
		com = core->iface("core.com");
		const struct cmd *c;
		FFARR_WALKNT(commands, FFCNT(commands), c, struct cmd) {
			if (0 != com->reg(c->name, c->mod))
				return -1;
		}
		break;
	}
	case FCOM_SIGFREE:
		ffmem_safefree(jpgo_conf);
		break;
	}
	return 0;
}

static const void* pic_iface(const char *name)
{
	const struct cmd *cmd;
	FFARRS_FOREACH(filters, cmd) {
		if (ffsz_eq(name, cmd->name))
			return cmd->iface;
	}
	return NULL;
}

static int pic_conf(const char *name, ffpars_ctx *ctx)
{
	if (ffsz_eq(name, "jpg-out"))
		return jpgo_config(ctx);
	return 1;
}


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

static int jpgo_config(ffpars_ctx *ctx)
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


#define FILT_NAME  "pic.conv"

struct piconv {
	ffarr fn;
	const char *output;
};

static void* piconv_open(fcom_cmd *cmd)
{
	struct piconv *c;
	if (NULL == (c = ffmem_new(struct piconv)))
		return FCOM_OPEN_SYSERR;
	return c;
}

static void piconv_close(void *p, fcom_cmd *cmd)
{
	struct piconv *c = p;
	ffarr_free(&c->fn);
	ffmem_free(c);
}

static const char exts[][8] = {
	"bmp",
	"jpeg",
	"jpg",
};
static const char *const ifilters[] = {
	"pic.bmp-in",
	"pic.jpg-in",
	"pic.jpg-in",
};
static const char *const ofilters[] = {
	"pic.bmp-out",
	"pic.jpg-out",
	"pic.jpg-out",
};

/** The same as ffpath_split3() except that for fullname="path/.ext" ".ext" is treated as extension. */
static void path_split3(const char *fullname, size_t len, ffstr *path, ffstr *name, ffstr *ext)
{
	ffpath_split2(fullname, len, path, name);
	char *dot = ffs_rfind(name->ptr, name->len, '.');
	ffs_split2(name->ptr, name->len, dot, name, ext);
}

/** Get next input file, excluding directories.
Return NULL if no more files. */
static const char* next_file(fcom_cmd *cmd)
{
	const char *fn;
	fffileinfo fi;
	for (;;) {

		if (NULL == (fn = com->arg_next(cmd, 0)))
			return NULL;

		if (0 == fffile_infofn(fn, &fi) && fffile_isdir(fffile_infoattr(&fi)))
			continue;

		return fn;
	}
}

/** Create filters chain for picture conversion.
chain: f.in -> p.in -> p.out -> f.out */
static int piconv_process(void *p, fcom_cmd *cmd)
{
	struct piconv *c = p;

	if (cmd->output.fn == c->fn.ptr) {
		cmd->output.fn = c->output;
	}

	if (NULL == (cmd->input.fn = next_file(cmd)))
		return FCOM_DONE;

	int r;
	size_t prev;
	ffstr idir, iname, iext;
	ffstr odir, oname, oext;
	ffstr_setz(&iname, cmd->input.fn);
	ffpath_split3(iname.ptr, iname.len, &idir, &iname, &iext);

	if (0 > (r = ffcharr_findsorted(exts, FFCNT(exts), sizeof(exts[0]), iext.ptr, iext.len))) {
		fcom_errlog(FILT_NAME, "unknown picture file extension .%S", &iext);
		return FCOM_ERR;
	}

	prev = com->ctrl(cmd, FCOM_CMD_FILTADD, FCOM_CMD_FILT_IN(cmd));

	prev = com->ctrl(cmd, FCOM_CMD_FILTADD_AFTER, ifilters[r], prev);

	if (cmd->output.fn == NULL) {
		fcom_errlog(FILT_NAME, "output file isn't set", 0);
		return FCOM_ERR;
	}
	ffstr_setz(&oname, cmd->output.fn);
	path_split3(oname.ptr, oname.len, &odir, &oname, &oext);

	if (0 > (r = ffcharr_findsorted(exts, FFCNT(exts), sizeof(exts[0]), oext.ptr, oext.len))) {
		fcom_errlog(FILT_NAME, "unknown picture file extension .%S", &oext);
		return FCOM_ERR;
	}

	if (oname.len == 0) {
		// out_dir/in_dir/in_name.out_ext
		if (0 == ffstr_catfmt(&c->fn, "%S/%S/%S.%S%Z"
			, &odir, &idir, &iname, &oext))
			return FCOM_SYSERR;
		c->fn.len = 0;
		c->output = cmd->output.fn;
		cmd->output.fn = c->fn.ptr;
		if (odir.len == 0)
			cmd->output.fn += FFSLEN("/");
	}

	prev = com->ctrl(cmd, FCOM_CMD_FILTADD_AFTER, ofilters[r], prev);

	prev = com->ctrl(cmd, FCOM_CMD_FILTADD_AFTER, FCOM_CMD_FILT_OUT(cmd), prev);

	return FCOM_NEXTDONE;
}

#undef FILT_NAME
