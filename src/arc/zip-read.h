/** fcom: .zip reader
2020, Simon Zolin
*/

#include <ffpack/zipread.h>

#define dbglog0(...)  fcom_dbglog(0, FILT_NAME, __VA_ARGS__)
#define errlog(...)  fcom_errlog(FILT_NAME, __VA_ARGS__)

struct file {
	ffuint64 off;
	ffuint attr_unix, attr_win;
	ffuint64 compressed_size, uncompressed_size;
};

struct unzip1 {
	uint state;
	ffvec files; // struct file[]
	uint ifile;
	ffzipread zip;
	ffarr fn;
	ffstr in;
	uint64 total_comp, total_uncomp;
	struct members members;
};

#define FILT_NAME  "unzip"

static void zip_log(void *udata, uint level, ffstr msg)
{
	dbglog0("%S", &msg);
}

static void* unzip1_open(fcom_cmd *cmd)
{
	struct unzip1 *z = ffmem_new(struct unzip1);
	ffarr_alloc(&z->fn, 4096);
	ffzipread_open(&z->zip, cmd->input.size);
	z->zip.codepage = FFUNICODE_WIN1252;
	z->zip.log = zip_log;
	z->zip.timezone_offset = core->conf->tz.real_offset;
	cmd->skip_err = 1;

	members_optimize(&z->members, cmd->members);
	return z;
}

static void unzip1_close(void *p, fcom_cmd *cmd)
{
	struct unzip1 *z = p;
	ffvec_free(&z->files);
	ffarr_free(&z->fn);
	ffzipread_close(&z->zip);
	members_destroy(&z->members);
	ffmem_free(z);
}

/* "size csize(%) date name" */
static void unzip_showinfo(struct unzip1 *z, const ffzipread_fileinfo_t *f)
{
	char *p = z->fn.ptr, *end = ffarr_edge(&z->fn);
	int isdir = (f->attr_unix & FFFILE_UNIX_DIR) || (f->attr_win & FFFILE_WIN_DIR);

	if (isdir) {
		p = ffs_copy(p, end, "       <DIR>                  ", 12+12+6);
	} else {
		ffuint percent = (f->uncompressed_size != 0)
			? f->compressed_size * 100 / f->uncompressed_size
			: 0;
		p += ffs_format_r0(p, end - p, "%12u%12u(%3u%%)"
			, f->uncompressed_size, f->compressed_size, percent);
	}
	p = ffs_copyc(p, end, ' ');

	ffdatetime dt;
	fftime m = f->mtime;
	m.sec += FFTIME_1970_SECONDS + core->conf->tz.real_offset;

	fftime_split1(&dt, &m);
	p += fftime_tostr1(&dt, p, end - p, FFTIME_DATE_YMD | FFTIME_HMS);
	p = ffs_copyc(p, end, ' ');

	p = ffs_copystr(p, end, &f->name);

	fcom_userlog("%*s", p - (char*)z->fn.ptr, z->fn.ptr);
}

