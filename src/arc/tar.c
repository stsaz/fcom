/** .tar pack/unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/pack/tar.h>
#include <FF/path.h>
#include <FF/time.h>


extern const fcom_core *core;
extern const fcom_command *com;
extern int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);
extern int out_hlink(fcom_cmd *cmd, const char *target, const char *linkname);
extern int out_slink(fcom_cmd *cmd, const char *target, const char *linkname);


// TAR
static void* tar_open(fcom_cmd *cmd);
static void tar_close(void *p, fcom_cmd *cmd);
static int tar_process(void *p, fcom_cmd *cmd);
const fcom_filter tar_filt = { &tar_open, &tar_close, &tar_process };

// UNTAR
static void* untar_open(fcom_cmd *cmd);
static void untar_close(void *p, fcom_cmd *cmd);
static int untar_process(void *p, fcom_cmd *cmd);
const fcom_filter untar_filt = { &untar_open, &untar_close, &untar_process };

struct untar;
static void untar_showinfo(struct untar *t, const fftar_file *f);


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

	if (cmd->flags & FCOM_CMD_FWD) {
		if (cmd->in_last)
			fftar_fin(&t->tar);
		t->tar.in = cmd->in;
	}

again:
	switch ((enum E)t->state) {
	case R_EOF:
		if (cmd->in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%xU", cmd->input.offset);
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

		switch (f->type) {
		case FFTAR_FILE:
		case FFTAR_FILE0:
		case FFTAR_DIR:
		case FFTAR_HLINK:
		case FFTAR_SLINK:
			break;
		default:
			fcom_warnlog(FILT_NAME, "%s: unsupported file type '%c'", f->name, f->type);
			t->skipfile = 1;
			continue;
		}

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

		switch (f->type) {

		case FFTAR_HLINK:
			if (FCOM_DONE != (r = out_hlink(cmd, f->link_to, cmd->output.fn)))
				return r;
			t->skipfile = 1;
			continue;

		case FFTAR_SLINK:
			if (FCOM_DONE != (r = out_slink(cmd, "../r", cmd->output.fn)))
				return r;
			t->skipfile = 1;
			continue;
		}

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
		t->state = R_NEXT;
		if (t->skipfile) {
			t->skipfile = 0;
			goto again;
		}
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
