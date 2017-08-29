/** File input/output.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/number.h>
#include <FF/path.h>
#include <FFOS/error.h>
#include <FFOS/file.h>
#include <FFOS/dir.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, __VA_ARGS__)

extern const fcom_core *core;

// MODULE
static int file_sig(uint signo);
static const void* file_iface(const char *name);
static int file_conf(const char *name, ffpars_ctx *ctx);
const fcom_mod file_mod = {
	.sig = &file_sig, .iface = &file_iface, .conf = &file_conf,
};

// OUTPUT
static int fo_conf(ffpars_ctx *ctx);
static void* fo_open(fcom_cmd *cmd);
static void fo_close(void *p, fcom_cmd *cmd);
static int fo_adddata(void *p, fcom_cmd *cmd);
static const fcom_filter fo_filt = {
	&fo_open, &fo_close, &fo_adddata,
};


static const void* file_iface(const char *name)
{
	if (ffsz_eq(name, "file-out"))
		return &fo_filt;
	return NULL;
}

static int file_conf(const char *name, ffpars_ctx *ctx)
{
	if (ffsz_eq(name, "file-out"))
		return fo_conf(ctx);
	return 1;
}

static int file_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT:
	case FCOM_SIGSTART:
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}


#define FILT_NAME  "file-out"

struct outconf {
	uint bufsize;
	uint prealloc;
	byte prealloc_grow;
	byte mkpath;
	byte del_on_err;
};
static struct outconf *outconf;

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

	if (cmd->read_only)
		return f;

	if (NULL == ffarr_alloc(&f->buf, outconf->bufsize))
		goto err;

	if (NULL == ffstr_alcopyz(&f->name, cmd->output.fn))
		goto err;

	const char *filename = cmd->output.fn;
	uint flags = (cmd->out_overwrite) ? O_CREAT : FFO_CREATENEW;
	flags |= O_WRONLY;
	mask = 0;
	canskip = 1;
	while (FF_BADFD == (f->fd = fffile_open(filename, flags))) {

		if (outconf->mkpath && fferr_nofile(fferr_last())) {
			if (0 != ffdir_make_path((void*)filename, 0)) {
				syserrlog("%s: for filename %s", ffdir_make_S, filename);
				goto err;
			}

			f->fd = fffile_open(filename, flags);
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

	f->mtime = cmd->output.mtime;

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

	if (f->fd != FF_BADFD) {

		if (!cmd->out_notrunc)
			fffile_trunc(f->fd, f->size);

		if (!f->ok && outconf->del_on_err) {

			if (0 != fffile_close(f->fd))
				syserrlog("%s", fffile_close_S);

			if (0 == fffile_rm(f->name.ptr))
				dbglog(0, "removed file %S", &f->name);

		} else {

			if (f->mtime.s != 0)
				fffile_settime(f->fd, &f->mtime);

			if (0 != fffile_close(f->fd))
				syserrlog("%s", fffile_close_S);

			fcom_verblog(FILT_NAME, "saved file %S, %Ukb written"
				, &f->name, (int64)ffmax(f->size / 1024, f->size != 0));
		}
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
	f->size = ffmax(f->size, f->off);
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
