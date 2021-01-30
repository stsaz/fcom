/** .iso pack/unpack.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <ffpack/isowrite.h>
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

#include <arc/iso-read.h>

#define FILT_NAME  "arc.iso"

struct iso {
	uint state;
	ffstr in;
	ffisowrite iso;
	ffarr fnames; //char*[]
	size_t cur_fname;
	char *volname;
};

static void* iso_open(fcom_cmd *cmd)
{
	struct iso *iso;
	if (NULL == (iso = ffmem_new(struct iso)))
		return FCOM_OPEN_SYSERR;
	if (0 != ffisowrite_create(&iso->iso, NULL, 0)) {
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

		ffisowrite_fileinfo_t f = {};
		ffstr_setz(&f.name, fn);
		f.size = fffile_infosize(&fi);
		f.mtime = fffile_infomtime(&fi);
		uint a = fffile_infoattr(&fi);
		if (fffile_isdir(a))
			f.attr = FFUNIX_FILE_DIR;
		ffisowrite_fileadd(&iso->iso, &f);
		goto again;
	}

	case W_EOF:
		FF_ASSERT(cmd->in.len == 0);
		iso->state = W_NEWFILE;
		//fall through

	case W_NEWFILE: {
		if (iso->cur_fname == iso->fnames.len) {
			ffisowrite_finish(&iso->iso);
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
		ffisowrite_filenext(&iso->iso);
		fcom_dbglog(0, FILT_NAME, "writing file data for %s", fn);
		iso->state = W_DATA;
		return FCOM_MORE;
	}

	case W_HDR:
	case W_DATA:
		break;
	}

	if (cmd->flags & FCOM_CMD_FWD)
		iso->in = cmd->in;

	for (;;) {

	r = ffisowrite_process(&iso->iso, &iso->in, &cmd->out);
	switch (r) {

	case FFISOWRITE_DATA:
		return FCOM_DATA;

	case FFISOWRITE_MORE:
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

	case FFISOWRITE_SEEK:
		fcom_cmd_outseek(cmd, ffisowrite_offset(&iso->iso));
		break;

	case FFISOWRITE_DONE:
		return FCOM_DONE;

	case FFISOWRITE_ERROR:
		fcom_errlog(FILT_NAME, "%s", ffisowrite_error(&iso->iso));
		return FCOM_ERR;
	}
	}
}

#undef FILT_NAME
