/** Pictures processing.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/pic/pic.h>
#include <FF/path.h>


const fcom_core *core;
const fcom_command *com;

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

// CROP
static void* piccrop_open(fcom_cmd *cmd);
static void piccrop_close(void *p, fcom_cmd *cmd);
static int piccrop_process(void *p, fcom_cmd *cmd);
static const fcom_filter piccrop_filt = { &piccrop_open, &piccrop_close, &piccrop_process };

extern const fcom_filter bmpi_filt;
extern const fcom_filter bmpo_filt;

extern const fcom_filter jpgi_filt;
extern const fcom_filter jpgo_filt;
extern struct jpgo_conf *jpgo_conf;
extern int jpgo_config(ffpars_ctx *ctx);

extern const fcom_filter pngi_filt;
extern const fcom_filter pngo_filt;
extern struct pngo_conf *pngo_conf;
extern int pngo_config(ffpars_ctx *ctx);

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
	{ "crop", NULL, &piccrop_filt },
	{ "bmp-in", NULL, &bmpi_filt },
	{ "bmp-out", NULL, &bmpo_filt },
	{ "jpg-in", NULL, &jpgi_filt },
	{ "jpg-out", NULL, &jpgo_filt },
	{ "png-in", NULL, &pngi_filt },
	{ "png-out", NULL, &pngo_filt },
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
		ffmem_safefree(pngo_conf);
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
	else if (ffsz_eq(name, "png-out"))
		return pngo_config(ctx);
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


#define FILT_NAME  "pic.conv"

struct piconv {
	fcom_cmd *cmd;
	ffarr fn;
	ffatomic nsubtasks;
	uint close :1;
};

static void* piconv_open(fcom_cmd *cmd)
{
	struct piconv *c;
	if (NULL == (c = ffmem_new(struct piconv)))
		return FCOM_OPEN_SYSERR;
	c->cmd = cmd;
	return c;
}

static void piconv_close(void *p, fcom_cmd *cmd)
{
	struct piconv *c = p;

	if (0 != ffatom_get(&c->nsubtasks)) {
		// wait until the last subtask is finished
		c->close = 1;
		return;
	}

	ffarr_free(&c->fn);
	ffmem_free(c);
}

static const char exts[][8] = {
	"bmp",
	"jpeg",
	"jpg",
	"png",
};
static const char *const ifilters[] = {
	"pic.bmp-in",
	"pic.jpg-in",
	"pic.jpg-in",
	"pic.png-in",
};
static const char *const ofilters[] = {
	"pic.bmp-out",
	"pic.jpg-out",
	"pic.jpg-out",
	"pic.png-out",
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

/** Make filename:  ([out_dir/] | [in_dir/]) in_name .out_ext */
static int fn_make(ffarr *fn, ffstr *idir, const ffstr *iname, ffstr *odir, const ffstr *oext)
{
	fn->len = 0;
	if (odir->len != 0) {
		if (NULL == ffarr_append(fn, odir->ptr, odir->len + 1))
			return FCOM_SYSERR;
	} else if (idir->len != 0) {
		idir->len++;
		uint f = 0;
		if (NULL == ffarr_grow(fn, idir->len, 0))
			return FCOM_SYSERR;
		fn->len += ffpath_norm(ffarr_end(fn), ffarr_unused(fn), idir->ptr, idir->len, f | FFPATH_MERGEDOTS);
	}

	if (0 == ffstr_catfmt(fn, "%S.%S%Z", iname, oext))
		return FCOM_SYSERR;

	return 0;
}

static void picconv_task_done(fcom_cmd *cmd, uint sig);
static const struct fcom_cmd_mon picconv_mon_iface = { &picconv_task_done };

static void picconv_task_done(fcom_cmd *cmd, uint sig)
{
	struct piconv *c = (void*)com->ctrl(cmd, FCOM_CMD_UDATA);
	if (0 == ffatom_decret(&c->nsubtasks) && c->close) {
		piconv_close(c, NULL);
		return;
	}

	com->ctrl(c->cmd, FCOM_CMD_RUNASYNC);
}

static void fcom_cmd_set(fcom_cmd *dst, const fcom_cmd *src)
{
	ffmemcpy(dst, src, sizeof(*dst));
	ffstr_null(&dst->in);
	ffstr_null(&dst->out);
}

/** Create a sub-command for picture conversion.
Filter chain: f.in -> p.in -> [filters] -> p.out -> f.out */
static int piconv_process1(struct piconv *c, fcom_cmd *cmd, const char *ifn)
{
	const char *ofn;
	void *nc;
	int r, ii, oi;
	ffstr idir, iname, iext;
	ffstr odir, oname, oext;
	ffstr_setz(&iname, ifn);
	ffpath_split3(iname.ptr, iname.len, &idir, &iname, &iext);

	if (0 > (ii = ffcharr_findsorted(exts, FFCNT(exts), sizeof(exts[0]), iext.ptr, iext.len))) {
		fcom_errlog(FILT_NAME, "unknown picture file extension .%S", &iext);
		return FCOM_ERR;
	}

	fcom_cmd ncmd = {};
	fcom_cmd_set(&ncmd, cmd);
	ncmd.name = "pic.conv-task";
	ncmd.flags = FCOM_CMD_EMPTY | FCOM_CMD_INTENSE;
	ncmd.input.fn = ifn;

	if (cmd->output.fn == NULL) {
		fcom_errlog(FILT_NAME, "output file isn't set", 0);
		r = FCOM_ERR;
		goto err;
	}
	ffstr_setz(&oname, cmd->output.fn);
	path_split3(oname.ptr, oname.len, &odir, &oname, &oext);

	if (0 > (oi = ffcharr_findsorted(exts, FFCNT(exts), sizeof(exts[0]), oext.ptr, oext.len))) {
		fcom_errlog(FILT_NAME, "unknown picture file extension .%S", &oext);
		r = FCOM_ERR;
		goto err;
	}

	ofn = cmd->output.fn;
	if (oname.len == 0) {
		if (0 != fn_make(&c->fn, &idir, &iname, &odir, &oext)) {
			r = FCOM_SYSERR;
			goto err;
		}
		ofn = c->fn.ptr;
	}
	ncmd.output.fn = ofn;
	ncmd.out_fn_copy = 1;

	if (NULL == (nc = com->create(&ncmd)))
		return FCOM_ERR;

	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(&ncmd));
	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, ifilters[ii]);

	if (cmd->crop.width != 0 || cmd->crop.height != 0)
		com->ctrl(nc, FCOM_CMD_FILTADD_LAST, "pic.crop");

	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, ofilters[oi]);
	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(&ncmd));
	com->fcom_cmd_monitor(nc, &picconv_mon_iface);
	com->ctrl(nc, FCOM_CMD_SETUDATA, c);
	ffatom_inc(&c->nsubtasks);
	com->ctrl(nc, FCOM_CMD_RUNASYNC);
	return FCOM_MORE;

err:
	return r;
}

/**
Note: on error core.com module won't wait for the tasks already running -
 they will be forced to stop. */
static int piconv_process(void *p, fcom_cmd *cmd)
{
	struct piconv *c = p;
	const char *ifn;
	int r;

	for (;;) {

		if (NULL == (ifn = next_file(cmd))) {
			if (0 != ffatom_get(&c->nsubtasks))
				return FCOM_ASYNC;
			return FCOM_DONE;
		}

		r = piconv_process1(c, cmd, ifn);
		if (r != FCOM_MORE)
			return r;

		if (0 == core->cmd(FCOM_WORKER_AVAIL))
			break;
	}

	return FCOM_ASYNC;
}

#undef FILT_NAME


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
