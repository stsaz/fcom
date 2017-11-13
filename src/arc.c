/** Archives.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/pack/gz.h>
#include <FF/pack/xz.h>
#include <FF/pack/tar.h>
#include <FF/pack/zip.h>
#include <FF/pack/7z.h>
#include <FF/path.h>
#include <FF/time.h>


static const fcom_core *core;
static const fcom_command *com;

// MODULE
static int arc_sig(uint signo);
static const void* arc_iface(const char *name);
static int arc_conf(const char *name, ffpars_ctx *ctx);
static const fcom_mod arc_mod = {
	.sig = &arc_sig, .iface = &arc_iface, .conf = &arc_conf,
	.ver = FCOM_VER,
	.name = "Archiver", .desc = "Pack/unpack archives .gz, .xz, .tar, .zip, .7z",
};

enum {
	BUFSIZE = 64 * 1024,
};

static int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);

// GZIP
static void* gzip_open(fcom_cmd *cmd);
static void gzip_close(void *p, fcom_cmd *cmd);
static int gzip_process(void *p, fcom_cmd *cmd);
static const fcom_filter gzip_filt = { &gzip_open, &gzip_close, &gzip_process };

// UNGZ
static void* ungz_open(fcom_cmd *cmd);
static void ungz_close(void *p, fcom_cmd *cmd);
static int ungz_process(void *p, fcom_cmd *cmd);
static const fcom_filter ungz_filt = { &ungz_open, &ungz_close, &ungz_process };
static void* ungz1_open(fcom_cmd *cmd);
static void ungz1_close(void *p, fcom_cmd *cmd);
static int ungz1_process(void *p, fcom_cmd *cmd);
static const fcom_filter ungz1_filt = { &ungz1_open, &ungz1_close, &ungz1_process };

// UNXZ
static void* unxz_open(fcom_cmd *cmd);
static void unxz_close(void *p, fcom_cmd *cmd);
static int unxz_process(void *p, fcom_cmd *cmd);
static const fcom_filter unxz_filt = {
	&unxz_open, &unxz_close, &unxz_process,
};
static void* unxz1_open(fcom_cmd *cmd);
static void unxz1_close(void *p, fcom_cmd *cmd);
static int unxz1_process(void *p, fcom_cmd *cmd);
static const fcom_filter unxz1_filt = { &unxz1_open, &unxz1_close, &unxz1_process };

// TAR
static void* tar_open(fcom_cmd *cmd);
static void tar_close(void *p, fcom_cmd *cmd);
static int tar_process(void *p, fcom_cmd *cmd);
static const fcom_filter tar_filt = { &tar_open, &tar_close, &tar_process };

// UNTAR
static void* untar_open(fcom_cmd *cmd);
static void untar_close(void *p, fcom_cmd *cmd);
static int untar_process(void *p, fcom_cmd *cmd);
static const fcom_filter untar_filt = { &untar_open, &untar_close, &untar_process };

struct untar;
static void untar_showinfo(struct untar *t, const fftar_file *f);

// ZIP
static void* zip_open(fcom_cmd *cmd);
static void zip_close(void *p, fcom_cmd *cmd);
static int zip_process(void *p, fcom_cmd *cmd);
static const fcom_filter zip_filt = { &zip_open, &zip_close, &zip_process };

// UNZIP
static void* unzip_open(fcom_cmd *cmd);
static void unzip_close(void *p, fcom_cmd *cmd);
static int unzip_process(void *p, fcom_cmd *cmd);
static const fcom_filter unzip_filt = { &unzip_open, &unzip_close, &unzip_process };

struct unzip;
static void unzip_showinfo(struct unzip *z, const ffzip_file *f);

// UN7Z
static void* un7z_open(fcom_cmd *cmd);
static void un7z_close(void *p, fcom_cmd *cmd);
static int un7z_process(void *p, fcom_cmd *cmd);
static const fcom_filter un7z_filt = { &un7z_open, &un7z_close, &un7z_process };

struct un7z;
static void un7z_showinfo(struct un7z *z, const ff7zfile *f);

// UNPACK
static void* unpack_open(fcom_cmd *cmd);
static void unpack_close(void *p, fcom_cmd *cmd);
static int unpack_process(void *p, fcom_cmd *cmd);
static const fcom_filter unpack_filt = { &unpack_open, &unpack_close, &unpack_process };


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &arc_mod;
}

struct cmd {
	const char *name;
	const char *mod;
	const fcom_filter *iface;
};

static const struct cmd cmds[] = {
	{ "gz", "arc.gz", &gzip_filt },
	{ "ungz", "arc.ungz", &ungz_filt },
	{ "ungz1", NULL, &ungz1_filt },
	{ "unxz", "arc.unxz", &unxz_filt },
	{ "unxz1", NULL, &unxz1_filt },
	{ "tar", "arc.tar", &tar_filt },
	{ "untar", "arc.untar", &untar_filt },
	{ "zip", "arc.zip", &zip_filt },
	{ "unzip", "arc.unzip", &unzip_filt },
	{ "un7z", "arc.un7z", &un7z_filt },
	{ "unpack", "arc.unpack", &unpack_filt },
};

static int arc_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		ffmem_init();
		com = core->iface("core.com");
		const struct cmd *c;
		FFARRS_FOREACH(cmds, c) {
			if (c->mod != NULL && 0 != com->reg(c->name, c->mod))
				return -1;
		}
		break;
	}
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}

static const void* arc_iface(const char *name)
{
	const struct cmd *cmd;
	FFARRS_FOREACH(cmds, cmd) {
		if (ffsz_eq(name, cmd->name))
			return cmd->iface;
	}
	return NULL;
}

static int arc_conf(const char *name, ffpars_ctx *ctx)
{
	return 0;
}


/** Get output filename.  Handle user-defined output directory. */
static int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf)
{
	size_t n;
	char *p, *end;

	n = input->len + 1;
	n += (cmd->outdir != NULL) ? ffsz_len(cmd->outdir) + FFSLEN("/") : 0;
	if (NULL == ffarr_grow(buf, n, 0))
		return FCOM_SYSERR;

	p = buf->ptr;
	end = ffarr_edge(buf);
	if (cmd->outdir) {
		p = ffs_copyz(p, end, cmd->outdir);
		p = ffs_copyc(p, end, '/');
	}
	if (0 == (n = ffpath_norm(p, end - p, input->ptr, input->len, FFPATH_NOWINDOWS | FFPATH_FORCESLASH | FFPATH_TOREL | FFPATH_MERGEDOTS)))
		return FCOM_ERR;

	n = ffpath_makefn_full(p, end - p, p, n, '_');
	p += n;
	*p = '\0';

	return FCOM_DATA;
}


