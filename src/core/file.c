/** fcom: core: fcom_file implementation
2022, Simon Zolin */

#include <fcom.h>
#include <core/fbuf.h>
#include <ffsys/std.h>
#include <ffsys/dir.h>
#include <ffsys/pipe.h>
#include <ffsys/perf.h>

extern fcom_core *core;
#define ALIGN (4*1024)

struct file {
	char *name;
	fffd fd;
	uint open_flags;
	uint buffer_size;
	fftime mtime;
	fffd fd_stdin, fd_stdout;

	struct fbufset fbuf;
	struct fbuf wbuf;
	uint64 size;
	uint64 cur_off;
	uint64 prealloc;

	struct {
		fftime t_read, t_write;
		uint64 total_read, total_write;
	} stats;
};

static int f_write(struct file *f, ffstr d, uint64 off);
static int file_flush(fcom_file_obj *_f, uint flags);

static fcom_file_obj* file_create(struct fcom_file_conf *conf)
{
	struct file *f = ffmem_new(struct file);
	f->fd = FFFILE_NULL;
	f->buffer_size = conf->buffer_size;
	if (f->buffer_size == 0)
		f->buffer_size = 64*1024;
	if (conf->n_buffers == 0)
		conf->n_buffers = 3;

	f->fd_stdin = ffstdin;
	f->fd_stdout = ffstdout;
	if (conf->fd_stdin != (fffd)0)
		f->fd_stdin = conf->fd_stdin;
	if (conf->fd_stdout != (fffd)0)
		f->fd_stdout = conf->fd_stdout;

	f->buffer_size = ffmax(f->buffer_size, ALIGN);
	fbufset_init(&f->fbuf, conf->n_buffers, f->buffer_size, ALIGN);
	f->wbuf.ptr = ffmem_align(f->buffer_size, ALIGN);
	f->wbuf.len = 0;
	f->wbuf.off = 0;
	return f;
}

static void mtime_set(struct file *f)
{
	if (f->mtime.sec == 0
		|| (f->open_flags & FCOM_FILE_STDOUT))
		return;

	fftime time1970 = f->mtime;
	time1970.sec -= FFTIME_1970_SECONDS;
	if (0 != fffile_set_mtime(f->fd, &time1970)) {
		fcom_syswarnlog("file set mtime: %s", f->name);
	} else if (core->debug) {
		char buf[128];
		ffdatetime dt;
		fftime_split1(&dt, &f->mtime);
		int r = fftime_tostr1(&dt, buf, sizeof(buf), FFTIME_YMD);
		buf[r] = '\0';
		fcom_dbglog("%s: mtime: %s", f->name, buf);
	}
}

static void prealloc_trunc(struct file *f)
{
	if (f->prealloc <= f->size) return;

	fcom_dbglog("%s: truncate: %U/%U", f->name, f->size, f->prealloc);

	fffd fd = f->fd;

#ifdef FF_WIN
	if ((f->open_flags & FCOM_FILE_DIRECTIO)
		&& (f->size % ALIGN) != 0) {
		if (FFFILE_NULL == (fd = fffile_open(f->name, FFFILE_WRITEONLY))) {
			fcom_syserrlog("%s: fffile_open", f->name);
			return;
		}
		fcom_dbglog("%s: opened without FFFILE_DIRECT", f->name);
	}
#endif

	if (0 != fffile_trunc(fd, f->size)) {
		fcom_syserrlog("%s: fffile_trunc", f->name);
		goto end;
	}
	f->prealloc = f->size;

end:
	if (fd != f->fd)
		fffile_close(fd);
}

