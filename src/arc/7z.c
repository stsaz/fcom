/** .7z unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <arc/arc.h>
#include <ffpack/7zread.h>
#include <FF/time.h>


extern const fcom_core *core;
extern const fcom_command *com;
extern int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);


// UN7Z
static void* un7z_open(fcom_cmd *cmd);
static void un7z_close(void *p, fcom_cmd *cmd);
static int un7z_process(void *p, fcom_cmd *cmd);
const fcom_filter un7z_filt = { &un7z_open, &un7z_close, &un7z_process };

struct un7z;
static void un7z_showinfo(struct un7z *z, const ff7zread_fileinfo *f, fcom_cmd *cmd);


#define FILT_NAME  "arc.un7z"

enum {
	BUFSIZE = 64 * 1024,
};

typedef struct un7z {
	uint state;
	ff7zread z;
	ffstr in;
	ffarr fn;
	ffarr buf;
	const ff7zread_fileinfo *curfile;
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
	ff7zread_close(&z->z);
	ffmem_free(z);
}

static int un7z_process(void *p, fcom_cmd *cmd)
{
	un7z *z = p;
	int r;
	enum E { R_FIRST, R_NEXT, R_DATA, R_EOF, };

	switch ((enum E)z->state) {
	case R_EOF:
		ff7zread_close(&z->z);
		ffmem_tzero(&z->z);
		z->state = R_FIRST;
		//fall through

	case R_FIRST:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
			return FCOM_DONE;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		ff7zread_open(&z->z);
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
		ffstr_set2(&z->in, &cmd->in);
	}

	for (;;) {

	r = ff7zread_process(&z->z, &z->in, &cmd->out);
	switch (r) {

	case FF7ZREAD_FILEHEADER:
		for (;;) {
			const ff7zread_fileinfo *f;
			if (NULL == (f = ff7zread_nextfile(&z->z))) {
				z->state = R_EOF;
				return FCOM_MORE;
			}

			if (!arc_need_member(&cmd->members, 0, &f->name)) {
				continue;
			}

			z->curfile = f;

			if (fcom_logchk(core->conf->loglev, FCOM_LOGVERB))
				un7z_showinfo(z, f, cmd);
			if (cmd->show)
				continue;

			if ((f->attr & FFWIN_FILE_DIR) && f->size != 0)
				fcom_warnlog(FILT_NAME, "directory %s has non-zero size", f->name);

			if (cmd->output.fn == NULL) {
				if (FCOM_DATA != (r = fn_out(cmd, &f->name, &z->fn)))
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

	case FF7ZREAD_DATA:
		return FCOM_DATA;

	case FF7ZREAD_FILEDONE:
		z->state = R_NEXT;
		return FCOM_NEXTDONE;

	case FF7ZREAD_MORE:
		return FCOM_MORE;

	case FF7ZREAD_SEEK:
		cmd->input.offset = ff7zread_offset(&z->z);
		cmd->in_seek = 1;
		return FCOM_MORE;

	case FF7ZREAD_ERROR:
		fcom_errlog_ctx(cmd, FILT_NAME, "%s: %s"
			, (z->curfile != NULL) ? z->curfile->name.ptr : "", ff7zread_error(&z->z));
		return FCOM_ERR;
	}
	}
}

/* "size date name" */
static void un7z_showinfo(un7z *z, const ff7zread_fileinfo *f, fcom_cmd *cmd)
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

	if (!ffutf8_valid(f->name.ptr, f->name.len)) {
		ffssize r = ffutf8_from_cp(p, end - p, f->name.ptr, f->name.len, core->conf->codepage);
		if (r < 0) {
			fcom_errlog_ctx(cmd, "arc.un7z", "ffutf8_from_cp: %S", &f->name);
			return;
		}
	} else {
		p = ffs_copystr(p, end, &f->name);
	}

	fcom_verblog(FILT_NAME, "%*s", p - z->fn.ptr, z->fn.ptr);
}

#undef FILT_NAME