#define FILT_NAME  "arc.gz"

typedef struct gzip {
	uint state;
	ffgz_cook gz;
	ffarr buf;
	ffarr fn;
} gzip;

static void* gzip_open(fcom_cmd *cmd)
{
	gzip *g;
	if (NULL == (g = ffmem_new(gzip)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&g->buf, BUFSIZE)
		|| NULL == ffarr_alloc(&g->fn, 1024)) {
		fcom_syserrlog(FILT_NAME, "%s", ffmem_alloc_S);
		goto err;
	}

	return g;

err:
	gzip_close(g, cmd);
	return FCOM_OPEN_SYSERR;
}

static void gzip_close(void *p, fcom_cmd *cmd)
{
	gzip *g = p;
	ffarr_free(&g->buf);
	ffarr_free(&g->fn);
	ffgz_wclose(&g->gz);
	ffmem_free(g);
}

static int gzip_process(void *p, fcom_cmd *cmd)
{
	gzip *g = p;
	int r;
	enum E { W_NEXT, W_NEWFILE, W_EOF, W_DATA };

	switch ((enum E)g->state) {

	case W_EOF:
		if (!(cmd->flags & FCOM_CMD_FWD))
			return FCOM_MORE;
		FF_ASSERT(cmd->in.len == 0);
		g->state = W_NEXT;
		//fall through

	case W_NEXT:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));

		g->state = W_NEWFILE;
		return FCOM_MORE;

	case W_NEWFILE: {
		if (cmd->output.fn == NULL) {
			ffstr outdir, name;
			ffstr_setz(&name, cmd->input.fn);
			if (cmd->outdir != NULL)
				ffstr_setz(&outdir, cmd->outdir);
			else
				ffstr_setz(&outdir, ".");
			ffpath_split2(name.ptr, name.len, NULL, &name);
			g->fn.len = 0;
			if (0 == ffstr_catfmt(&g->fn, "%S/%S.gz%Z", &outdir, &name))
				return FCOM_SYSERR;
			cmd->output.fn = g->fn.ptr;
		}
		com->ctrl(cmd, FCOM_CMD_FILTADD, FCOM_CMD_FILT_OUT(cmd));

		uint lev = (cmd->deflate_level != 255) ? cmd->deflate_level : 6;
		if (0 != ffgz_winit(&g->gz, lev, 0)) {
			fcom_errlog(FILT_NAME, "%s", ffgz_errstr(&g->gz));
			return FCOM_ERR;
		}

		if (0 != ffgz_wfile(&g->gz, cmd->input.fn, cmd->input.mtime.s)) {
			fcom_errlog(FILT_NAME, "%s", ffgz_errstr(&g->gz));
			return FCOM_ERR;
		}

		g->state = W_DATA;
		//fall through
	}

	case W_DATA:
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD) {
		if (cmd->in_last)
			ffgz_wfinish(&g->gz);
		g->gz.in = cmd->in;
	}

	r = ffgz_write(&g->gz, ffarr_end(&g->buf), ffarr_unused(&g->buf));

	switch (r) {
	case FFGZ_DATA:
		cmd->out = g->gz.out;
		return FCOM_DATA;

	case FFGZ_DONE:
		if (g->gz.insize > (uint)-1)
			fcom_warnlog(FILT_NAME, "truncated input file size", 0);
		fcom_infolog(FILT_NAME, "%U => %U (%u%%)"
			, g->gz.insize, g->gz.outsize, (uint)FFINT_DIVSAFE(g->gz.outsize * 100, g->gz.insize));
		ffgz_wclose(&g->gz);
		ffmem_tzero(&g->gz);
		FF_CMPSET(&cmd->output.fn, g->fn.ptr, NULL);
		g->state = W_EOF;
		return FCOM_NEXTDONE;

	case FFGZ_MORE:
		return FCOM_MORE;

	case FFGZ_ERR:
		fcom_errlog(FILT_NAME, "%s", ffgz_errstr(&g->gz));
		return FCOM_ERR;
	}
	return FCOM_ERR;
}