static void file_close(fcom_file_obj *_f)
{
	struct file *f = _f;
	if (f->fd != FFFILE_NULL) {

		if (f->open_flags & (FCOM_FILE_WRITE | FCOM_FILE_READWRITE)) {
			if (FCOM_FILE_ASYNC == file_flush(f, 0))
				fcom_syserrlog("file_flush");
			prealloc_trunc(f);
			mtime_set(f);
		}

		if (!(f->open_flags & (FCOM_FILE_STDIN | FCOM_FILE_STDOUT))) {
			if (0 != fffile_close(f->fd)) {
				fcom_syserrlog("%s: fffile_close", f->name);
			} else {
				if (f->open_flags & (FCOM_FILE_WRITE | FCOM_FILE_READWRITE)) {
					fcom_dbglog("saved file: %s (%,U)  %UMB/sec | %UMB/sec"
						, f->name, f->size
						, FFINT_DIVSAFE(f->stats.total_read, fftime_to_usec(&f->stats.t_read))
						, FFINT_DIVSAFE(f->stats.total_write, fftime_to_usec(&f->stats.t_write))
						);
				} else {
					fcom_dbglog("read file: %s  %UMB/sec | %UMB/sec"
						, f->name
						, FFINT_DIVSAFE(f->stats.total_read, fftime_to_usec(&f->stats.t_read))
						, FFINT_DIVSAFE(f->stats.total_write, fftime_to_usec(&f->stats.t_write))
						);
				}
			}
		}

		f->fd = FFFILE_NULL;
	}

	ffmem_free(f->name); f->name = NULL;
}

static void file_destroy(fcom_file_obj *_f)
{
	struct file *f = _f;
	if (f == NULL)
		return;

	file_close(f);
	ffmem_alignfree(f->wbuf.ptr);
	fbufset_destroy(&f->fbuf);
	ffmem_free(f->name);
	ffmem_free(f);
}

static int file_open(fcom_file_obj *_f, const char *name, uint how)
{
	struct file *f = _f;
	file_close(f);
	f->open_flags = how;

	fbufset_reset(&f->fbuf);
	f->size = 0;
	f->prealloc = 0;
	f->cur_off = 0;
	ffmem_zero_obj(&f->mtime);

	if (how & FCOM_FILE_STDIN) {
		fcom_dbglog("file: using stdin");
		f->fd = f->fd_stdin;
		return FCOM_FILE_OK;
	}
	if (how & FCOM_FILE_STDOUT) {
		fcom_dbglog("file: using stdout");
		f->open_flags |= FCOM_FILE_WRITE | FCOM_FILE_NO_PREALLOC;
		f->fd = f->fd_stdout;
		return FCOM_FILE_OK;
	}

	f->name = ffsz_dup(name);

	if (how & FCOM_FILE_FAKEWRITE)
		return FCOM_FILE_OK;

	uint flags = FFFILE_READONLY;
	if ((how & 3) == FCOM_FILE_WRITE)
		flags = FFFILE_WRITEONLY;
	else if ((how & 3) == FCOM_FILE_READWRITE)
		flags = FFFILE_READWRITE;

	if (how & FCOM_FILE_CREATE)
		flags |= FFFILE_CREATE;
	else if (how & FCOM_FILE_CREATENEW)
		flags |= FFFILE_CREATENEW;

	if (how & FCOM_FILE_DIRECTIO)
		flags |= FFFILE_DIRECT;

#ifdef FF_WIN
	if (how & FCOM_FILE_READAHEAD)
		flags |= FILE_FLAG_SEQUENTIAL_SCAN;
#endif

	flags |= FFFILE_NOATIME;

	for (uint i = 0;  ;  i++) {
		if (FFFILE_NULL == (f->fd = fffile_open(f->name, flags))) {

#ifdef FF_UNIX
			if (fferr_last() == EINVAL && (flags & FFFILE_DIRECT)) {
				fcom_dbglog("directio: EINVAL");
				flags &= ~FFFILE_DIRECT;
				if (how & FCOM_FILE_CREATENEW) {
					flags &= ~FFFILE_CREATENEW;
					flags |= FFFILE_CREATE;
				}
				continue;
			}
#endif

			if (i == 0 && fferr_notexist(fferr_last())
				&& (how & (FCOM_FILE_WRITE | FCOM_FILE_READWRITE))) {
				if (0 != ffdir_make_path(f->name, 0)) {
					fcom_syserrlog("ffdir_make_path: for filename %s", f->name);
					return FCOM_FILE_ERR;
				}
				continue;
			}

			fcom_syserrlog("file open: %s", f->name);
			return FCOM_FILE_ERR;
		}
		break;
	}

	if ((how & FCOM_FILE_DIRECTIO) && !(flags & FFFILE_DIRECT)) {
		fcom_infolog("%s: opened without DIRECT_IO flag", f->name);
		f->open_flags &= ~FCOM_FILE_DIRECTIO;
	}

	fcom_dbglog("%s: opened file", f->name);
	return FCOM_FILE_OK;
}

