/** File input/output.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/sys/thpool.h>
#include <FF/sys/fileread.h>
#include <FF/sys/filewrite.h>
#include <FF/path.h>
#include <FFOS/error.h>
#include <FFOS/file.h>
#include <FFOS/dir.h>
#include <FFOS/asyncio.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, __VA_ARGS__)

extern const fcom_core *core;

struct file_mod {
	fflock lk;
	ffthpool *thpool;
	const fcom_command *com;
};
static struct file_mod *mod;

static ffthpool* thpool_create();

// MODULE
static int file_sig(uint signo);
static const void* file_iface(const char *name);
static int file_conf(const char *name, ffconf_scheme *cs);
const fcom_mod file_mod = {
	.sig = &file_sig, .iface = &file_iface, .conf = &file_conf,
};

// INPUT
static int fi_conf(ffconf_scheme *cs);
static void* fi_open(fcom_cmd *cmd);
static void fi_close(void *p, fcom_cmd *cmd);
static int fi_process(void *p, fcom_cmd *cmd);
static const fcom_filter fi_filt = {
	&fi_open, &fi_close, &fi_process,
};
struct inconf;
static struct inconf *inconf;
typedef struct file file;
static void fi_log(void *p, uint level, ffstr msg);
static void fi_onread(void *p);

// OUTPUT
static int fo_conf(ffconf_scheme *cs);
static void* fo_open(fcom_cmd *cmd);
static void fo_close(void *p, fcom_cmd *cmd);
static int fo_adddata(void *p, fcom_cmd *cmd);
static const fcom_filter fo_filt = {
	&fo_open, &fo_close, &fo_adddata,
};
struct outconf;
static struct outconf *outconf;
static void onwrite(void *udata);

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

static int file_conf(const char *name, ffconf_scheme *cs)
{
	if (ffsz_eq(name, "file-in"))
		return fi_conf(cs);
	else if (ffsz_eq(name, "file-out"))
		return fo_conf(cs);
	return 1;
}

static int file_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT:
		mod = ffmem_new(struct file_mod);
		mod->com = core->iface("core.com");
		break;

	case FCOM_SIGSTART:
		if (0 != ffaio_fctxinit())
			return -1;
		break;

	case FCOM_SIGFREE:
		if (0 != ffthpool_free(mod->thpool))
			fcom_syserrlog("file", "ffthpool_free", 0);
		mod->thpool = NULL;
		ffmem_safefree(inconf);
		ffmem_safefree(outconf);
		ffaio_fctxclose();
		ffmem_free0(mod);
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
	byte use_thread_pool;
};

#define OFF(member)  FF_OFF(struct inconf, member)
static const ffconf_arg fi_conf_args[] = {
	{ "bufsize",	FFCONF_TSIZE32 | FFCONF_FNOTZERO,  OFF(bufsize) },
	{ "nbufs",	FFCONF_TINT32 | FFCONF_FNOTZERO,  OFF(nbufs) },
	{ "readahead",	FFCONF_TBOOL8,  OFF(readahead) },
	{ "use_thread_pool",	FFCONF_TBOOL8,  OFF(use_thread_pool) },
	{ "direct_io",	FFCONF_TBOOL8,  OFF(directio) },
	{}
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

static int fi_conf(ffconf_scheme *cs)
{
	if (NULL == (inconf = ffmem_new(struct inconf)))
		return -1;
	inconf->bufsize = 64 * 1024;
	inconf->nbufs = 3;
	inconf->align = 4096;
	inconf->use_thread_pool = 1;
	inconf->directio = 0;
	inconf->readahead = 1;
	ffconf_scheme_addctx(cs, fi_conf_args, inconf);
	return 0;
}

static void* fi_open(fcom_cmd *cmd)
{
	file *f;
	if (NULL == (f = ffmem_new(file)))
		return NULL;
	f->fn = cmd->input.fn;

	fffileread_conf conf = {};
	if (inconf->use_thread_pool && !inconf->directio)
		conf.thpool = thpool_create();
	conf.directio = inconf->directio;
	conf.udata = f;
	conf.log = &fi_log;
	conf.log_debug = fcom_logchk(core->conf->loglev, FCOM_LOGDBG);
	conf.onread = &fi_onread;
	conf.kq = (fffd)mod->com->ctrl(cmd, FCOM_CMD_KQ);
	conf.oflags = FFO_RDONLY | FFO_NOATIME | FFO_NONBLOCK | FFO_NODOSNAME;
	conf.bufsize = inconf->bufsize;
	conf.nbufs = inconf->nbufs;
	conf.bufalign = inconf->align;
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

	f->uctx = cmd;
	dbglog(0, "opened file %s, %UKB"
		, f->fn, f->size / 1024);
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
		fffileread_free(f->fr);
	}

	ffmem_free(f);
}

static void fi_log(void *p, uint level, ffstr msg)
{
	file *f = p;
	switch (level) {
	case FFFILEREAD_LOG_ERR:
		errlog("%s: %S", f->fn, &msg);
		break;
	case FFFILEREAD_LOG_DBG:
		dbglog(0, "%S", &msg);
		break;
	}
}

static void fi_onread(void *p)
{
	file *f = p;
	mod->com->ctrl(f->uctx, FCOM_CMD_RUNASYNC);
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

	if (cmd->in_seek) {
		//read data at offset
		cmd->in_seek = 0;
		dbglog(0, "seek:%U", cmd->input.offset);
		f->nseek++;
		f->next_off = cmd->input.offset;
	} else {
		//read next data block
		cmd->input.offset = f->next_off;
	}

	uint flags = 0;
	if (inconf->readahead)
		flags |= FFFILEREAD_FREADAHEAD;
	if (cmd->in_backward)
		flags |= FFFILEREAD_FBACKWARD;

	r = fffileread_getdata(f->fr, &b, f->next_off, flags);
	switch ((enum FFFILEREAD_R)r) {

	case FFFILEREAD_RASYNC:
		return FCOM_ASYNC;

	case FFFILEREAD_RERR:
		return FCOM_ERR;

	case FFFILEREAD_REOF:
		cmd->in_last = 1;
		return FCOM_DONE;

	case FFFILEREAD_RREAD:
		break;
	}

	cmd->out = b;
	f->next_off += b.len;
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
	byte use_thread_pool;
};

#define OFF(member)  FF_OFF(struct outconf, member)
static const ffconf_arg fo_conf_args[] = {
	{ "bufsize",	FFCONF_TSIZE32 | FFCONF_FNOTZERO,  OFF(bufsize) },
	{ "prealloc",	FFCONF_TSIZE32 | FFCONF_FNOTZERO,  OFF(prealloc) },
	{ "prealloc_grow",	FFCONF_TBOOL8,  OFF(prealloc_grow) },
	{ "mkpath",	FFCONF_TBOOL8,  OFF(mkpath) },
	{ "del_on_err",	FFCONF_TBOOL8,  OFF(del_on_err) },
	{ "use_thread_pool",	FFCONF_TBOOL8,  OFF(use_thread_pool) },
	{}
};
#undef OFF

static int fo_conf(ffconf_scheme *cs)
{
	if (NULL == (outconf = ffmem_new(struct outconf)))
		return -1;
	outconf->bufsize = 64 * 1024;
	outconf->prealloc = 1 * 1024 * 1024;
	outconf->prealloc_grow = 1;
	outconf->mkpath = 1;
	outconf->del_on_err = 1;
	outconf->use_thread_pool = 1;
	ffconf_scheme_addctx(cs, fo_conf_args, outconf);
	return 0;
}

typedef struct fout {
	fffilewrite *fw;
	uint64 wr;
	ffstr name;
	fftime mtime;
	uint attr;
	fcom_cmd *cmd;
	uint ok :1;
	uint skip :1;
} fout;

static void fo_log(void *p, uint level, ffstr msg)
{
	// fout *f = p;
	switch (level) {
	case FFFILEWRITE_LOG_ERR:
		errlog("%S", &msg);
		break;
	case FFFILEWRITE_LOG_DBG:
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

	f->name.ptr = ffsz_dup(cmd->output.fn);
	if (f->name.ptr == NULL)
		goto err;
	f->name.len = ffsz_len(f->name.ptr);

	if (cmd->read_only)
		return f;

	fffilewrite_conf conf;
	fffilewrite_setconf(&conf);
	conf.udata = f;
	conf.log = &fo_log;
	conf.log_debug = fcom_logchk(core->conf->loglev, FCOM_LOGDBG);
	conf.onwrite = &onwrite;
	if (outconf->use_thread_pool)
		conf.thpool = thpool_create();
	conf.bufsize = outconf->bufsize;
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
	f->cmd = cmd;
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
			dbglog(0, "%S: mem write#:%u  file write#:%u  async#:%u  prealloc#:%u"
				, &f->name, st.nmwrite, st.nfwrite, st.nasync, st.nprealloc);
		}
	}

	if (verbose) {
		fcom_verblog(FILT_NAME, "saved file %S, %Ukb written"
			, &f->name, (int64)ffmax(f->wr / 1024, f->wr != 0));
	}

	ffstr_free(&f->name);
	ffmem_free(f);
}

static void onwrite(void *udata)
{
	fout *f = udata;
	mod->com->ctrl(f->cmd, FCOM_CMD_RUNASYNC);
}

/** Add more data from user.  Flush to file, if necessary. */
static int fo_adddata(void *p, fcom_cmd *cmd)
{
	fout *f = p;

	if (f->fw == NULL) {
		return (cmd->flags & FCOM_CMD_FIRST) ? FCOM_DONE : FCOM_MORE;
	}
	if (f->skip) {
		return (cmd->flags & FCOM_CMD_FIRST) ? FCOM_DONE : FCOM_MORE;
	}

	int64 seek = -1;
	if (cmd->out_seek) {
		cmd->out_seek = 0;
		seek = cmd->output.offset;
		dbglog(0, "seeking to %xU...", seek);
	}

	uint flags = 0;
	if (cmd->flags & FCOM_CMD_FIRST)
		flags = FFFILEWRITE_FFLUSH;

	for (;;) {
		ssize_t r = fffilewrite_write(f->fw, cmd->in, seek, flags);
		switch (r) {
		default:
			if (r == 0 && (cmd->flags & FCOM_CMD_FIRST)) {
				f->ok = 1;
				return FCOM_DONE;
			}

			ffstr_shift(&cmd->in, r);
			f->wr += r;

			if (cmd->in.len == 0 && !(cmd->flags & FCOM_CMD_FIRST)) {
				return FCOM_MORE;
			}

			seek = -1;
			continue;

		case FFFILEWRITE_RERR:
			if (cmd->skip_err) {
				f->skip = 1;
				return (cmd->flags & FCOM_CMD_FIRST) ? FCOM_DONE : FCOM_MORE;
			}
			return FCOM_ERR;

		case FFFILEWRITE_RASYNC:
			return FCOM_ASYNC;
		}
	}
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


/** Create thread pool. */
static ffthpool* thpool_create()
{
	if (mod->thpool != NULL)
		return mod->thpool;

	fflk_lock(&mod->lk);
	if (mod->thpool == NULL) {
		ffthpoolconf ioconf = {};
		ioconf.maxthreads = 1;
		if (inconf->use_thread_pool && outconf->use_thread_pool)
			ioconf.maxthreads = 2;
		ioconf.maxqueue = 64;
		if (NULL == (mod->thpool = ffthpool_create(&ioconf)))
			fcom_syserrlog("file", "ffthpool_create", 0);
	}
	fflk_unlock(&mod->lk);
	return mod->thpool;
}
