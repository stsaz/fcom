/** .iso pack/unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/pack/iso.h>
#include <FF/path.h>
#include <FF/time.h>


extern const fcom_core *core;
extern const fcom_command *com;
extern int fn_out(fcom_cmd *cmd, const ffstr *input, ffarr *buf);


// ISO
static void* iso_open(fcom_cmd *cmd);
static void iso_close(void *p, fcom_cmd *cmd);
static int iso_process(void *p, fcom_cmd *cmd);
const fcom_filter iso_filt = { &iso_open, &iso_close, &iso_process };

// UNISO
static void* uniso_open(fcom_cmd *cmd);
static void uniso_close(void *p, fcom_cmd *cmd);
static int uniso_process(void *p, fcom_cmd *cmd);
const fcom_filter uniso_filt = { &uniso_open, &uniso_close, &uniso_process };

struct uniso;
static void uniso_showinfo(struct uniso *o, const ffiso_file *f, uint show);


#define FILT_NAME  "arc.iso"

struct iso {
	uint state;
	ffiso_cook iso;
	ffarr fnames; //char*[]
	size_t cur_fname;
	char *volname;
};

static void* iso_open(fcom_cmd *cmd)
{
	struct iso *iso;
	if (NULL == (iso = ffmem_new(struct iso)))
		return FCOM_OPEN_SYSERR;
	if (0 != ffiso_wcreate(&iso->iso, 0)) {
		ffmem_free(iso);
		return FCOM_OPEN_SYSERR;
	}
	return iso;
}

static void iso_close(void *p, fcom_cmd *cmd)
{
	struct iso *iso = p;
	ffarr_free(&iso->fnames);
	ffmem_free(iso->volname);
	ffmem_free(iso);
}

static int iso_process(void *p, fcom_cmd *cmd)
{
	struct iso *iso = p;
	int r;
	const char *fn;
	enum E { W_NEXT, W_NEWFILE, W_HDR, W_DATA, W_EOF };

again:
	switch ((enum E)iso->state) {

	case W_NEXT: {
		if (NULL == (fn = com->arg_next(cmd, 0))) {
			const char *filt = FCOM_CMD_FILT_OUT(cmd);
			com->ctrl(cmd, FCOM_CMD_FILTADD, filt);
			iso->state = W_HDR;

			ffstr fn, name;
			ffstr_setz(&fn, cmd->output.fn);
			ffpath_split3(fn.ptr, fn.len, NULL, &name, NULL);
			if (NULL == (iso->volname = ffsz_alcopystr(&name)))
				return FCOM_SYSERR;
			iso->iso.name = iso->volname;
			goto again;
		}
		const char **p = (void*)ffarr_pushgrowT(&iso->fnames, 64, char*);
		*p = fn;

		fffileinfo fi;
		if (0 != fffile_infofn(fn, &fi))
			return FCOM_SYSERR;

		ffiso_file f = {};
		ffstr_setz(&f.name, fn);
		f.size = fffile_infosize(&fi);
		f.mtime = fffile_infomtime(&fi);
		uint a = fffile_infoattr(&fi);
		if (fffile_isdir(a))
			f.attr = FFUNIX_FILE_DIR;
		ffiso_wfile(&iso->iso, &f);
		goto again;
	}

	case W_EOF:
		FF_ASSERT(cmd->in.len == 0);
		iso->state = W_NEWFILE;
		//fall through

	case W_NEWFILE: {
		if (iso->cur_fname == iso->fnames.len) {
			ffiso_wfinish(&iso->iso);
			iso->state = W_DATA;
			goto again;
		}
		fn = *ffarr_itemT(&iso->fnames, iso->cur_fname++, char*);

		fffileinfo fi;
		if (0 != fffile_infofn(fn, &fi))
			return FCOM_SYSERR;
		uint a = fffile_infoattr(&fi);
		if (fffile_isdir(a))
			goto again;

		cmd->input.fn = fn;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		ffiso_wfilenext(&iso->iso);
		iso->state = W_DATA;
		return FCOM_MORE;
	}

	case W_HDR:
	case W_DATA:
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD)
		ffiso_input(&iso->iso, cmd->in.ptr, cmd->in.len);

	for (;;) {

	r = ffiso_write(&iso->iso);
	switch (r) {

	case FFISO_DATA:
		cmd->out = ffiso_output(&iso->iso);
		return FCOM_DATA;

	case FFISO_MORE:
		if (cmd->flags & FCOM_CMD_FIRST) {
			// 'file-in' filter has already returned the last block of data
			iso->state = W_EOF;
			goto again;
		}
		if (cmd->flags & FCOM_CMD_FWD) {
			if (cmd->in_last) {
				iso->state = W_EOF;
				return FCOM_MORE;
			}
		}
		if (iso->state == W_HDR) {
			iso->state = W_NEWFILE;
			goto again;
		}
		return FCOM_MORE;

	case FFISO_SEEK:
		fcom_cmd_outseek(cmd, ffiso_woffset(&iso->iso));
		break;

	case FFISO_DONE:
		return FCOM_DONE;

	case FFISO_ERR:
		fcom_errlog(FILT_NAME, "%s", ffiso_werrstr(&iso->iso));
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME


#define FILT_NAME  "arc.uniso"

struct uniso {
	uint state;
	ffiso iso;
	ffarr fn;
	uint init :1;
};

static void* uniso_open(fcom_cmd *cmd)
{
	struct uniso *o;
	if (NULL == (o = ffmem_new(struct uniso)))
		return NULL;

	if (NULL == ffarr_alloc(&o->fn, 4096))
		goto err;

	return o;

err:
	uniso_close(o, cmd);
	return FCOM_OPEN_SYSERR;
}

static void uniso_close(void *p, fcom_cmd *cmd)
{
	struct uniso *o = p;
	if (o->init)
		ffiso_close(&o->iso);
	ffarr_free(&o->fn);
	ffmem_free(o);
}

static int uniso_process(void *p, fcom_cmd *cmd)
{
	struct uniso *o = p;
	int r;
	ffiso_file *f;
	enum E { R_FIRST, R_INIT, R_DATA, R_EOF, R_NEXT };

	if (cmd->flags & FCOM_CMD_FWD)
		ffiso_input(&o->iso, cmd->in.ptr, cmd->in.len);

again:
	switch ((enum E)o->state) {
	case R_EOF:
		ffiso_close(&o->iso);
		ffmem_tzero(&o->iso);
		o->init = 0;
		if (!(cmd->flags & FCOM_CMD_FWD))
			return FCOM_MORE;
		o->state = R_FIRST;
		//fall through

	case R_FIRST:
		if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
			return FCOM_FIN;
		com->ctrl(cmd, FCOM_CMD_FILTADD_PREV, FCOM_CMD_FILT_IN(cmd));
		o->state = R_INIT;
		return FCOM_MORE;

	case R_INIT:
		ffiso_init(&o->iso);
		o->init = 1;
		o->state = R_DATA;
		break;

	case R_NEXT:
		FF_CMPSET(&cmd->output.fn, o->fn.ptr, NULL);
		if (NULL == (f = ffiso_nextfile(&o->iso))) {
			o->state = R_EOF;
			return FCOM_MORE;
		}

		if (cmd->members.len != 0) {
			if (0 > ffs_findarrz((void*)cmd->members.ptr, cmd->members.len, f->name.ptr, f->name.len))
				goto again;
		}

		if (cmd->output.fn == NULL) {
			if (FCOM_DATA != (r = fn_out(cmd, &f->name, &o->fn)))
				return r;
			cmd->output.fn = o->fn.ptr;
		}
		cmd->output.size = f->size;
		cmd->output.mtime = f->mtime;

		const char *filt = ffiso_file_isdir(f) ? "core.dir-out" : FCOM_CMD_FILT_OUT(cmd);
		com->ctrl(cmd, FCOM_CMD_FILTADD, filt);

		ffiso_readfile(&o->iso, f);
		o->state = R_DATA;
		break;

	case R_DATA:
		break;
	}

	for (;;) {

	r = ffiso_read(&o->iso);
	switch (r) {

	case FFISO_HDR:
		break;

	case FFISO_FILEMETA:
		f = ffiso_getfile(&o->iso);

		if (cmd->members.len != 0) {
			ffbool skip = 0;
			if (0 > ffs_findarrz((void*)cmd->members.ptr, cmd->members.len, f->name.ptr, f->name.len))
				skip = 1;
			else {
				if (cmd->show || fcom_logchk(core->conf->loglev, FCOM_LOGVERB))
					uniso_showinfo(o, f, cmd->show);
			}
			if (!skip || ffiso_file_isdir(f)) {
				if (0 != ffiso_storefile(&o->iso))
					return FCOM_ERR;
			}
			continue;
		}

		if (cmd->show || fcom_logchk(core->conf->loglev, FCOM_LOGVERB))
			uniso_showinfo(o, f, cmd->show);

		if (0 != ffiso_storefile(&o->iso))
			return FCOM_ERR;
		continue;

	case FFISO_LISTEND:
		if (cmd->show) {
			o->state = R_EOF;
			return FCOM_MORE;
		}
		o->state = R_NEXT;
		goto again;

	case FFISO_DATA:
		cmd->out = ffiso_output(&o->iso);
		return FCOM_DATA;

	case FFISO_FILEDONE:
		o->state = R_NEXT;
		return FCOM_NEXTDONE;

	case FFISO_MORE:
		return FCOM_MORE;

	case FFISO_SEEK:
		fcom_cmd_seek(cmd, ffiso_offset(&o->iso));
		return FCOM_MORE;

	case FFISO_ERR:
		fcom_errlog(FILT_NAME, "%s  offset:0x%xU", ffiso_errstr(&o->iso), cmd->input.offset);
		return FCOM_ERR;

	default:
		FF_ASSERT(0);
		return FCOM_ERR;
	}
	}
}

/* "size date name" */
static void uniso_showinfo(struct uniso *o, const ffiso_file *f, uint show)
{
	char *p = o->fn.ptr, *end = ffarr_edge(&o->fn);

	if (ffiso_file_isdir(f))
		p = ffs_copy(p, end, "       <DIR>", 12);
	else
		p += ffs_fromint(f->size, p, end - p, FFINT_WIDTH(12));
	p = ffs_copyc(p, end, ' ');

	ffdtm dt;
	fftime_split(&dt, &f->mtime, FFTIME_TZLOCAL);
	p += fftime_tostr(&dt, p, end - p, FFTIME_DATE_YMD | FFTIME_HMS);
	p = ffs_copyc(p, end, ' ');

	p = ffs_copystr(p, end, &f->name);

	if (show)
		fcom_infolog(FILT_NAME, "%*s", p - o->fn.ptr, o->fn.ptr);
	else
		fcom_verblog(FILT_NAME, "%*s", p - o->fn.ptr, o->fn.ptr);
}

#undef FILT_NAME
