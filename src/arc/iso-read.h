/** fcom: .iso reader
2021, Simon Zolin
*/

#include <ffpack/isoread.h>
#include <arc/arc.h>

#define dbglog0(...)  fcom_dbglog(0, FILT_NAME, __VA_ARGS__)

#define FILT_NAME  "arc.uniso"

struct uniso {
	uint state;
	ffstr in;
	ffisoread iso;
	ffarr fn;
	uint init :1;
};

static void uniso_showinfo(struct uniso *o, const ffisoread_fileinfo_t *f, uint show);

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
		ffisoread_close(&o->iso);
	ffarr_free(&o->fn);
	ffmem_free(o);
}

static void uniso_log(void *udata, uint level, ffstr msg)
{
	dbglog0("%S", &msg);
}

static int uniso_process(void *p, fcom_cmd *cmd)
{
	struct uniso *o = p;
	int r;
	ffisoread_fileinfo_t *f;
	enum R { R_FIRST, R_INIT, R_DATA, R_EOF, R_NEXT };

	if (cmd->flags & FCOM_CMD_FWD)
		o->in = cmd->in;

again:
	switch ((enum R)o->state) {
	case R_EOF:
		ffisoread_close(&o->iso);
		ffmem_zero_obj(&o->iso);
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
		ffisoread_init(&o->iso);
		o->iso.log = uniso_log;
		o->iso.udata = o;
		o->init = 1;
		o->state = R_DATA;
		break;

	case R_NEXT:
		FF_CMPSET(&cmd->output.fn, o->fn.ptr, NULL);
		if (NULL == (f = ffisoread_nextfile(&o->iso))) {
			o->state = R_EOF;
			return FCOM_MORE;
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

		ffisoread_readfile(&o->iso, f);
		o->state = R_DATA;
		break;

	case R_DATA:
		break;
	}

	for (;;) {

		r = ffisoread_process(&o->iso, &o->in, &cmd->out);
		switch (r) {

		case FFISOREAD_HDR:
			break;

		case FFISOREAD_FILEMETA:
			f = ffisoread_fileinfo(&o->iso);

			if (!arc_need_member(&cmd->members, 0, &f->name))
				continue;

			if (cmd->show || fcom_logchk(core->conf->loglev, FCOM_LOGVERB))
				uniso_showinfo(o, f, cmd->show);

			if (0 != ffisoread_storefile(&o->iso))
				return FCOM_ERR;
			continue;

		case FFISOREAD_LISTEND:
			if (cmd->show) {
				o->state = R_EOF;
				return FCOM_MORE;
			}
			o->state = R_NEXT;
			goto again;

		case FFISOREAD_DATA:
			return FCOM_DATA;

		case FFISOREAD_FILEDONE:
			o->state = R_NEXT;
			return FCOM_NEXTDONE;

		case FFISOREAD_MORE:
			return FCOM_MORE;

		case FFISOREAD_SEEK:
			fcom_cmd_seek(cmd, ffisoread_offset(&o->iso));
			return FCOM_MORE;

		case FFISOREAD_ERROR:
			fcom_errlog_ctx(cmd, FILT_NAME, "%s  offset:0x%xU", ffisoread_error(&o->iso), cmd->input.offset);
			return FCOM_ERR;

		default:
			FF_ASSERT(0);
			return FCOM_ERR;
		}
	}
}

/* "size date name" */
static void uniso_showinfo(struct uniso *o, const ffisoread_fileinfo_t *f, uint show)
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
		fcom_userlog("%*s", p - o->fn.ptr, o->fn.ptr);
	else
		fcom_verblog(FILT_NAME, "%*s", p - o->fn.ptr, o->fn.ptr);
}

#undef FILT_NAME