#undef FILT_NAME


#define FILT_NAME  "arc.ungz"

static void* ungz_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void ungz_close(void *p, fcom_cmd *cmd)
{
}

static int ungz_process(void *p, fcom_cmd *cmd)
{
	if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
		return FCOM_DONE;

	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(cmd));
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, "arc.ungz1");
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
	return FCOM_NEXTDONE;
}

typedef struct ungz {
	uint state;
	ffgz gz;
	ffarr buf;
	ffarr fn;
} ungz;

static void* ungz1_open(fcom_cmd *cmd)
{
	ungz *g;
	if (NULL == (g = ffmem_new(ungz)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&g->buf, BUFSIZE)) {
		ungz_close(g, cmd);
		return FCOM_OPEN_SYSERR;
	}

	return g;
}

static void ungz1_close(void *p, fcom_cmd *cmd)
{
	ungz *g = p;
	ffarr_free(&g->buf);
	ffarr_free(&g->fn);
	ffgz_close(&g->gz);
	ffmem_free(g);
}

static int ungz1_process(void *p, fcom_cmd *cmd)
{
	ungz *g = p;
	int r;
	enum E { R_FIRST, R_INIT, R_DATA, R_EOF, };

	switch ((enum E)g->state) {
	case R_FIRST:
		if (cmd->in.len == 0) {
			g->state = R_INIT;
			return FCOM_MORE;
		}
		//fall through

	case R_INIT:
		cmd->output.mtime = cmd->input.mtime;
		ffgz_init(&g->gz, cmd->input.size);
		g->state = R_DATA;
		break;

	case R_DATA:
		break;

	case R_EOF:
		if (cmd->in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%U", cmd->input.offset);
			return FCOM_ERR;
		}
		return FCOM_DONE;
	}

	if (cmd->flags & FCOM_CMD_FWD)
		g->gz.in = cmd->in;

	for (;;) {

	r = ffgz_read(&g->gz, ffarr_end(&g->buf), ffarr_unused(&g->buf));
	switch (r) {

	case FFGZ_INFO: {
		uint mtime;
		if (0 != (mtime = ffgz_mtime(&g->gz)))
			cmd->output.mtime.s = mtime;

		const char *gzfn = ffgz_fname(&g->gz);
		cmd->output.size = ffgz_size64(&g->gz, cmd->input.size);

		if (cmd->output.fn == NULL) {
			ffstr name;
			if (gzfn == NULL || *gzfn == '\0') {
				// "/path/file.txt.gz" -> "file.txt"
				ffstr_setz(&name, cmd->input.fn);
				ffpath_split3(name.ptr, name.len, NULL, &name, NULL);
			} else {
				ffpath_split2(gzfn, ffsz_len(gzfn), NULL, &name);
			}

			if (FCOM_DATA != (r = fn_out(cmd, &name, &g->fn)))
				return r;
			cmd->output.fn = g->fn.ptr;
		}

		fcom_dbglog(0, FILT_NAME, "info: name:%s  mtime:%u  osize:%u  crc32:%xu"
			, (gzfn != NULL) ? gzfn : "", mtime, ffgz_size(&g->gz), ffgz_crc(&g->gz));
		continue;
	}

	case FFGZ_DATA:
		cmd->out = g->gz.out;
		return FCOM_DATA;

	case FFGZ_DONE:
		fcom_verblog(FILT_NAME, "finished: %U => %U (%u%%)"
			, g->gz.insize, ffgz_size(&g->gz)
			, (int)(g->gz.insize * 100 / ffgz_size(&g->gz)));

		if (g->gz.in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%U", cmd->input.offset);
			return FCOM_ERR;
		}
		FF_CMPSET(&cmd->output.fn, g->fn.ptr, NULL);
		g->state = R_EOF;
		return FCOM_MORE;

	case FFGZ_MORE:
		return FCOM_MORE;

	case FFGZ_SEEK:
		cmd->input.offset = ffgz_offset(&g->gz);
		cmd->in_seek = 1;
		return FCOM_MORE;

	case FFGZ_ERR:
		fcom_errlog(FILT_NAME, "%s  offset:0x%xU", ffgz_errstr(&g->gz), cmd->input.offset);
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME


#define FILT_NAME  "arc.unxz"

static void* unxz_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void unxz_close(void *p, fcom_cmd *cmd)
{
}

static int unxz_process(void *p, fcom_cmd *cmd)
{
	if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
		return FCOM_DONE;

	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(cmd));
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, "arc.unxz1");
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
	return FCOM_NEXTDONE;
}

typedef struct unxz {
	uint state;
	ffxz xz;
	ffarr buf;
	ffarr fn;
} unxz;

enum {
	UNXZ_BUFSIZE = 64 * 1024,
};

static void* unxz1_open(fcom_cmd *cmd)
{
	unxz *x;
	if (NULL == (x = ffmem_new(unxz)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&x->buf, UNXZ_BUFSIZE)) {
		unxz_close(x, cmd);
		return FCOM_OPEN_SYSERR;
	}

	return x;
}

static void unxz1_close(void *p, fcom_cmd *cmd)
{
	unxz *x = p;
	ffarr_free(&x->buf);
	ffarr_free(&x->fn);
	ffxz_close(&x->xz);
	ffmem_free(x);
}

static int unxz1_process(void *p, fcom_cmd *cmd)
{
	unxz *x = p;
	int r;
	enum E { R_FIRST, R_INIT, R_DATA, R_EOF, };

	switch ((enum E)x->state) {
	case R_FIRST:
		if (cmd->in.len == 0) {
			x->state = R_INIT;
			return FCOM_MORE;
		}
		//fall through

	case R_INIT:
		ffxz_init(&x->xz, cmd->input.size);
		cmd->output.mtime = cmd->input.mtime;
		x->state = R_DATA;
		break;

	case R_DATA:
		break;

	case R_EOF:
		if (cmd->in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%U", cmd->input.offset);
			return FCOM_ERR;
		}
		return FCOM_DONE;
	}

	if (cmd->flags & FCOM_CMD_FWD)
		x->xz.in = cmd->in;

	for (;;) {

	r = ffxz_read(&x->xz, ffarr_end(&x->buf), ffarr_unused(&x->buf));
	switch (r) {

	case FFXZ_INFO:
		cmd->output.size = ffxz_size(&x->xz);
		cmd->output.mtime = cmd->input.mtime;
		cmd->output.attr = cmd->input.attr;

		if (cmd->output.fn == NULL) {
			// "/path/file.txt.xz" -> "file.txt"
			ffstr name;
			ffstr_setz(&name, cmd->input.fn);
			ffpath_split3(name.ptr, name.len, NULL, &name, NULL);
			if (FCOM_DATA != (r = fn_out(cmd, &name, &x->fn)))
				return r;
			cmd->output.fn = x->fn.ptr;
		}
		continue;

	case FFXZ_DATA:
		cmd->out = x->xz.out;
		return FCOM_DATA;

	case FFXZ_DONE:
		fcom_verblog(FILT_NAME, "finished: %U => %U (%u%%)"
			, x->xz.insize, x->xz.outsize
			, (int)(x->xz.insize * 100 / x->xz.outsize));

		if (x->xz.in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%U", cmd->input.offset);
			return FCOM_ERR;
		}
		FF_CMPSET(&cmd->output.fn, x->fn.ptr, NULL);
		x->state = R_EOF;
		return FCOM_MORE;

	case FFXZ_MORE:
		return FCOM_MORE;

	case FFXZ_SEEK:
		cmd->input.offset = ffxz_offset(&x->xz);
		cmd->in_seek = 1;
		return FCOM_MORE;

	case FFXZ_ERR:
		fcom_errlog(FILT_NAME, "%s  offset:0x%xU", ffxz_errstr(&x->xz), cmd->input.offset);
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME


#define FILT_NAME  "arc.tar"

typedef struct tar {
	uint state;
	fftar_cook tar;
} tar;

static void* tar_open(fcom_cmd *cmd)
{
	tar *t;
	if (NULL == (t = ffmem_new(tar)))
		return FCOM_OPEN_SYSERR;

	if (0 != fftar_create(&t->tar)) {
		fcom_errlog(FILT_NAME, "%s", fftar_errstr(&t->tar));
		goto end;
	}

	if (cmd->output.fn == NULL) {
		fcom_errlog(FILT_NAME, "Output file name must be specified", 0);
		goto end;
	}

	com->ctrl(cmd, FCOM_CMD_FILTADD, FCOM_CMD_FILT_OUT(cmd));
	return t;

end:
	tar_close(t, cmd);
	return NULL;
}

static void tar_close(void *p, fcom_cmd *cmd)
{
	tar *t = p;
	fftar_wclose(&t->tar);
	ffmem_free(t);
}

static int tar_process(void *p, fcom_cmd *cmd)
{
	tar *t = p;
	int r;
	enum E { W_NEXT, W_NEWFILE, W_DATA, W_EOF };

	switch ((enum E)t->state) {

	case W_EOF:
		FF_ASSERT(cmd->in.len == 0);
		t->state = W_NEXT;
		//fall through

	case W_NEXT:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0))) {
			fftar_wfinish(&t->tar);
			t->state = W_DATA;
			break;
		}
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));

		t->state = W_NEWFILE;
		return FCOM_MORE;

	case W_NEWFILE: {
		fftar_file f = {0};
		f.name = cmd->input.fn;
#ifdef FF_UNIX
		f.mode = cmd->input.attr;
#else
		f.mode = (fffile_isdir(cmd->input.attr)) ? FFUNIX_FILE_DIR | 0755 : 0644;
#endif
		f.size = cmd->input.size;
		f.mtime = cmd->input.mtime;
		if (0 != fftar_newfile(&t->tar, &f)) {
			fcom_errlog(FILT_NAME, "%s", fftar_errstr(&t->tar));
			return FCOM_ERR;
		}
		t->state = W_DATA;
		//fall through
	}

	case W_DATA:
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD) {
		if (cmd->in_last)
			fftar_wfiledone(&t->tar);
		t->tar.in = cmd->in;
	}

	for (;;) {

	r = fftar_write(&t->tar);
	switch (r) {

	case FFTAR_DATA:
		cmd->out = t->tar.out;
		return FCOM_DATA;

	case FFTAR_FILEDONE:
		fcom_verblog(FILT_NAME, "added %s: %U", cmd->input.fn, t->tar.fsize);
		t->state = W_EOF;
		return FCOM_MORE;

	case FFTAR_MORE:
		return FCOM_MORE;

	case FFTAR_DONE:
		return FCOM_DONE;

	case FFTAR_ERR:
		fcom_errlog(FILT_NAME, "%s", fftar_errstr(&t->tar));
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME


#define FILT_NAME  "arc.untar"

typedef struct untar {
	uint state;
	fftar tar;
	ffarr fn;
	uint skipfile :1;
} untar;

static void* untar_open(fcom_cmd *cmd)
{
	untar *t;
	if (NULL == (t = ffmem_new(untar)))
		return FCOM_OPEN_SYSERR;
	if (NULL == ffarr_alloc(&t->fn, 4096)) {
		untar_close(t, cmd);
		return FCOM_OPEN_SYSERR;
	}

	return t;
}

static void untar_close(void *p, fcom_cmd *cmd)
{
	untar *t = p;
	fftar_close(&t->tar);
	ffarr_free(&t->fn);
	ffmem_free(t);
}

static int untar_process(void *p, fcom_cmd *cmd)
{
	untar *t = p;
	int r;
	fftar_file *f;
	enum E { R_FIRST, R_NEXT, R_DATA1, R_DATA, R_EOF, };

	switch ((enum E)t->state) {
	case R_EOF:
		if (cmd->in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%U", cmd->input.offset);
			return FCOM_ERR;
		}
		t->state = R_FIRST;
		//fall through

	case R_FIRST: {
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));

		const char *comp = NULL;
		ffstr ext;
		ffpath_split3(cmd->input.fn, ffsz_len(cmd->input.fn), NULL, NULL, &ext);
		if (ffstr_ieqcz(&ext, "tgz") || ffstr_ieqcz(&ext, "gz"))
			comp = "arc.ungz1";
		else if (ffstr_ieqcz(&ext, "txz") || ffstr_ieqcz(&ext, "xz"))
			comp = "arc.unxz1";
		if (comp != NULL) {
			com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, comp);
			FF_CMPSET(&cmd->output.fn, NULL, (void*)1); //decompression filters won't set output filename
		}

		fftar_init(&t->tar);
		t->state = R_DATA1;
		return FCOM_MORE;
	}

	case R_DATA1:
		FF_CMPSET(&cmd->output.fn, (void*)1, NULL);
		t->state = R_DATA;
		//fall through

	case R_DATA:
		break;

	case R_NEXT:
		FF_CMPSET(&cmd->output.fn, t->fn.ptr, NULL);
		t->state = R_DATA;
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD) {
		if (cmd->in_last)
			fftar_fin(&t->tar);
		t->tar.in = cmd->in;
	}

	for (;;) {

	r = fftar_read(&t->tar);
	switch (r) {

	case FFTAR_FILEHDR: {
		f = fftar_nextfile(&t->tar);

		if (cmd->members.len != 0) {
			if (0 > ffs_findarrz((void*)cmd->members.ptr, cmd->members.len, f->name, ffsz_len(f->name))) {
				t->skipfile = 1;
				continue;
			}
		}

		if (fcom_logchk(core->conf->loglev, FCOM_LOGVERB))
			untar_showinfo(t, f);
		if (cmd->show) {
			t->skipfile = 1;
			continue;
		}

		if (!(f->type == FFTAR_FILE || f->type == FFTAR_FILE0 || f->type == FFTAR_DIR)) {
			fcom_warnlog(FILT_NAME, "%s: unsupported file type '%c'", f->name, f->type);
			t->skipfile = 1;
			continue;
		}

		if (f->type == FFTAR_DIR && f->size != 0)
			fcom_warnlog(FILT_NAME, "directory %s has non-zero size", f->name);

		if (cmd->output.fn == NULL) {
			ffstr name;
			ffstr_setz(&name, f->name);
			if (FCOM_DATA != (r = fn_out(cmd, &name, &t->fn)))
				return r;
			cmd->output.fn = t->fn.ptr;
		}
		cmd->output.size = f->size;
		cmd->output.mtime = f->mtime;
		cmd->output.attr = f->mode & 0777;

		const char *filt = (f->type == FFTAR_DIR) ? "core.dir-out" : FCOM_CMD_FILT_OUT(cmd);
		com->ctrl(cmd, FCOM_CMD_FILTADD, filt);
		continue;
	}

	case FFTAR_DATA:
		if (t->skipfile)
			continue;
		cmd->out = t->tar.out;
		return FCOM_DATA;

	case FFTAR_FILEDONE:
		if (t->skipfile) {
			t->skipfile = 0;
			continue;
		}
		t->state = R_NEXT;
		return FCOM_NEXTDONE;

	case FFTAR_DONE:
		fftar_fin(&t->tar);
		ffmem_tzero(&t->tar);
		t->state = R_EOF;
		return FCOM_MORE;

	case FFTAR_MORE:
		return FCOM_MORE;

	case FFTAR_ERR:
		fcom_errlog(FILT_NAME, "%s", fftar_errstr(&t->tar));
		return FCOM_ERR;
	}
	}
}

