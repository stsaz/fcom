/** .7z unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <arc/arc.h>
#include <FF/pack/7z.h>


extern const fcom_core *core;
extern const fcom_command *com;
extern int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);


// UN7Z
static void* un7z_open(fcom_cmd *cmd);
static void un7z_close(void *p, fcom_cmd *cmd);
static int un7z_process(void *p, fcom_cmd *cmd);
const fcom_filter un7z_filt = { &un7z_open, &un7z_close, &un7z_process };

struct un7z;
static void un7z_showinfo(struct un7z *z, const ff7zfile *f);


#define FILT_NAME  "arc.un7z"

enum {
	BUFSIZE = 64 * 1024,
};

typedef struct un7z {
	uint state;
	ff7z z;
	ffarr fn;
	ffarr buf;
	const ff7zfile *curfile;
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

			ffstr fn;
			ffstr_setz(&fn, f->name);
			if (!arc_need_member(&cmd->members, 0, &fn)) {
				continue;
			}

			z->curfile = f;

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
		fcom_errlog_ctx(cmd, FILT_NAME, "%s: %s"
			, (z->curfile != NULL) ? z->curfile->name : "", ff7z_errstr(&z->z));
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
