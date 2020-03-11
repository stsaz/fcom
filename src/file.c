/** File input/output.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/sys/fileread.h>
#include <FF/sys/filewrite.h>
#include <FF/path.h>
#include <FFOS/error.h>
#include <FFOS/file.h>
#include <FFOS/dir.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, __VA_ARGS__)

extern const fcom_core *core;
static const fcom_command *com;

// MODULE
static int file_sig(uint signo);
static const void* file_iface(const char *name);
static int file_conf(const char *name, ffpars_ctx *ctx);
const fcom_mod file_mod = {
	.sig = &file_sig, .iface = &file_iface, .conf = &file_conf,
};

// INPUT
static int fi_conf(ffpars_ctx *ctx);
static void* fi_open(fcom_cmd *cmd);
static void fi_close(void *p, fcom_cmd *cmd);
static int fi_process(void *p, fcom_cmd *cmd);
static const fcom_filter fi_filt = {
	&fi_open, &fi_close, &fi_process,
};
struct inconf;
static struct inconf *inconf;
typedef struct file file;
static void fi_log(void *p, uint level, const ffstr *msg);
static void fi_onread(void *p);

// OUTPUT
static int fo_conf(ffpars_ctx *ctx);
static void* fo_open(fcom_cmd *cmd);
static void fo_close(void *p, fcom_cmd *cmd);
static int fo_adddata(void *p, fcom_cmd *cmd);
static const fcom_filter fo_filt = {
	&fo_open, &fo_close, &fo_adddata,
};
struct outconf;
static struct outconf *outconf;

// DIR OUTPUT
static void* diro_open(fcom_cmd *cmd);
static void diro_close(void *p, fcom_cmd *cmd);
static int diro_process(void *p, fcom_cmd *cmd);
static const fcom_filter diro_filt = { &diro_open, &diro_close, &diro_process };


struct cmd {
	const char *name;
	const fcom_filter *iface;
};

extern const fcom_filter fistd_filt;
extern const fcom_filter fostd_filt;

static const struct cmd cmds[] = {
	{ "file-in", &fi_filt },
	{ "file-out", &fo_filt },
	{ "dir-out", &diro_filt },
	{ "stdin", &fistd_filt },
	{ "stdout", &fostd_filt },
};

static const void* file_iface(const char *name)
{
	const struct cmd *cmd;
	FFARR_WALKNT(cmds, FFCNT(cmds), cmd, struct cmd) {
		if (ffsz_eq(name, cmd->name))
			return cmd->iface;
	}
	return NULL;
}

static int file_conf(const char *name, ffpars_ctx *ctx)
{
	if (ffsz_eq(name, "file-in"))
		return fi_conf(ctx);
	else if (ffsz_eq(name, "file-out"))
		return fo_conf(ctx);
	return 1;
}

static int file_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT:
		com = core->iface("core.com");
		break;
	case FCOM_SIGSTART:
		if (0 != ffaio_fctxinit())
			return -1;
		break;
	case FCOM_SIGFREE:
		ffmem_safefree(inconf);
		ffmem_safefree(outconf);
		ffaio_fctxclose();
		break;
	}
	return 0;
}


#define FILT_NAME  "file-in"

struct inconf {
	uint bufsize;
	uint nbufs;
	uint align;
	byte directio;
	byte readahead;
};

#define OFF(member)  FFPARS_DSTOFF(struct inconf, member)
static const ffpars_arg fi_conf_args[] = {
	{ "bufsize",	FFPARS_TSIZE | FFPARS_FNOTZERO,  OFF(bufsize) },
	{ "nbufs",	FFPARS_TINT8 | FFPARS_FNOTZERO,  OFF(nbufs) },
	{ "direct_io",	FFPARS_TBOOL8,  OFF(directio) },
	{ "readahead",	FFPARS_TBOOL8,  OFF(readahead) },
};
#undef OFF

struct file {
	fffileread *fr;
	const char *fn;
	uint64 size;
	fcom_cmd *uctx;
	uint64 next_off; // the offset of the next read
	uint nseek;
};

static int fi_conf(ffpars_ctx *ctx)
{
	if (NULL == (inconf = ffmem_new(struct inconf)))
		return -1;
	inconf->bufsize = 64 * 1024;
	inconf->nbufs = 3;
	inconf->align = 4096;
	inconf->directio = 1;
	inconf->readahead = 1;
	ffpars_setargs(ctx, inconf, fi_conf_args, FFCNT(fi_conf_args));
	return 0;
}

static void* fi_open(fcom_cmd *cmd)
{
	file *f;
	if (NULL == (f = ffmem_new(file)))
		return NULL;
	f->fn = cmd->input.fn;

	fffileread_conf conf = {};
	conf.udata = f;
	conf.log = &fi_log;
	conf.onread = &fi_onread;
	conf.kq = (fffd)com->ctrl(cmd, FCOM_CMD_KQ);
	conf.oflags = FFO_RDONLY | FFO_NOATIME | FFO_NONBLOCK | FFO_NODOSNAME;
	conf.bufsize = inconf->bufsize;
	conf.nbufs = inconf->nbufs;
	conf.bufalign = inconf->align;
	conf.directio = inconf->directio;
	f->fr = fffileread_create(f->fn, &conf);
	if (f->fr == NULL)
		goto err;

	fffileinfo fi;
	if (0 != fffile_info(fffileread_fd(f->fr), &fi)) {
		syserrlog("%s: %s", fffile_info_S, f->fn);
		goto err;
	}
	f->size = fffile_infosize(&fi);

	cmd->input.size = f->size;
	cmd->input.offset = 0;
	cmd->input.attr = fffile_infoattr(&fi);
	cmd->input.mtime = fffile_infomtime(&fi);
	if (cmd->out_preserve_date)
		cmd->output.mtime = cmd->input.mtime;
	cmd->in_last = 0;

	dbglog(0, "opened file %s, %UKB, directio:%u"
		, f->fn, f->size / 1024, (int)conf.directio);
	return f;

err:
	fi_close(f, cmd);
	return NULL;
}

static void fi_close(void *p, fcom_cmd *cmd)
{
	file *f = p;

	if (f->fr != NULL) {
		struct fffileread_stat stat;
		fffileread_stat(f->fr, &stat);
		dbglog(0, "cache-hit#:%u  read#:%u  async#:%u  seek#:%u"
			, stat.ncached, stat.nread, stat.nasync, f->nseek);
		fffileread_unref(f->fr);
	}

	ffmem_free(f);
}

static void fi_log(void *p, uint level, const ffstr *msg)
{
	file *f = p;
	switch (level) {
	case 0:
		syserrlog("%s: %S", f->fn, msg);
		break;
	case 1:
		dbglog(0, "%S", msg);
		break;
	}
}

static void fi_onread(void *p)
{
	file *f = p;
	if (f->uctx == NULL)
		return;
	com->run(f->uctx);
}

/** Return data requested by user, if available, or suspend otherwise. */
static int fi_process(void *p, fcom_cmd *cmd)
{
	file *f = p;
	ffstr b;
	int r;

	if (fffile_isdir(cmd->input.attr)) {
		cmd->in_last = 1;
		ffstr_null(&cmd->out);
		return FCOM_DONE;
	}

	if (f->uctx != NULL) {
		f->uctx = NULL;
	} else {
		if (cmd->in_seek) {
			//read data at offset
			dbglog(0, "seek:%U", cmd->input.offset);
			f->nseek++;
		} else {
			//read next data block
			cmd->input.offset = f->next_off;
		}
	}

	uint flags = 0;
	if (inconf->readahead)
		flags |= FFFILEREAD_FREADAHEAD;
	if (cmd->in_backward)
		flags |= FFFILEREAD_FBACKWARD;

	r = fffileread_getdata(f->fr, &b, cmd->input.offset, flags);
	switch ((enum FFFILEREAD_R)r) {

	case FFFILEREAD_RASYNC:
		f->uctx = cmd;
		return FCOM_ASYNC;

	case FFFILEREAD_RERR:
		return FCOM_ERR;

	case FFFILEREAD_REOF:
		cmd->in_last = 1;
		return FCOM_DONE;

	case FFFILEREAD_RREAD:
		break;
	}

	if (cmd->in_seek)
		cmd->in_seek = 0;
	cmd->out = b;
	f->next_off = cmd->input.offset + b.len;
	return FCOM_DATA;
}

