/** fcom: .tar reader
2020, Simon Zolin
*/

#include <ffpack/tarread.h>
#include <FF/number.h>
#include <FF/path.h>
#include <FF/array.h>
#include <FF/time.h>

#define dbglog0(...)  fcom_dbglog(0, FILT_NAME, __VA_ARGS__)
#define warnlog(...)  fcom_warnlog(FILT_NAME, __VA_ARGS__)
#define errlog(...)  fcom_errlog(FILT_NAME, __VA_ARGS__)

struct untar;
static void untar_showinfo(struct untar *t, const fftarread_fileinfo_t *f);
static void untar_close(void *p, fcom_cmd *cmd);

#define FILT_NAME  "untar"

typedef struct untar {
	uint state;
	fftarread tar;
	ffarr fn;
	ffstr in;
	uint skipfile :1;
} untar;

static void* untar1_open(fcom_cmd *cmd)
{
	untar *t;
	if (NULL == (t = ffmem_new(untar)))
		return FCOM_OPEN_SYSERR;
	if (NULL == ffarr_alloc(&t->fn, 4096)) {
		untar_close(t, cmd);
		return FCOM_OPEN_SYSERR;
	}
	if (0 != fftarread_open(&t->tar)) {
		untar_close(t, cmd);
		return FCOM_OPEN_SYSERR;
	}

	return t;
}

static void untar1_close(void *p, fcom_cmd *cmd)
{
	untar *t = p;
	fftarread_close(&t->tar);
	ffarr_free(&t->fn);
	ffmem_free(t);
}

static int untar1_process(void *p, fcom_cmd *cmd)
{
	untar *t = p;
	int r;
	fftarread_fileinfo_t *f;
	enum E { R_DATA1, R_DATA, R_FDONE, R_EOF, };

	if (cmd->flags & FCOM_CMD_FWD) {
		t->in = cmd->in;
	}

again:
	switch ((enum E)t->state) {
	case R_DATA1:
		// 'untar' might have set 0x01 to prevent 'ungz1' filter from setting `output.fn`
		FF_CMPSET(&cmd->output.fn, (void*)1, NULL);
		t->state = R_DATA;
		// fallthrough

	case R_DATA:
		break;

	case R_FDONE:
		FF_CMPSET(&cmd->output.fn, t->fn.ptr, NULL);
		t->state = R_DATA;
		break;

	case R_EOF:
		if (cmd->in.len != 0) {
			fcom_warnlog(FILT_NAME, "unprocessed data at offset 0x%xU", cmd->input.offset);
			return FCOM_ERR;
		}
		return FCOM_OUTPUTDONE;
	}

	for (;;) {

		r = fftarread_process(&t->tar, &t->in, &cmd->out);
		switch (r) {

		case FFTARREAD_FILEHEADER:
			f = fftarread_fileinfo(&t->tar);

			if (!arc_need_member(&cmd->members, 0, &f->name)) {
				t->skipfile = 1;
				continue;
			}

			if (fcom_logchk(core->conf->loglev, FCOM_LOGVERB))
				untar_showinfo(t, f);
			if (cmd->show) {
				t->skipfile = 1;
				continue;
			}

			switch (f->type) {
			case TAR_FILE:
			case TAR_FILE0:
			case TAR_DIR:
			case TAR_HLINK:
			case TAR_SLINK:
				break;
			default:
				fcom_warnlog(FILT_NAME, "%s: unsupported file type '%c'", f->name, f->type);
				t->skipfile = 1;
				continue;
			}

			if (cmd->output.fn == NULL) {
				if (FCOM_DATA != (r = fn_out(cmd, &f->name, &t->fn)))
					return r;
				cmd->output.fn = t->fn.ptr;
			}
			cmd->output.size = f->size;
			cmd->output.mtime = f->mtime;
			cmd->output.attr = f->attr_unix & 0777;

			switch (f->type) {

			case TAR_HLINK:
				if (FCOM_DONE != (r = out_hlink(cmd, f->link_to, cmd->output.fn)))
					return r;
				t->skipfile = 1;
				continue;

			case TAR_SLINK:
				if (FCOM_DONE != (r = out_slink(cmd, f->link_to, cmd->output.fn)))
					return r;
				t->skipfile = 1;
				continue;
			}

			const char *filt = (f->type == TAR_DIR) ? "core.dir-out" : FCOM_CMD_FILT_OUT(cmd);
			com->ctrl(cmd, FCOM_CMD_FILTADD, filt);
			continue;

		case FFTARREAD_DATA:
			if (t->skipfile) {
				cmd->out.len = 0;
				continue;
			}
			return FCOM_DATA;

		case FFTARREAD_FILEDONE:
			t->state = R_FDONE;
			if (t->skipfile) {
				t->skipfile = 0;
				goto again;
			}
			return FCOM_NEXTDONE;

		case FFTARREAD_DONE:
			fftarread_close(&t->tar);
			ffmem_zero_obj(&t->tar);
			t->state = R_EOF;
			return FCOM_MORE;

		case FFTARREAD_MORE:
			return FCOM_MORE;

		case FFTARREAD_WARNING:
			fcom_warnlog_ctx(cmd, FILT_NAME, "%s near offset %U"
				, fftarread_error(&t->tar), fftarread_offset(&t->tar));
			break;
		case FFTARREAD_ERROR:
			fcom_errlog_ctx(cmd, FILT_NAME, "%s near offset %U"
				, fftarread_error(&t->tar), fftarread_offset(&t->tar));
			return FCOM_ERR;
		}
	}
}

/* "mode user group size date name" */
static void untar_showinfo(untar *t, const fftarread_fileinfo_t *f)
{
	char *p = t->fn.ptr, *end = ffarr_edge(&t->fn);

	p += fffile_unixattr_tostr(p, end - p, f->attr_unix);
	p += ffs_format(p, end - p, " %4u %4u %12u "
		, f->uid, f->gid, f->size);

	ffdatetime dt;
	fftime tm = f->mtime;
	tm.sec += FFTIME_1970_SECONDS + core->conf->tz.real_offset;
	fftime_split1(&dt, &tm);
	p += fftime_tostr1(&dt, p, end - p, FFTIME_DATE_YMD | FFTIME_HMS);
	p = ffs_copyc(p, end, ' ');

	p = ffs_copystr(p, end, &f->name);

	fcom_verblog(FILT_NAME, "%*s", p - (char*)t->fn.ptr, t->fn.ptr);
}

#undef FILT_NAME

const fcom_filter untar1_filt = { &untar1_open, &untar1_close, &untar1_process };