/* "mode user group size date name" */
static void untar_showinfo(untar *t, const fftar_file *f)
{
	char *p = t->fn.ptr, *end = ffarr_edge(&t->fn);

	p += fffile_unixattr_tostr(p, end - p, fftar_mode(f));
	p = ffs_copyc(p, end, ' ');

	p += ffs_fromint(f->uid, p, end - p, FFINT_WIDTH(4));
	p = ffs_copyc(p, end, ' ');
	p += ffs_fromint(f->gid, p, end - p, FFINT_WIDTH(4));
	p = ffs_copyc(p, end, ' ');

	p += ffs_fromint(f->size, p, end - p, FFINT_WIDTH(12));
	p = ffs_copyc(p, end, ' ');

	ffdtm dt;
	fftime_split(&dt, &f->mtime, FFTIME_TZLOCAL);
	p += fftime_tostr(&dt, p, end - p, FFTIME_DATE_YMD | FFTIME_HMS);
	p = ffs_copyc(p, end, ' ');

	p = ffs_copyz(p, end, f->name);

	fcom_verblog(FILT_NAME, "%*s", p - t->fn.ptr, t->fn.ptr);
}

#undef FILT_NAME


#define FILT_NAME  "arc.zip"

typedef struct zip {
	uint state;
	ffzip_cook zip;
	ffarr buf;
} zip;