#undef FILT_NAME


#define FILT_NAME  "file-out"

struct outconf {
	uint bufsize;
	uint prealloc;
	byte prealloc_grow;
	byte mkpath;
	byte del_on_err;
};

#define OFF(member)  FFPARS_DSTOFF(struct outconf, member)
static const ffpars_arg fo_conf_args[] = {
	{ "bufsize",	FFPARS_TSIZE | FFPARS_FNOTZERO,  OFF(bufsize) },
	{ "prealloc",	FFPARS_TSIZE | FFPARS_FNOTZERO,  OFF(prealloc) },
	{ "prealloc_grow",	FFPARS_TBOOL8,  OFF(prealloc_grow) },
	{ "mkpath",	FFPARS_TBOOL8,  OFF(mkpath) },
	{ "del_on_err",	FFPARS_TBOOL8,  OFF(del_on_err) },
};
#undef OFF

static int fo_conf(ffpars_ctx *ctx)
{
	if (NULL == (outconf = ffmem_new(struct outconf)))
		return -1;
	outconf->bufsize = 64 * 1024;
	outconf->prealloc = 1 * 1024 * 1024;
	outconf->prealloc_grow = 1;
	outconf->mkpath = 1;
	outconf->del_on_err = 1;
	ffpars_setargs(ctx, outconf, fo_conf_args, FFCNT(fo_conf_args));
	return 0;
}

