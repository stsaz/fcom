/** File input/output.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/number.h>
#include <FF/path.h>
#include <FFOS/asyncio.h>
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
typedef struct buf buf;
static int fi_getdata(file *f, buf **pb, uint64 off);
static int fi_read_off(file *f, uint64 off);
static int fi_read(file *f);

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

//STDIN
static void* fistd_open(fcom_cmd *cmd);
static void fistd_close(void *ctx, fcom_cmd *cmd);
static int fistd_read(void *ctx, fcom_cmd *cmd);
static const fcom_filter fistd_filt = { &fistd_open, &fistd_close, &fistd_read };

//STDOUT
static void* fostd_open(fcom_cmd *cmd);
static void fostd_close(void *ctx, fcom_cmd *cmd);
static int fostd_write(void *ctx, fcom_cmd *cmd);
static const fcom_filter fostd_filt = { &fostd_open, &fostd_close, &fostd_write, };


struct cmd {
	const char *name;
	const fcom_filter *iface;
};

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

struct buf {
	size_t len;
	char *ptr;
	uint64 offset;
};

struct file {
	const char *fn;
	fffd fd;
	uint64 size;
	ffaio_filetask aio;
	uint state; //enum FI_ST
	uint rdahead :1;

	ffarr2 bufs; //buf[]
	uint wbuf;
	uint last;
	uint locked;

	fcom_cmd *uctx;
	uint64 next_off;

	struct {
		uint nread;
		uint nasync;
		uint nseek;
		uint ncached;
	} stat;
};

enum FI_ST {
	FI_IOK,
	FI_IERR,
	FI_IASYNC,
	FI_IEOF,
};

enum FI_R {
	FI_RERR = FI_IERR,
	FI_RASYNC = FI_IASYNC,
	FI_REOF = FI_IEOF,
	FI_RCACHE,
	FI_RREAD,
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

/** Create buffers aligned to system pagesize. */
static int bufs_create(file *f)
{
	if (NULL == ffarr2_callocT(&f->bufs, inconf->nbufs, buf))
		goto err;
	f->bufs.len = inconf->nbufs;
	buf *b;
	FFARR_WALKT(&f->bufs, b, buf) {
		if (NULL == (b->ptr = ffmem_align(inconf->bufsize, inconf->align)))
			goto err;
		b->offset = (uint64)-1;
	}
	return 0;

err:
	syserrlog("%s", ffmem_alloc_S);
	return -1;
}

static void bufs_free(ffarr2 *bufs)
{
	buf *b;
	FFARR_WALKT(bufs, b, buf) {
		ffmem_alignfree(b->ptr);
	}
	ffarr2_free(bufs);
}

/** Find buffer containing file offset. */
static buf* bufs_find(file *f, uint64 offset)
{
	buf *b;
	FFARR_WALKT(&f->bufs, b, buf) {
		if (ffint_within(offset, b->offset, b->offset + b->len))
			return b;
	}
	return NULL;
}

static void* fi_open(fcom_cmd *cmd)
{
	file *f;
	if (NULL == (f = ffmem_new(file)))
		return NULL;
	f->fd = FF_BADFD;
	f->locked = (uint)-1;
	f->fn = cmd->input.fn;

	if (0 != bufs_create(f))
		goto err;

	uint flags = O_RDONLY | O_NOATIME | O_NONBLOCK | FFO_NODOSNAME;
	flags |= (inconf->directio) ? O_DIRECT : 0;

	while (FF_BADFD == (f->fd = fffile_open(f->fn, flags))) {

#ifdef FF_LINUX
		if (fferr_last() == EINVAL && (flags & O_DIRECT)) {
			flags &= ~O_DIRECT;
			continue;
		}
#endif

		syserrlog("%s: %s", fffile_open_S, f->fn);
		goto err;
	}

	fffileinfo fi;
	if (0 != fffile_info(f->fd, &fi)) {
		syserrlog("%s: %s", fffile_info_S, f->fn);
		goto err;
	}
	f->size = fffile_infosize(&fi);

	ffaio_finit(&f->aio, f->fd, f);
	if (0 != ffaio_fattach(&f->aio, core->kq, !!(flags & O_DIRECT))) {
		syserrlog("%s: %s", ffkqu_attach_S, f->fn);
		goto err;
	}
	f->rdahead = inconf->readahead && !!(flags & O_DIRECT) && (inconf->nbufs != 1);

	cmd->input.size = f->size;
	cmd->input.offset = 0;
	cmd->input.attr = fffile_infoattr(&fi);
	cmd->input.mtime = fffile_infomtime(&fi);
	if (cmd->out_preserve_date)
		cmd->output.mtime = cmd->input.mtime;
	cmd->in_last = 0;

	dbglog(0, "opened file %s, %UKB, directio:%u"
		, cmd->input.fn, f->size / 1024, !!(flags & O_DIRECT));
	return f;

err:
	fi_close(f, cmd);
	return NULL;
}