static int f_benchmark(fftime *t)
{
	if (core->verbose) {
		*t = fftime_monotonic();
		return 1;
	}
	return 0;
}

static int file_read(fcom_file_obj *_f, ffstr *d, int64 off)
{
	struct file *f = _f;
	struct fbuf *b;

	if (off == -1)
		off = f->cur_off;

	if (NULL != (b = fbufset_find(&f->fbuf, off))) {
		fcom_dbglog("%s: @%U: cache hit: %L @%U", f->name, off, b->len, b->off);
		goto done;
	}

	b = fbufset_nextbuf(&f->fbuf);

	fftime t1, t2 = {};
	f_benchmark(&t1);

	ffuint64 offset = off;
	ffssize r;
	if (f->open_flags & FCOM_FILE_STDIN) {
		if ((uint64)off != f->cur_off) {
			fcom_errlog("invalid seeking on stdin");
			return FCOM_FILE_ERR;
		}

		r = ffpipe_read(f->fd, b->ptr, f->buffer_size);
	} else {
		offset = ffint_align_floor2(off, ALIGN);
		r = fffile_readat(f->fd, b->ptr, f->buffer_size, offset);
	}

	if (r < 0 && fferr_again(fferr_last()))
		return FCOM_FILE_ASYNC;

	if (r < 0) {
		fcom_syserrlog("file read: %s", f->name);
		return FCOM_FILE_ERR;
	}

	if (f_benchmark(&t2)) {
		fftime_sub(&t2, &t1);
		fftime_add(&f->stats.t_read, &t2);
		f->stats.total_read += b->len;
	}

	if (r < f->buffer_size)
		f->size = offset + r;
	b->off = offset;
	b->len = r;
	fcom_dbglog("%s: read %L @%U", f->name, b->len, b->off);

done:
	ffstr_set(d, b->ptr, b->len);
	ffstr_shift(d, off - b->off);
	f->cur_off = b->off + b->len;
	if (d->len == 0)
		return FCOM_FILE_EOF;
	return FCOM_FILE_OK;
}

/** Pass data to kernel */
static int f_write(struct file *f, ffstr d, uint64 off)
{
	ffsize len = d.len;
	ffssize r;

	fftime t1, t2 = {};
	f_benchmark(&t1);

	if (f->open_flags & FCOM_FILE_STDOUT) {
		if (off != f->size) {
			fcom_errlog("invalid seeking on stdout");
			return FCOM_FILE_ERR;
		}

		r = ffpipe_write(f->fd, d.ptr, d.len);
	} else {
		if (f->open_flags & FCOM_FILE_DIRECTIO) {
			len = ffint_align_ceil2(d.len, ALIGN);
			ffmem_fill(d.ptr + d.len, 0x00, len - d.len); // zero the trailer
		}
		r = fffile_writeat(f->fd, d.ptr, len, off);
	}

	if (r < 0 && fferr_again(fferr_last()))
		return FCOM_FILE_ASYNC;

	if (r < 0) {
		fcom_syserrlog("file write: %s %L @%U", f->name, len, off);
		return FCOM_FILE_ERR;
	}

	if (f_benchmark(&t2)) {
		fftime_sub(&t2, &t1);
		fftime_add(&f->stats.t_write, &t2);
		f->stats.total_write += len;
	}

	fcom_dbglog("%s: written %L @%U", f->name, len, off);
	if (off + d.len > f->size)
		f->size = off + d.len;
	if (f->prealloc < off + len)
		f->prealloc = off + len;
	return 0;
}

static int file_flush(fcom_file_obj *_f, uint flags)
{
	struct file *f = _f;
	ffstr d;
	ffstr_set(&d, f->wbuf.ptr, f->wbuf.len);
	if (d.len != 0) {
		int r = f_write(f, d, f->wbuf.off);
		if (r != 0)
			return r;
		f->wbuf.len = 0;
		f->wbuf.off = 0;
	}
	return 0;
}