typedef struct fout {
	fffilewrite *fw;
	uint64 wr;
	ffstr name;
	fftime mtime;
	uint attr;
	uint ok :1;
} fout;

static void fo_log(void *p, uint level, ffstr msg)
{
	// fout *f = p;
	switch (level) {
	case 0:
		syserrlog("%S", &msg);
		break;
	case 1:
		dbglog(0, "%S", &msg);
		break;
	}
}

/** Open/create file.  Create file path, if needed. */
static void* fo_open(fcom_cmd *cmd)
{
	fout *f;
	if (NULL == (f = ffmem_new(fout)))
		return NULL;

	if (NULL == ffstr_alcopyz(&f->name, cmd->output.fn))
		goto err;

	if (cmd->read_only)
		return f;

	fffilewrite_conf conf;
	fffilewrite_setconf(&conf);
	conf.udata = f;
	conf.log = &fo_log;
	conf.bufsize = outconf->bufsize;
	conf.align = 4096;
	conf.prealloc = (cmd->output.size != 0) ? cmd->output.size : outconf->prealloc;
	conf.prealloc_grow = outconf->prealloc_grow;
	conf.mkpath = outconf->mkpath;
	conf.del_on_err = outconf->del_on_err;
	conf.overwrite = cmd->out_overwrite;
	const char *filename = cmd->output.fn;
	if (NULL == (f->fw = fffilewrite_create(filename, &conf)))
		goto err;

	f->mtime = cmd->output.mtime;
	f->attr = cmd->output.attr;
	return f;

err:
	if (cmd->skip_err)
		return f;
	fo_close(f, cmd);
	return NULL;
}

