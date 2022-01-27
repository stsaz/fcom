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
static int pic_conf(const char *name, ffconf_scheme *cs);
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

#include <pic/conv.h>
#include <pic/crop.h>

extern const fcom_filter bmpi_filt;
extern const fcom_filter bmpo_filt;

extern const fcom_filter jpgi_filt;
extern const fcom_filter jpgo_filt;
extern struct jpgo_conf *jpgo_conf;
extern int jpgo_config(ffconf_scheme *cs);

extern const fcom_filter pngi_filt;
extern const fcom_filter pngo_filt;
extern struct pngo_conf *pngo_conf;
extern int pngo_config(ffconf_scheme *cs);

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

static int pic_conf(const char *name, ffconf_scheme *cs)
{
	if (ffsz_eq(name, "jpg-out"))
		return jpgo_config(cs);
	else if (ffsz_eq(name, "png-out"))
		return pngo_config(cs);
	return 1;
}


#define FILT_NAME  "pic.conv"

struct piconv {
	fcom_cmd *cmd;
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

struct pictask {
	struct piconv *c;
	char *ifn, *ofn;
};

void pictask_free(struct pictask *t)
{
	ffmem_free(t->ifn);
	ffmem_free(t->ofn);
	ffmem_free(t);
}

static void picconv_task_done(fcom_cmd *cmd, uint sig, void *param)
{
	struct pictask *t = param;
	struct piconv *c = t->c;

	if (cmd->del_source && !cmd->err) {
		char *newfn = ffsz_allocfmt("%s.deleted", cmd->input.fn);
		if (0 != fffile_rename(cmd->input.fn, newfn))
			fcom_syserrlog(FILT_NAME, "file rename: %s -> %s", cmd->input.fn, newfn);
		ffmem_free(newfn);
	}

	pictask_free(t);

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
	struct pictask *t = ffmem_new(struct pictask);
	ffarr buf = {};
	const char *ofn;
	void *nc;
	int r, ii, oi;
	ffstr idir, iname, iext;
	ffstr odir, oname, oext;
	ffstr_setz(&iname, ifn);
	ffpath_split3(iname.ptr, iname.len, &idir, &iname, &iext);

	if (0 > (ii = ffcharr_findsorted(exts, FFCNT(exts), sizeof(exts[0]), iext.ptr, iext.len))) {
		fcom_errlog(FILT_NAME, "unknown picture file extension .%S", &iext);
		r = FCOM_ERR;
		goto err;
	}

	fcom_cmd ncmd = {};
	fcom_cmd_set(&ncmd, cmd);
	ncmd.name = "pic.conv-task";
	ncmd.flags = FCOM_CMD_EMPTY | FCOM_CMD_INTENSE;
	t->ifn = ffsz_dup(ifn);
	ncmd.input.fn = t->ifn;

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
		if (0 != ffpath_makefn_out(&buf, &idir, &iname, &odir, &oext)) {
			r = FCOM_SYSERR;
			goto err;
		}
		t->ofn = buf.ptr;
		ffarr_null(&buf);
		ofn = t->ofn;
	}
	ncmd.output.fn = ofn;
	ncmd.out_fn_copy = 1;

	if (NULL == (nc = com->create(&ncmd))) {
		r = FCOM_ERR;
		goto err;
	}

	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(&ncmd));
	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, ifilters[ii]);

	if (cmd->crop.width != 0 || cmd->crop.height != 0)
		com->ctrl(nc, FCOM_CMD_FILTADD_LAST, "pic.crop");

	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, ofilters[oi]);
	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(&ncmd));
	t->c = c;
	com->fcom_cmd_monitor_func(nc, picconv_task_done, t);
	ffatom_inc(&c->nsubtasks);
	com->ctrl(nc, FCOM_CMD_RUNASYNC);
	return FCOM_MORE;

err:
	ffarr_free(&buf);
	pictask_free(t);
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

		if (NULL == (ifn = com->arg_next(cmd, FCOM_CMD_ARG_FILE))) {
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