static int file_write(fcom_file_obj *_f, ffstr data, int64 off)
{
	struct file *f = _f;
	if (f->open_flags & FCOM_FILE_FAKEWRITE)
		return FCOM_FILE_OK;

	if (off == -1)
		off = f->cur_off;

	if (!(f->open_flags & FCOM_FILE_NO_PREALLOC) && f->prealloc < off + data.len) {
		f->prealloc = ffint_align_power2(off + data.len);
		if (0 != fffile_trunc(f->fd, f->prealloc)) {
			fcom_syswarnlog("fffile_trunc: %s", f->name);
			f->open_flags |= FCOM_FILE_NO_PREALLOC;
		}
	}

	for (;;) {
		ffstr d;
		ffsize n = data.len;
		ffint64 woff = fbuf_write(&f->wbuf, f->buffer_size, &data, off, &d);
		off += n - data.len;
		if (n != data.len) {
			fcom_dbglog("%s: write: cached %L bytes @%U+%L", f->name, n - data.len, f->wbuf.off, f->wbuf.len);
		}
		if (woff < 0) {
			break;
		}

		int r;
		if (0 != (r = f_write(f, d, woff)))
			return r;

		f->wbuf.len = 0;
		f->wbuf.off = 0;
	}

	f->cur_off = off;
	return FCOM_FILE_OK;
}

static int file_write_fmt(fcom_file_obj *_f, const char *fmt, ...)
{
	ffstr s = {};
	ffsize cap = 0;
	va_list va;
	va_start(va, fmt);
	ffstr_growfmtv(&s, &cap, fmt, va);
	va_end(va);

	int r = file_write(_f, s, -1);
	ffstr_free(&s);
	return r;
}

static int file_trunc(fcom_file_obj *_f, int64 size)
{
	struct file *f = _f;
	if (f->open_flags & FCOM_FILE_NO_PREALLOC)
		return 0;

	if (size == -1)
		size = f->cur_off;
	f->prealloc = size;

	if (0 != fffile_trunc(f->fd, f->prealloc)) {
		fcom_syswarnlog("fffile_trunc: %s", f->name);
		f->open_flags |= FCOM_FILE_NO_PREALLOC;
	}
	fcom_dbglog("%s: truncate: %U", f->name, f->prealloc);
	return 0;
}

static int file_behaviour(fcom_file_obj *_f, uint flags)
{
	struct file *f = _f;
	switch (flags) {
	case FCOM_FBEH_SEQ:
		fcom_dbglog("%s: sequential access", f->name);
		f->size = fffile_size(f->fd);
		if (0 != fffile_readahead(f->fd, f->size))
			fcom_dbglog("file read ahead: %E", fferr_last());
		break;

	case FCOM_FBEH_RANDOM:
		fcom_dbglog("%s: random access", f->name);
		if (0 != fffile_readahead(f->fd, -1))
			fcom_dbglog("file read ahead: %E", fferr_last());
		break;

	case FCOM_FBEH_TRUNC_PREALLOC:
		if (FCOM_FILE_ASYNC == file_flush(f, 0))
			fcom_syserrlog("file_flush");
		prealloc_trunc(f);
		break;
	}
	return 0;
}

static fffd file_fd(fcom_file_obj *_f, uint flags)
{
	struct file *f = _f;
	fffd fd = f->fd;
	if (flags & FCOM_FILE_ACQUIRE)
		f->fd = FFFILE_NULL;
	return fd;
}

static int file_info(fcom_file_obj *_f, fffileinfo *fi)
{
	struct file *f = _f;

	if (f->open_flags & FCOM_FILE_INFO_NOFOLLOW) {
		if (0 != fffile_info_linkpath(f->name, fi)) {
			fcom_syserrlog("file get info: %s", f->name);
			return FCOM_FILE_ERR;
		}
		return 0;
	}

	if (0 != fffile_info(f->fd, fi)) {
		fcom_syserrlog("file get info: %s", f->name);
		return FCOM_FILE_ERR;
	}
	return 0;
}