static void fi_close(void *p, fcom_cmd *cmd)
{
	file *f = p;

	if (f->fd != FF_BADFD)
		dbglog(0, "cache-hit#:%u  read#:%u  async#:%u  seek#:%u"
			, f->stat.ncached, f->stat.nread, f->stat.nasync, f->stat.nseek);

	FF_SAFECLOSE(f->fd, FF_BADFD, fffile_close);
	if (f->state == FI_IASYNC)
		return; //wait until AIO is completed

	bufs_free(&f->bufs);
	ffmem_free(f);
}

/** Called by producer with the result of operation. */
static void fi_nfy(file *f)
{
	if (f->uctx == NULL)
		return;
	com->run(f->uctx);
}

/** Return data requested by user, if available, or suspend otherwise. */
static int fi_process(void *p, fcom_cmd *cmd)
{
	file *f = p;
	buf *b = NULL;
	int r, async = 0, cachehit = 0;

	if (fffile_isdir(cmd->input.attr)) {
		cmd->in_last = 1;
		ffstr_null(&cmd->out);
		return FCOM_DONE;
	}

	if (f->uctx != NULL) {
		f->uctx = NULL;
		async = 1;
	} else {
		if (cmd->in_seek) {
			//read data at offset
			dbglog(0, "seek:%U", cmd->input.offset);
			f->stat.nseek++;
		} else {
			//read next data block
			cmd->input.offset = f->next_off;
		}
	}

	r = fi_getdata(f, &b, cmd->input.offset);
	switch ((enum FI_R)r) {
	case FI_RASYNC:
		f->uctx = cmd;
		return FCOM_ASYNC;
	case FI_RERR:
		return FCOM_ERR;
	case FI_REOF:
		cmd->in_last = 1;
		return FCOM_DONE;
	case FI_RCACHE:
		if (!async) {
			cachehit = 1;
			f->stat.ncached++;
		}
		break;
	case FI_RREAD:
		break;
	}

	if (cmd->in_seek)
		cmd->in_seek = 0;
	ffarr_setshift(&cmd->out, b->ptr, b->len, cmd->input.offset - b->offset);
	f->next_off = b->offset + b->len;

	uint ibuf = b - (buf*)f->bufs.ptr;
	dbglog(0, "returning buf#%u  off:%Uk  cache-hit:%u"
		, ibuf, b->offset / 1024, cachehit);
	return FCOM_DATA;
}

