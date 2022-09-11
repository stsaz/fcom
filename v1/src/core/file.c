/** fcom: core: fcom_file implementation
2022, Simon Zolin */

#include <fcom.h>
#include <core/fcache.h>
#include <FFOS/std.h>
#include <FFOS/dir.h>

extern fcom_core *core;
#define ALIGN (4*1024)

struct file {
	const char *name;
	fffd fd;
	uint open_flags;
	uint buffer_size;

	struct fcache fcache;
	uint64 size;
	uint64 cur_off;
	uint64 prealloc;
};

static fcom_file_obj* file_create(struct fcom_file_conf *conf)
{
	struct file *f = ffmem_new(struct file);
	f->fd = FFFILE_NULL;
	f->buffer_size = conf->buffer_size;
	if (f->buffer_size == 0)
		f->buffer_size = 64*1024;
	if (conf->n_buffers == 0)
		conf->n_buffers = 3;
	fcache_init(&f->fcache, conf->n_buffers, f->buffer_size, ALIGN);
	return f;
}

static void prealloc_trunc(struct file *f)
{
	if (f->prealloc > f->size) {
		if (0 != fffile_trunc(f->fd, f->size))
			fcom_syserrlog("%s: fffile_trunc", f->name);
		else
			f->prealloc = f->size;
	}
}

static void file_destroy(fcom_file_obj *_f)
{
	struct file *f = _f;
	if (f == NULL)
		return;

	if (f->fd != FFFILE_NULL) {
		prealloc_trunc(f);

		if (0 != fffile_close(f->fd))
			fcom_syserrlog("%s: fffile_close", f->name);
	}

	fcache_destroy(&f->fcache);
	ffmem_free(f);
}

static int file_open(fcom_file_obj *_f, const char *static_name_ptr, uint how)
{
	struct file *f = _f;
	f->name = static_name_ptr;
	f->open_flags = how;

	fcache_reset(&f->fcache);
	f->size = 0;
	f->prealloc = 0;
	f->cur_off = 0;

	if (how & FCOM_FILE_STDIN) {
		fcom_dbglog("file: using stdin");
		f->fd = ffstdin;
		return FCOM_FILE_OK;
	}
	if (how & FCOM_FILE_STDOUT) {
		fcom_dbglog("file: using stdout");
		f->open_flags |= FCOM_FILE_NO_PREALLOC;
		f->fd = ffstdout;
		return FCOM_FILE_OK;
	}

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

	for (uint i = 0;  ;  i++) {
		if (FFFILE_NULL == (f->fd = fffile_open(f->name, flags))) {
			if (i == 0 && fferr_last() == EINVAL && (flags & FFFILE_DIRECT)) {
				flags &= ~FFFILE_DIRECT;
				continue;
			}
			fcom_syserrlog("file open: %s", f->name);
			return FCOM_FILE_ERR;
		}
		break;
	}

	if ((how & FCOM_FILE_DIRECTIO) && !(flags & FFFILE_DIRECT)) {
		fcom_infolog("%s: opened without DIRECT_IO flag", f->name);
	}

	fcom_dbglog("%s: opened file", f->name);
	return FCOM_FILE_OK;
}

static void file_close(fcom_file_obj *_f)
{
	struct file *f = _f;
	if (f->fd != FFFILE_NULL) {
		fffile_close(f->fd);
		f->fd = FFFILE_NULL;
	}
}