static int unzip1_process(void *p, fcom_cmd *cmd)
{
	struct unzip1 *z = p;
	enum { I_HDR, I_NEXTFILE, I_DATA };

	if (cmd->flags & FCOM_CMD_FWD) {
		z->in = cmd->in;
	}

again:
	switch (z->state) {
	case I_HDR:
		break;

	case I_NEXTFILE:
		FF_CMPSET(&cmd->output.fn, z->fn.ptr, NULL);
		if (z->ifile == z->files.len) {
			if (cmd->show) {
				ffuint percent = (z->total_uncomp != 0)
					? z->total_comp * 100 / z->total_uncomp
					: 0;
				fcom_userlog("%12u%12u(%3u%%)"
					, z->total_uncomp, z->total_comp, percent);
			}
			return FCOM_OUTPUTDONE;
		}
		struct file *of = ffslice_itemT(&z->files, z->ifile, struct file);
		ffzipread_fileread(&z->zip, of->off, of->compressed_size);
		z->state = I_DATA;
		break;

	case I_DATA:
		break;
	}

	for (;;) {

		int r = ffzipread_process(&z->zip, &z->in, &cmd->out);
		switch ((enum FFZIPREAD_R)r) {

		case FFZIPREAD_FILEINFO: {
			const ffzipread_fileinfo_t *f = ffzipread_fileinfo(&z->zip);
			dbglog0("CDIR header for %S: %U -> %U  unix_attr:%xu"
				, &f->name, f->compressed_size, f->uncompressed_size, f->attr_unix);

			int isdir = (f->attr_unix & FFFILE_UNIX_DIR) || (f->attr_win & FFFILE_WIN_DIR);
			if (isdir
				&& (f->compressed_size != 0 || f->uncompressed_size != 0))
				fcom_warnlog(FILT_NAME, "directory %S has non-zero size", &f->name);

			if (!members_check(&z->members, f->name))
				break;

			if (cmd->show || fcom_logchk(core->conf->loglev, FCOM_LOGVERB))
				unzip_showinfo(z, f);

			if (cmd->show) {
				z->total_comp += f->compressed_size;
				z->total_uncomp += f->uncompressed_size;
				break;
			}

			struct file *of = ffvec_pushT(&z->files, struct file);
			of->attr_unix = f->attr_unix;
			of->attr_win = f->attr_win;
			of->uncompressed_size = f->uncompressed_size;
			of->compressed_size = f->compressed_size;
			of->off = f->hdr_offset;
			break;
		}

		case FFZIPREAD_FILEHEADER: {
			struct file *of = ffslice_itemT(&z->files, z->ifile, struct file);
			const ffzipread_fileinfo_t *f = ffzipread_fileinfo(&z->zip);
			dbglog0("file header for %S", &f->name);

			if (cmd->output.fn == NULL) {
				if (FCOM_DATA != (r = fn_out(cmd, &f->name, &z->fn)))
					return r;
				cmd->output.fn = z->fn.ptr;
			}
			cmd->output.size = of->uncompressed_size;
			cmd->output.mtime = f->mtime;
#ifdef FF_WIN
			cmd->output.attr = of->attr_win;
#else
			cmd->output.attr = of->attr_unix;
#endif
			cmd->out_attr_win = 1;

			int isdir = (of->attr_unix & FFFILE_UNIX_DIR) || (of->attr_win & FFFILE_WIN_DIR);
			const char *filt = (isdir) ? "core.dir-out" : FCOM_CMD_FILT_OUT(cmd);
			com->ctrl(cmd, FCOM_CMD_FILTADD, filt);
			break;
		}

		case FFZIPREAD_DONE:
			z->state = I_NEXTFILE;
			goto again;

		case FFZIPREAD_FILEDONE: {
			z->state = I_NEXTFILE;
			z->ifile++;

			const ffzipread_fileinfo_t *f = ffzipread_fileinfo(&z->zip);
			dbglog0("%S: %U => %U (%u%%)"
				, &f->name, z->zip.file_rd, z->zip.file_wr
				, (int)FFINT_DIVSAFE(z->zip.file_rd * 100, z->zip.file_wr));
			return FCOM_NEXTDONE;
		}

		case FFZIPREAD_DATA:
			if (cmd->skip_err_active) {
				cmd->skip_err_active = 0;
				z->state = I_NEXTFILE;
				z->ifile++;
				return FCOM_NEXTDONE;
			}
			return FCOM_DATA;

		case FFZIPREAD_MORE:
			return FCOM_MORE;

		case FFZIPREAD_SEEK:
			cmd->input.offset = ffzipread_offset(&z->zip);
			cmd->in_seek = 1;
			return FCOM_MORE;

		case FFZIPREAD_WARNING:
		case FFZIPREAD_ERROR:
			fcom_errlog_ctx(cmd, FILT_NAME, "%s", ffzipread_error(&z->zip));
			return FCOM_ERR;
		}
	}
}

const fcom_filter unzip1_filt = { &unzip1_open, &unzip1_close, &unzip1_process };

#undef FILT_NAME
