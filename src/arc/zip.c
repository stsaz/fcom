/** .zip pack/unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/pack/zip.h>
#include <FF/number.h>
#include <FF/time.h>


extern const fcom_core *core;
extern const fcom_command *com;
extern int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);


// ZIP
static void* zip_open(fcom_cmd *cmd);
static void zip_close(void *p, fcom_cmd *cmd);
static int zip_process(void *p, fcom_cmd *cmd);
const fcom_filter zip_filt = { &zip_open, &zip_close, &zip_process };

// UNZIP
static void* unzip_open(fcom_cmd *cmd);
static void unzip_close(void *p, fcom_cmd *cmd);
static int unzip_process(void *p, fcom_cmd *cmd);
const fcom_filter unzip_filt = { &unzip_open, &unzip_close, &unzip_process };

struct unzip;
static void unzip_showinfo(struct unzip *z, const ffzip_file *f);


#define FILT_NAME  "arc.zip"

enum {
	BUFSIZE = 64 * 1024,
};

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
		fcom_verblog(FILT_NAME, "%s: %U => %U (%u%%)"
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
	uint member_wildcard :1;
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

	const char **pm;
	FFARR2_WALK(&cmd->members, pm) {
		size_t n = ffsz_len(*pm);
		if (*pm + n != ffs_findof(*pm, n, "*?", 2)) {
			z->member_wildcard = 1;
			break;
		}
	}

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

	if (cmd->flags & FCOM_CMD_FWD) {
		z->zip.in = cmd->in;
	}

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

			if (z->member_wildcard) {
				const char **pm;
				ffstr fn;
				ffstr_setz(&fn, f->fn);
				ffbool match = 0;
				FFARR2_WALK(&cmd->members, pm) {
					if (!ffs_wildcard(*pm, ffsz_len(*pm), fn.ptr, fn.len, 0)) {
						match = 1;
						break;
					}
				}
				if (!match)
					continue;

			} else if (cmd->members.len != 0
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