static int file_mtime_set(fcom_file_obj *_f, fftime mtime)
{
	struct file *f = _f;
	if (f->open_flags & FCOM_FILE_FAKEWRITE)
		return FCOM_FILE_OK;

	f->mtime = mtime;
	return 0;
}

static int file_attr_set(fcom_file_obj *_f, uint attr)
{
	struct file *f = _f;
	if (f->open_flags & FCOM_FILE_FAKEWRITE)
		return FCOM_FILE_OK;

	if (0 != fffile_set_attr(f->fd, attr)) {
		fcom_syserrlog("fffile_set_attr: %s", f->name);
		return FCOM_FILE_ERR;
	}

	fcom_dbglog("%s: attr: %xu", f->name, attr);
	return 0;
}

static int dir_create(const char *name, uint flags)
{
	int r = ffdir_make(name);
	if (r) {
		if (fferr_exist(fferr_last())) {
			fcom_dbglog("%s: directory already exists", name);
			return 0;
		}

		if (flags & FCOM_FILE_DIR_RECURSIVE) {
			char *sz = ffsz_dup(name);
			r = ffdir_make_all(sz, 0);
			ffmem_free(sz);
		}

		if (r) {
			fcom_syserrlog("directory create: %s", name);
			return FCOM_FILE_ERR;
		}
	}
	fcom_verblog("%s: created directory", name);
	return 0;
}

static int hlink_create(const char *oldpath, const char *newpath, uint flags)
{
	if (0 != fffile_hardlink(oldpath, newpath)) {
		fcom_syserrlog("fffile_hardlink(): %s -> %s"
			, newpath, oldpath);
		return FCOM_FILE_ERR;
	}

	fcom_verblog("created hard link: %s -> %s"
		, newpath, oldpath);
	return 0;
}

static int slink_create(const char *target, const char *linkpath, uint flags)
{
	int r = fffile_symlink(target, linkpath);

	if (r != 0 && (flags & FCOM_FILE_CREATE)) {
		int e = fferr_last();
		if (0 != fffile_remove(linkpath)) {
			fcom_syswarnlog("remove: %s", linkpath);
			fferr_set(e);
		} else {
			r = fffile_symlink(target, linkpath);
		}
	}

	if (r != 0) {
		fcom_syserrlog("fffile_symlink(): %s -> %s"
			, linkpath, target);
		return FCOM_FILE_ERR;
	}

	fcom_verblog("created symbolic link: %s -> %s"
		, linkpath, target);
	return 0;
}

static int file_move(ffstr old, ffstr _new, uint flags)
{
	int rc = FCOM_FILE_ERR;
	char *src = ffsz_dupstr(&old);
	char *dst = ffsz_dupstr(&_new);

	if (flags & FCOM_FILE_MOVE_SAFE) {
		fffd fd = fffile_open(dst, FFFILE_READONLY);
		if (fd != FFFILE_NULL) {
			fffile_close(fd);
			fcom_errlog("move: target file already exists: %S", &_new);
			goto end;
		}
	}

	int r = fffile_rename(src, dst);
	if (r != 0 && fferr_notexist(fferr_last())) {
		if (0 != ffdir_make_path(dst, 0)) {
			fcom_syserrlog("ffdir_make_path: for filename %s", dst);
			goto end;
		}
		r = fffile_rename(src, dst);
	}
	if (r != 0) {
		fcom_syserrlog("move: %S -> %S", &old, &_new);
		goto end;
	}

	fcom_verblog("moved: %S -> %S", &old, &_new);
	rc = 0;

end:
	ffmem_free(src);
	ffmem_free(dst);
	return rc;
}

static int file_delete(const char *name, uint flags)
{
	if (0 != fffile_remove(name)) {
		fcom_syserrlog("file delete: %s", name);
		return -1;
	}
	fcom_verblog("file deleted: %s", name);
	return 0;
}

const fcom_file _fcom_file = {
	file_create,
	file_destroy,
	file_open, file_close,
	file_read, file_write, file_write_fmt,
	file_flush,
	file_trunc,
	file_behaviour,
	file_info, file_fd,
	file_mtime_set, file_attr_set,
	dir_create,
	hlink_create, slink_create,
	file_move,
	file_delete,
};