static void fo_close(void *p, fcom_cmd *cmd)
{
	fout *f = p;
	uint verbose = cmd->read_only;

	if (f->fw != NULL) {
		fffilewrite_stat st;
		fffilewrite_getstat(f->fw, &st);
		fffilewrite_free(f->fw);

		if (f->ok) {
#ifdef FF_UNIX
			if (f->attr != 0 && !cmd->out_attr_win)
				fffile_attrsetfn(f->name.ptr, f->attr);
#elif defined FF_WIN
			if (f->attr != 0 && cmd->out_attr_win)
				fffile_attrsetfn(f->name.ptr, f->attr);
#endif

			if (fftime_sec(&f->mtime) != 0)
				fffile_settimefn(f->name.ptr, &f->mtime);

			verbose = 1;
			dbglog(0, "%S: mem write#:%u  file write#:%u  prealloc#:%u"
				, &f->name, st.nmwrite, st.nfwrite, st.nprealloc);
		}
	}

	if (verbose) {
		fcom_verblog(FILT_NAME, "saved file %S, %Ukb written"
			, &f->name, (int64)ffmax(f->wr / 1024, f->wr != 0));
	}

	ffstr_free(&f->name);
	ffmem_free(f);
}

/** Add more data from user.  Flush to file, if necessary. */
static int fo_adddata(void *p, fcom_cmd *cmd)
{
	fout *f = p;

	if (f->fw == NULL) {
		return (cmd->flags & FCOM_CMD_FIRST) ? FCOM_DONE : FCOM_MORE;
	}

	int64 off = -1;
	if (cmd->out_seek) {
		cmd->out_seek = 0;
		off = cmd->output.offset;
	}

	uint flags = 0;
	if (cmd->flags & FCOM_CMD_FIRST)
		flags = FFFILEWRITE_FFLUSH;

	int r = fffilewrite_write(f->fw, cmd->in, off, flags);
	switch (r) {
	case FFFILEWRITE_RWRITTEN:
		f->wr += cmd->in.len;
		if (cmd->flags & FCOM_CMD_FIRST) {
			f->ok = 1;
			return FCOM_DONE;
		}
		return FCOM_MORE;

	case FFFILEWRITE_RERR:
		return FCOM_ERR;

	case FFFILEWRITE_RASYNC:
		return FCOM_ASYNC;
	}
	return FCOM_ERR;
}

#undef FILT_NAME


#define FILT_NAME  "dir-out"

static void* diro_open(fcom_cmd *cmd)
{
	const char *fn = cmd->output.fn;

	if (cmd->read_only) {
		fcom_verblog(FILT_NAME, "created directory %s"
			, fn);
		return FCOM_SKIP;
	}

	if (0 != ffdir_make(fn)) {

		if (outconf->mkpath && fferr_nofile(fferr_last())) {
			if (0 != ffdir_rmake((void*)fn, 0)) {
				syserrlog("%s: for filename %s", ffdir_make_S, fn);
				goto err_nolog;
			}

		} else if (fferr_exist(fferr_last())) {

		} else
			goto err;
	}

#ifdef FF_UNIX
	if (cmd->output.attr != 0 && !cmd->out_attr_win)
		fffile_attrsetfn(fn, cmd->output.attr);
#else
	if (cmd->output.attr != 0 && cmd->out_attr_win)
		fffile_attrsetfn(fn, cmd->output.attr);
#endif

	if (fftime_sec(&cmd->output.mtime) != 0)
		fffile_settimefn(fn, &cmd->output.mtime);

	fcom_verblog(FILT_NAME, "created directory %s"
		, fn);

	return FCOM_SKIP;

err:
	syserrlog("%s", fn);
err_nolog:
	if (cmd->skip_err)
		return FCOM_SKIP;
	return NULL;
}

static void diro_close(void *p, fcom_cmd *cmd)
{
}

static int diro_process(void *p, fcom_cmd *cmd)
{
	return FCOM_ERR;
}

#undef FILT_NAME