/** Get data block from cache or begin reading data from file.
Return enum FI_R. */
static int fi_getdata(file *f, buf **pb, uint64 off)
{
	int r;
	buf *b;
	uint64 next;

	f->locked = (uint)-1;

	if (NULL != (b = bufs_find(f, off))) {
		r = FI_RCACHE;
		goto done;
	}

	if (off > f->size) {
		errlog("seek offset %U is bigger than file size %U", off, f->size);
		return FI_RERR;
	}
	if (f->state == FI_IEOF && off == f->size)
		return FI_REOF;

	if (f->state == FI_IASYNC)
		return FI_RASYNC;

	r = fi_read_off(f, ff_align_floor2(off, inconf->align));
	if (r == FCOM_ASYNC)
		return FI_RASYNC;
	else if (r == FCOM_ERR)
		return FI_RERR;

	//FCOM_DATA or FCOM_DONE
	b = bufs_find(f, off);
	if (r == FCOM_DONE && b == NULL)
		return FI_REOF;
	FF_ASSERT(b != NULL);
	r = FI_RREAD;

done:
	next = b->offset + b->len;
	if (f->rdahead && next < f->size
		&& NULL == bufs_find(f, next)) {
		if (f->state != FI_IASYNC)
			fi_read_off(f, next);
	}
	*pb = b;
	f->locked = b - (buf*)f->bufs.ptr;
	return r;
}

/** Prepare buffer for reading. */
static void buf_prepread(buf *b, uint64 off)
{
	b->len = 0;
	b->offset = off;
}

/** Async read has signalled.  Notify consumer about new events. */
static void fi_read_a(void *param)
{
	file *f = param;
	int r;
	FF_ASSERT(f->state == FI_IASYNC);
	f->state = FI_IOK;
	if (f->fd == FF_BADFD) {
		//chain was closed while AIO is pending
		fi_close(f, NULL);
		return;
	}
	r = fi_read(f);
	if (r == FCOM_ASYNC)
		return;
	fi_nfy(f);
}

/** Start reading at the specified aligned offset. */
static int fi_read_off(file *f, uint64 off)
{
	buf *b = ffarr_itemT(&f->bufs, f->wbuf, buf);
	buf_prepread(b, off);
	if (f->last == f->wbuf)
		f->last = (uint)-1;
	return fi_read(f);
}

/** Read from file. */
static int fi_read(file *f)
{
	int r;
	buf *b;

	b = ffarr_itemT(&f->bufs, f->wbuf, buf);
	r = (int)ffaio_fread(&f->aio, b->ptr, inconf->bufsize, b->offset, &fi_read_a);
	if (r < 0) {
		if (fferr_again(fferr_last())) {
			dbglog(0, "buf#%u: async read, offset:%Uk", f->wbuf, b->offset / 1024);
			f->state = FI_IASYNC;
			f->stat.nasync++;
			return FCOM_ASYNC;
		}

		syserrlog("%s: %s  buf#%u offset:%Uk"
			, fffile_read_S, f->fn, f->wbuf, b->offset / 1024);
		f->state = FI_IERR;
		return FCOM_ERR;
	}

	b->len = r;
	f->stat.nread++;
	dbglog(0, "buf#%u: read %L bytes at offset %Uk (%u%%)"
		, f->wbuf, b->len, b->offset / 1024, (int)FFINT_DIVSAFE(b->offset * 100, f->size));

	f->wbuf = ffint_cycleinc(f->wbuf, inconf->nbufs);

	if ((uint)r != inconf->bufsize) {
		dbglog(0, "read the last block", 0);
		f->last = f->wbuf;
		f->state = FI_IEOF;
		return FCOM_DONE;
	}
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
	fffd fd;
	ffarr buf;
	ffstr name;
	fftime mtime;
	uint attr;
	uint64 off;
	uint64 size;
	uint64 prealloc;
	uint64 prealloc_by;
	uint ok :1;
	struct {
		uint nmwrite;
		uint nfwrite;
		uint nprealloc;
	} stat;
} fout;