static void* zip_open(fcom_cmd *cmd)
{
	zip *z;
	if (NULL == (z = ffmem_new(zip)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&z->buf, BUFSIZE)) {
		fcom_syserrlog(FILT_NAME, "%s", ffmem_alloc_S);
		goto err;
	}

	uint lev = (cmd->deflate_level != 255) ? cmd->deflate_level : 6;
	if (0 != ffzip_winit(&z->zip, lev, 0)) {
		fcom_errlog(FILT_NAME, "%s", ffzip_errstr(&z->zip));
		goto err;
	}

	if (cmd->output.fn == NULL) {
		fcom_errlog(FILT_NAME, "Output file name must be specified", 0);
		goto err;
	}

	com->ctrl(cmd, FCOM_CMD_FILTADD, FCOM_CMD_FILT_OUT(cmd));
	return z;

err:
	zip_close(z, cmd);
	return NULL;
}

static void zip_close(void *p, fcom_cmd *cmd)
{
	zip *z = p;
	ffarr_free(&z->buf);
	ffzip_wclose(&z->zip);
	ffmem_free(z);
}

static int zip_process(void *p, fcom_cmd *cmd)
{
	zip *z = p;
	int r;
	enum E { W_NEXT, W_NEWFILE, W_DATA, W_EOF };

	switch ((enum E)z->state) {

	case W_EOF:
		FF_ASSERT(cmd->in.len == 0);
		z->state = W_NEXT;
		//fall through

	case W_NEXT:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0))) {
			ffzip_wfinish(&z->zip);
			z->state = W_DATA;
			break;
		}
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));

		z->state = W_NEWFILE;
		return FCOM_MORE;

	case W_NEWFILE: {
		ffzip_fattr attr = {0};
		ffzip_setsysattr(&attr, cmd->input.attr);
		if (0 != ffzip_wfile(&z->zip, cmd->input.fn, &cmd->input.mtime, &attr)) {
			fcom_errlog(FILT_NAME, "%s", ffzip_errstr(&z->zip));
			return FCOM_ERR;
		}
		z->state = W_DATA;
		//fall through
	}

	case W_DATA:
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD) {
		if (cmd->in_last)
			ffzip_wfiledone(&z->zip);
		z->zip.in = cmd->in;
	}

	for (;;) {

	r = ffzip_write(&z->zip, ffarr_end(&z->buf), ffarr_unused(&z->buf));
	switch (r) {

	case FFZIP_DATA:
		cmd->out = z->zip.out;
		return FCOM_DATA;

	case FFZIP_FILEDONE:
		fcom_infolog(FILT_NAME, "%s: %U => %U (%u%%)"
			, cmd->input.fn, z->zip.file_insize, z->zip.file_outsize
			, (uint)FFINT_DIVSAFE(z->zip.file_outsize * 100, z->zip.file_insize));
		z->state = W_EOF;
		return FCOM_MORE;

	case FFZIP_MORE:
		return FCOM_MORE;

	case FFZIP_DONE:
		return FCOM_DONE;

	case FFZIP_ERR:
		fcom_errlog(FILT_NAME, "%s", ffzip_errstr(&z->zip));
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME


#define FILT_NAME  "arc.unzip"

typedef struct unzip {
	uint state;
	ffzip zip;
	ffarr buf;
	ffarr fn;
	ffzip_file *curfile;
} unzip;

static void* unzip_open(fcom_cmd *cmd)
{
	unzip *z;
	if (NULL == (z = ffmem_new(unzip)))
		return FCOM_OPEN_SYSERR;
	ffzip_init(&z->zip, 0);

	if (NULL == ffarr_alloc(&z->buf, BUFSIZE))
		goto err;

	if (NULL == ffarr_alloc(&z->fn, 4096))
		goto err;

	return z;

err:
	unzip_close(z, cmd);
	return FCOM_OPEN_SYSERR;
}

static void unzip_close(void *p, fcom_cmd *cmd)
{
	unzip *z = p;
	ffarr_free(&z->buf);
	ffarr_free(&z->fn);
	ffzip_close(&z->zip);
	ffmem_free(z);
}

static int unzip_process(void *p, fcom_cmd *cmd)
{
	unzip *z = p;
	int r;
	ffzip_file *f;
	enum E { R_FIRST, R_NEXT, R_DATA1, R_DATA, R_EOF, };

again:
	switch ((enum E)z->state) {
	case R_EOF:
		ffzip_close(&z->zip);
		z->state = R_FIRST;
		//fall through

	case R_FIRST:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		z->state = R_DATA1;
		return FCOM_MORE;

	case R_DATA1:
		ffmem_tzero(&z->zip);
		ffzip_init(&z->zip, cmd->input.size);
		z->state = R_DATA;
		//fall through

	case R_DATA:
		break;

	case R_NEXT:
		FF_CMPSET(&cmd->output.fn, z->fn.ptr, NULL);
		for (;;) {
			if (NULL == (f = ffzip_nextfile(&z->zip))) {
				z->state = R_EOF;
				return FCOM_MORE;
			}

			if (cmd->members.len != 0
				&& 0 > ffs_findarrz((void*)cmd->members.ptr, cmd->members.len, f->fn, ffsz_len(f->fn)))
				continue;

			if (fcom_logchk(core->conf->loglev, FCOM_LOGVERB))
				unzip_showinfo(z, f);
			if (cmd->show)
				continue;

			ffzip_readfile(&z->zip, f->offset);
			z->curfile = f;
			break;
		}
		z->state = R_DATA;
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD) {
		z->zip.in = cmd->in;
	}

	for (;;) {

	r = ffzip_read(&z->zip, ffarr_end(&z->buf), ffarr_unused(&z->buf));
	switch ((enum FFZIP_R)r) {

	case FFZIP_FILEINFO:
		break;

	case FFZIP_DONE:
		z->state = R_NEXT;
		goto again;

	case FFZIP_FILEHDR:
		f = z->curfile;
		fcom_dbglog(0, FILT_NAME, "file header for %s: %U => %U"
			, f->fn, f->zsize, f->osize);

		if (ffzip_isdir(&f->attrs) && f->osize != 0)
			fcom_warnlog(FILT_NAME, "directory %s has non-zero size", f->fn);

		if (cmd->output.fn == NULL) {
			ffstr name;
			ffstr_setz(&name, f->fn);
			if (FCOM_DATA != (r = fn_out(cmd, &name, &z->fn)))
				return r;
			cmd->output.fn = z->fn.ptr;
		}
		cmd->output.size = f->osize;
		cmd->output.mtime = f->mtime;
		cmd->output.attr = f->attrs.win;
		cmd->out_attr_win = 1;

		const char *filt = (ffzip_isdir(&f->attrs)) ? "core.dir-out" : FCOM_CMD_FILT_OUT(cmd);
		com->ctrl(cmd, FCOM_CMD_FILTADD, filt);
		break;

	case FFZIP_DATA:
		cmd->out = z->zip.out;
		return FCOM_DATA;

	case FFZIP_FILEDONE:
		f = z->curfile;
		fcom_dbglog(0, FILT_NAME, "%s: %U => %U (%u%%)"
			, f->fn, f->zsize, f->osize
			, (int)FFINT_DIVSAFE(f->zsize * 100, f->osize));
		z->state = R_NEXT;
		return FCOM_NEXTDONE;

	case FFZIP_MORE:
		return FCOM_MORE;

	case FFZIP_SEEK:
		cmd->input.offset = ffzip_offset(&z->zip);
		cmd->in_seek = 1;
		return FCOM_MORE;

	case FFZIP_ERR:
		fcom_errlog(FILT_NAME, "%s", ffzip_errstr(&z->zip));
		return FCOM_ERR;
	}
	}
}

/* "size date name" */
static void unzip_showinfo(unzip *z, const ffzip_file *f)
{
	char *p = z->fn.ptr, *end = ffarr_edge(&z->fn);

	if (ffzip_isdir(&f->attrs))
		p = ffs_copy(p, end, "       <DIR>", 12);
	else
		p += ffs_fromint(f->osize, p, end - p, FFINT_WIDTH(12));
	p = ffs_copyc(p, end, ' ');

	ffdtm dt;
	fftime_split(&dt, &f->mtime, FFTIME_TZLOCAL);
	p += fftime_tostr(&dt, p, end - p, FFTIME_DATE_YMD | FFTIME_HMS);
	p = ffs_copyc(p, end, ' ');

	p = ffs_copyz(p, end, f->fn);

	fcom_verblog(FILT_NAME, "%*s", p - z->fn.ptr, z->fn.ptr);
}

#undef FILT_NAME


#define FILT_NAME  "arc.un7z"

typedef struct un7z {
	uint state;
	ff7z z;
	ffarr fn;
	ffarr buf;
} un7z;

static void* un7z_open(fcom_cmd *cmd)
{
	un7z *z;
	if (NULL == (z = ffmem_new(un7z)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&z->buf, BUFSIZE))
		goto err;

	if (NULL == ffarr_alloc(&z->fn, 4096))
		goto err;

	return z;

err:
	un7z_close(z, cmd);
	return FCOM_OPEN_SYSERR;
}

static void un7z_close(void *p, fcom_cmd *cmd)
{
	un7z *z = p;
	ffarr_free(&z->fn);
	ffarr_free(&z->buf);
	ff7z_close(&z->z);
	ffmem_free(z);
}

static int un7z_process(void *p, fcom_cmd *cmd)
{
	un7z *z = p;
	int r;
	enum E { R_FIRST, R_NEXT, R_DATA, R_EOF, };

	switch ((enum E)z->state) {
	case R_EOF:
		ff7z_close(&z->z);
		ffmem_tzero(&z->z);
		z->state = R_FIRST;
		//fall through

	case R_FIRST:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		ff7z_open(&z->z);
		z->state = R_DATA;
		return FCOM_MORE;

	case R_DATA:
		break;

	case R_NEXT:
		FF_CMPSET(&cmd->output.fn, z->fn.ptr, NULL);
		z->state = R_DATA;
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD) {
		ff7z_input(&z->z, cmd->in.ptr, cmd->in.len);
	}

	for (;;) {

	r = ff7z_read(&z->z);
	switch (r) {

	case FF7Z_FILEHDR:
		for (;;) {
			const ff7zfile *f;
			if (NULL == (f = ff7z_nextfile(&z->z))) {
				z->state = R_EOF;
				return FCOM_MORE;
			}

			if (cmd->members.len != 0
				&& 0 > ffs_findarrz((void*)cmd->members.ptr, cmd->members.len, f->name, ffsz_len(f->name)))
				continue;

			if (fcom_logchk(core->conf->loglev, FCOM_LOGVERB))
				un7z_showinfo(z, f);
			if (cmd->show)
				continue;

			if ((f->attr & FFWIN_FILE_DIR) && f->size != 0)
				fcom_warnlog(FILT_NAME, "directory %s has non-zero size", f->name);

			if (cmd->output.fn == NULL) {
				ffstr name;
				ffstr_setz(&name, f->name);
				if (FCOM_DATA != (r = fn_out(cmd, &name, &z->fn)))
					return r;
				cmd->output.fn = z->fn.ptr;
			}
			cmd->output.size = f->size;
			cmd->output.mtime = f->mtime;
			cmd->output.attr = f->attr;
			cmd->out_attr_win = 1;

			const char *filt = (f->attr & FFWIN_FILE_DIR) ? "core.dir-out" : FCOM_CMD_FILT_OUT(cmd);
			com->ctrl(cmd, FCOM_CMD_FILTADD, filt);
			break;
		}
		break;

	case FF7Z_DATA:
		cmd->out = z->z.out;
		return FCOM_DATA;

	case FF7Z_FILEDONE:
		z->state = R_NEXT;
		return FCOM_NEXTDONE;

	case FF7Z_MORE:
		return FCOM_MORE;

	case FF7Z_SEEK:
		cmd->input.offset = ff7z_offset(&z->z);
		cmd->in_seek = 1;
		return FCOM_MORE;

	case FF7Z_ERR:
		fcom_errlog(FILT_NAME, "%s", ff7z_errstr(&z->z));
		return FCOM_ERR;
	}
	}
}

/* "size date name" */
static void un7z_showinfo(un7z *z, const ff7zfile *f)
{
	char *p = z->fn.ptr, *end = ffarr_edge(&z->fn);

	if (f->attr & FFWIN_FILE_DIR)
		p = ffs_copy(p, end, "       <DIR>", 12);
	else
		p += ffs_fromint(f->size, p, end - p, FFINT_WIDTH(12));
	p = ffs_copyc(p, end, ' ');

	ffdtm dt;
	fftime_split(&dt, &f->mtime, FFTIME_TZLOCAL);
	p += fftime_tostr(&dt, p, end - p, FFTIME_DATE_YMD | FFTIME_HMS);
	p = ffs_copyc(p, end, ' ');

	p = ffs_copyz(p, end, f->name);

	fcom_verblog(FILT_NAME, "%*s", p - z->fn.ptr, z->fn.ptr);
}

#undef FILT_NAME


static void* unpack_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void unpack_close(void *p, fcom_cmd *cmd)
{
}

static const char arc_exts[][4] = {
	"7z",
	"gz",
	"tar",
	"tgz",
	"txz",
	"xz",
	"zip",
};
static const char *const arc_filts[] = {
	"arc.un7z",
	"arc.ungz",
	"arc.untar",
	"arc.untar",
	"arc.untar",
	"arc.unxz",
	"arc.unzip",
};

static int unpack_process(void *p, fcom_cmd *cmd)
{
	if (NULL == (cmd->input.fn = com->arg_next(cmd, FCOM_CMD_ARG_PEEK)))
		return FCOM_DONE;

	const char *name;
	int r;
	ffstr nm, ext;
	ffstr_setz(&nm, cmd->input.fn);
	ffpath_split3(nm.ptr, nm.len, NULL, &nm, &ext);
	if ((ffstr_ieqcz(&ext, "gz") || ffstr_ieqcz(&ext, "xz"))
		&& nm.len >= FFSLEN(".tar") && ffstr_irmatchz(&nm, ".tar"))
		ffstr_setz(&ext, "tar");

	if (0 > (r = ffcharr_findsorted(arc_exts, FFCNT(arc_exts), sizeof(arc_exts[0]), ext.ptr, ext.len))) {
		fcom_errlog("arc.unpack", "unknown archive file extension .%S", &ext);
		return FCOM_ERR;
	}

	name = arc_filts[r];
	com->fcom_cmd_filtadd(cmd, name);
	return FCOM_DONE;
}