static int file_read(fcom_file_obj *_f, ffstr *d, int64 off)
{
	struct file *f = _f;
	struct fcache_buf *b;
	if (NULL != (b = fcache_find(&f->fcache, off))) {
		goto done;
	}

	if (off == -1)
		off = f->cur_off;

	if (off >= f->size)
		return FCOM_FILE_EOF;

	b = fcache_nextbuf(&f->fcache);
	ffuint64 offset = ffint_align_floor2(off, ALIGN);

	ffssize r = fffile_readat(f->fd, b->ptr, f->buffer_size, offset);
	if (r < 0) {
		fcom_syserrlog("file read: %s", f->name);
		return FCOM_FILE_ERR;
	}
	if (r < f->buffer_size)
		f->size = offset + r;
	b->off = offset;
	b->len = r;

done:
	fcom_dbglog("%s: read %L @%U", f->name, b->len, b->off);
	ffstr_set(d, b->ptr, b->len);
	ffstr_shift(d, off - b->off);
	f->cur_off = b->off + b->len;
	if (b->len == 0)
		return FCOM_FILE_EOF;
	return FCOM_FILE_OK;
}

static int file_write(fcom_file_obj *_f, ffstr d, int64 off)
{
	struct file *f = _f;
	if (f->open_flags & FCOM_FILE_FAKEWRITE)
		return FCOM_FILE_OK;

	if (off == -1)
		off = f->cur_off;

	if (!(f->open_flags & FCOM_FILE_NO_PREALLOC) && f->prealloc < off + d.len) {
		f->prealloc = ffint_align_power2(off + d.len);
		if (0 != fffile_trunc(f->fd, f->prealloc))
			fcom_dbglog("fffile_trunc: %E", fferr_last());
	}

	ffssize r;
	if (f->open_flags & FCOM_FILE_STDOUT)
		r = fffile_write(f->fd, d.ptr, d.len);
	else
		r = fffile_writeat(f->fd, d.ptr, d.len, off);
	if (r < 0) {
		fcom_syserrlog("file write: %s", f->name);
		return FCOM_FILE_ERR;
	}
	fcom_dbglog("%s: written %L @%U", f->name, d.len, off);

	if (off + d.len > f->size)
		f->size = off + d.len;
	f->cur_off = off + d.len;
	return FCOM_FILE_OK;
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
	if (0 != fffile_info(f->fd, fi)) {
		fcom_syserrlog("file get info: %s", f->name);
		return FCOM_FILE_ERR;
	}
	return 0;
}

static int file_mtime(fcom_file_obj *_f, fftime *mtime)
{
	struct file *f = _f;
	fffileinfo fi = {};
	if (0 != fffile_info(f->fd, &fi)) {
		fcom_syserrlog("file get info: %s", f->name);
		return FCOM_FILE_ERR;
	}
	*mtime = fffileinfo_mtime(&fi);
	return 0;
}

static int file_mtime_set(fcom_file_obj *_f, fftime *mtime)
{
	struct file *f = _f;
	if (f->open_flags & FCOM_FILE_FAKEWRITE)
		return FCOM_FILE_OK;

	if (0 != fffile_set_mtime(f->fd, mtime)) {
		fcom_syserrlog("file set mtime: %s", f->name);
		return FCOM_FILE_ERR;
	}
	return 0;
}

static int dir_create(const char *name, uint flags)
{
	int r = ffdir_make(name);
	if (r < 0) {
		if (fferr_exist(fferr_last())) {
			fcom_dbglog("%s: directory already exists", name);
			return 0;
		}
		fcom_syserrlog("directory create: %s", name);
		return FCOM_FILE_ERR;
	}
	fcom_verblog("%s: created directory", name);
	return 0;
}

static int file_move(ffstr old, ffstr _new, uint flags)
{
	int rc = FCOM_FILE_ERR;
	char *src = ffsz_dupstr(&old);
	char *dst = ffsz_dupstr(&_new);

	if (1) {
		fffd fd = fffile_open(dst, FFFILE_READONLY);
		if (fd != FFFILE_NULL) {
			fffile_close(fd);
			fcom_errlog("move: target file already exists: %S", &_new);
			goto end;
		}
	}

	int r = fffile_rename(src, dst);
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

const fcom_file _fcom_file = {
	file_create,
	file_destroy,
	file_open, file_close,
	file_read, file_write,
	file_behaviour,
	file_info, file_fd,
	file_mtime, file_mtime_set,
	dir_create,
	file_move,
};