/** Open/create file.  Create file path, if needed.
Preallocate space if advised by user. */
static void* fo_open(fcom_cmd *cmd)
{
	fout *f;
	uint canskip = 0, mask;
	if (NULL == (f = ffmem_new(fout)))
		return NULL;
	f->fd = FF_BADFD;

	if (NULL == ffstr_alcopyz(&f->name, cmd->output.fn))
		goto err;

	if (cmd->read_only)
		return f;

	if (NULL == ffarr_alloc(&f->buf, outconf->bufsize))
		goto err;

	const char *filename = cmd->output.fn;
	uint flags = (cmd->out_overwrite || cmd->out_resume) ? O_CREAT : FFO_CREATENEW;
	flags |= O_WRONLY;
	mask = 0;
	canskip = 1;
	while (FF_BADFD == (f->fd = fffile_open(filename, flags))) {

		if (outconf->mkpath && fferr_nofile(fferr_last())) {
			if (0 != ffdir_make_path((void*)filename, 0)) {
				syserrlog("%s: for filename %s", ffdir_make_S, filename);
				goto err;
			}

			if (ffbit_testset32(&mask, 0))
				goto err;

#ifdef FF_UNIX
		} else if (fferr_last() == EISDIR) {
			flags = O_RDONLY;
			if (ffbit_testset32(&mask, 1))
				goto err;
#endif

		} else
			goto err;
	}

	if (cmd->output.size != 0) {
		if (0 == fffile_trunc(f->fd, cmd->output.size)) {
			dbglog(0, "prealloc: %U", cmd->output.size);
			f->prealloc = cmd->output.size;
			f->stat.nprealloc++;
		}
	}

	if (cmd->out_resume) {
		fffileinfo fi;
		if (0 != fffile_info(f->fd, &fi)) {
			syserrlog("%s: %s", fffile_info_S, filename);
			goto err;
		}
		cmd->out_seek = fffile_infosize(&fi);
	}

	f->mtime = cmd->output.mtime;
	f->attr = cmd->output.attr;

	f->prealloc_by = outconf->prealloc;
	return f;

err:
	syserrlog("%s", cmd->output.fn);
	if (canskip && cmd->skip_err)
		return f;
	fo_close(f, cmd);
	return NULL;
}

/**
Truncate file to the actual size written.
Delete file on error. */
static void fo_close(void *p, fcom_cmd *cmd)
{
	fout *f = p;
	uint verbose = cmd->read_only;

	if (f->fd != FF_BADFD) {

		if (!cmd->out_notrunc)
			fffile_trunc(f->fd, f->size);

		if (!f->ok && outconf->del_on_err) {

			if (0 != fffile_close(f->fd))
				syserrlog("%s", fffile_close_S);

			if (0 == fffile_rm(f->name.ptr))
				dbglog(0, "removed file %S", &f->name);

		} else {

#ifdef FF_UNIX
			if (f->attr != 0 && !cmd->out_attr_win)
				fffile_attrset(f->fd, f->attr);
#elif FF_WIN >= 0x0600
			if (f->attr != 0 && cmd->out_attr_win)
				fffile_attrset(f->fd, f->attr);
#elif FF_WIN < 0x0600
			if (f->attr != 0 && cmd->out_attr_win)
				fffile_attrsetfn(f->name.ptr, f->attr);
#endif

			if (fftime_sec(&f->mtime) != 0)
				fffile_settime(f->fd, &f->mtime);

			if (0 != fffile_close(f->fd))
				syserrlog("%s", fffile_close_S);
			verbose = 1;
		}
	}

	if (verbose) {
		fcom_verblog(FILT_NAME, "saved file %S, %Ukb written"
			, &f->name, (int64)ffmax(f->size / 1024, f->size != 0));
	}

	ffstr_free(&f->name);
	ffarr_free(&f->buf);
	dbglog(0, "mem write#:%u  file write#:%u  prealloc:%Uk(%u)"
		, f->stat.nmwrite, f->stat.nfwrite, f->prealloc / 1024, f->stat.nprealloc);
	ffmem_free(f);
}

/** Write data to file.  Preallocate space, if needed. */
static int fo_write(fout *f, fcom_cmd *cmd, const ffstr *s)
{
	if (f->off + s->len > f->prealloc) {
		uint64 n = ff_align_ceil(f->off + s->len, f->prealloc_by);
		if (0 == fffile_trunc(f->fd, n)) {
			dbglog(0, "prealloc: %U", n);

			if (outconf->prealloc_grow)
				f->prealloc_by += f->prealloc_by;

			f->prealloc = n;
			f->stat.nprealloc++;
		}
	}

	ssize_t r = fffile_write(f->fd, s->ptr, s->len);
	if (r != (ssize_t)s->len)
		return FCOM_SYSERR;
	f->stat.nfwrite++;

	dbglog(0, "written %L bytes at offset %Uk (%L pending)", s->len, f->off / 1024, cmd->in.len);
	f->off += s->len;
	ffint_setmax(f->size, f->off);
	return 0;
}

/** Add more data from user.  Flush to file, if necessary. */
static int fo_adddata(void *p, fcom_cmd *cmd)
{
	fout *f = p;
	ffstr s;

	if (f->fd == FF_BADFD) {
		return (cmd->flags & FCOM_CMD_FIRST) ? FCOM_DONE : FCOM_MORE;
	}

	if (cmd->out_seek) {
		cmd->out_seek = 0;

		if (f->buf.len != 0) {
			if (0 != fo_write(f, cmd, (void*)&f->buf))
				return FCOM_SYSERR;
			f->buf.len = 0;
		}

		dbglog(0, "seeking to %xU", cmd->output.offset);

		if (0 > fffile_seek(f->fd, cmd->output.offset, SEEK_SET)) {
			syserrlog("%s: %S", fffile_seek_S, &f->name);
			return FCOM_ERR;
		}
		f->off = cmd->output.offset;
	}

	for (;;) {

		size_t r = ffbuf_add(&f->buf, cmd->in.ptr, cmd->in.len, &s);
		ffstr_shift(&cmd->in, r);

		if (s.len == 0) {
			if (r != 0)
				f->stat.nmwrite++;
			if (!(cmd->flags & FCOM_CMD_FIRST))
				return FCOM_MORE;
			else if (f->buf.len == 0) {
				f->ok = 1;
				return FCOM_DONE;
			}

			ffstr_set2(&s, &f->buf);
			f->buf.len = 0;
		}

		if (0 != fo_write(f, cmd, &s))
			return FCOM_SYSERR;
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


#define FILT_NAME  "stdin"

typedef struct fistd {
	ffarr buf;
} fistd;

static void* fistd_open(fcom_cmd *cmd)
{
	fistd *f;
	if (NULL == (f = ffmem_new(fistd)))
		return FCOM_OPEN_SYSERR;

	if (NULL == ffarr_alloc(&f->buf, 64 * 1024)) {
		fistd_close(f, cmd);
		return FCOM_OPEN_SYSERR;
	}

	return f;
}

static void fistd_close(void *ctx, fcom_cmd *cmd)
{
	fistd *f = ctx;
	ffarr_free(&f->buf);
	ffmem_free(f);
}

static int fistd_read(void *ctx, fcom_cmd *cmd)
{
	fistd *f = ctx;
	size_t r;

	if (cmd->in_seek) {
		fcom_errlog(FILT_NAME, "can't seek on stdin.  offset:%U", cmd->input.offset);
		return FCOM_ERR;
	}

	r = ffstd_fread(ffstdin, f->buf.ptr, f->buf.cap);
	if (r == 0) {
		cmd->out.len = 0;
		cmd->in_last = 1;
		return FCOM_DONE;
	} else if ((ssize_t)r < 0) {
		fcom_syserrlog(FILT_NAME, "%s", fffile_read_S);
		return FCOM_ERR;
	}

	dbglog(0, "read %L bytes from stdin"
		, r);
	ffstr_set(&cmd->out, f->buf.ptr, r);
	return FCOM_DATA;
}

#undef FILT_NAME


static void* fostd_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void fostd_close(void *ctx, fcom_cmd *cmd)
{
}

static int fostd_write(void *ctx, fcom_cmd *cmd)
{
	size_t r;

	r = fffile_write(ffstdout, cmd->in.ptr, cmd->in.len);
	if (r != cmd->in.len)
		return FCOM_SYSERR;

	if (cmd->flags & FCOM_CMD_FIRST)
		return FCOM_DONE;

	return FCOM_MORE;
}
