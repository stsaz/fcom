/** fcom: unpack files from .zip
2022, Simon Zolin */

#include <fcom.h>
#include <ffpack/zipread.h>
#include <FFOS/path.h>

extern const fcom_core *core;

struct file {
	uint64 off;
	uint attr_unix, attr_win;
	uint64 zsize, size;
	fftime mtime;
};

#define f_isdir(f) \
	(((f)->attr_unix & FFFILE_UNIX_DIR) || ((f)->attr_win & FFFILE_WIN_DIR))

struct unzip {
	uint state;
	fcom_cominfo *cmd;
	ffzipread rzip;
	ffstr iname, base;
	ffstr zipdata, plain;
	fcom_file_obj *in, *out;
	char *oname;
	ffvec buf;
	uint stop;
	uint del_on_close :1;
	uint64 total_comp, total_uncomp;
	int64 roff;

	ffvec files; // struct file[]
	ffsize ifile;

	// conf:
	byte list;
};

static const char* unzip_help()
{
	return "\
Unpack files from .zip.\n\
Usage:\n\
  fcom unzip INPUT... [-C OUTPUT_DIR]\n\
    OPTIONS:\n\
    -l, --list      Just show the file list\n\
";
}

static int unzip_args_parse(struct unzip *z, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{ 'l', "list",	FFCMDARG_TSWITCH,	FF_OFF(struct unzip, list) },
		{}
	};
	int r = core->com->args_parse(cmd, args, z);
	if (r != 0)
		return r;

	if (!z->list && cmd->output.len != 0) {
		fcom_fatlog("Use -C to set output directory");
		return -1;
	}

	if (cmd->chdir.len == 0)
		ffstr_dupz(&cmd->chdir, ".");

	return 0;
}

static void unzip_log(void *udata, uint level, ffstr msg)
{
	fcom_dbglog("%S", &msg);
}

static void unzip_close(fcom_op *op)
{
	struct unzip *z = op;
	ffzipread_close(&z->rzip);
	core->file->destroy(z->in);
	if (z->del_on_close)
		core->file->delete(z->oname, 0);
	core->file->destroy(z->out);
	ffvec_free(&z->buf);
	ffmem_free(z->oname);
	ffmem_free(z);
}

static fcom_op* unzip_create(fcom_cominfo *cmd)
{
	struct unzip *z = ffmem_new(struct unzip);
	z->cmd = cmd;

	if (0 != unzip_args_parse(z, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	z->in = core->file->create(&fc);
	z->out = core->file->create(&fc);

	z->rzip.log = unzip_log;
	z->rzip.timezone_offset = core->tz.real_offset;

	ffsize cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&z->buf, cap, 1);
	return z;

end:
	unzip_close(z);
	return NULL;
}

/** Protect against division by zero. */
#define FFINT_DIVSAFE(val, by) \
({ \
	__typeof__(by) _by = (by); \
	((_by != 0) ? (val) / _by : 0); \
})

/* "size zsize(%) date name" */
static void unzip_showinfo(struct unzip *z, const ffzipread_fileinfo_t *zf)
{
	ffvec *b = &z->buf;
	b->len = 0;
	if (zip_fileinfo_isdir(zf)) {
		ffvec_addsz(b, "       <DIR>                  ");
	} else {
		uint percent = FFINT_DIVSAFE(zf->compressed_size * 100, zf->uncompressed_size);
		ffvec_addfmt(b, "%12U%12U(%3u%%)"
			, zf->uncompressed_size, zf->compressed_size, percent);
	}
	ffvec_addchar(b, ' ');

	ffdatetime dt;
	fftime m = zf->mtime;
	m.sec += FFTIME_1970_SECONDS + core->tz.real_offset;

	fftime_split1(&dt, &m);
	b->len += fftime_tostr1(&dt, ffslice_end(b, 1), ffvec_unused(b), FFTIME_DATE_YMD | FFTIME_HMS);
	ffvec_addchar(b, ' ');

	ffvec_addstr(b, &zf->name);

	fcom_infolog("%S", b);
}

static void unzip_f_info(struct unzip *z)
{
	const ffzipread_fileinfo_t *zf = ffzipread_fileinfo(&z->rzip);
	fcom_dbglog("CDIR header for %S: %U -> %U  attr:%xu/%xu"
		, &zf->name, zf->compressed_size, zf->uncompressed_size
		, zf->attr_unix, zf->attr_win);

	if (zip_fileinfo_isdir(zf)
		&& (zf->compressed_size != 0 || zf->uncompressed_size != 0))
		fcom_infolog("directory %S has non-zero size", &zf->name);

	// if (!members_check(&z->members, zf->name))
	// 	break;

	if (z->list) {
		unzip_showinfo(z, zf);
		z->total_comp += zf->compressed_size;
		z->total_uncomp += zf->uncompressed_size;
		return;
	}

	struct file *f = ffvec_pushT(&z->files, struct file);
	f->attr_unix = zf->attr_unix;
	f->attr_win = zf->attr_win;
	f->size = zf->uncompressed_size;
	f->zsize = zf->compressed_size;
	f->mtime = zf->mtime;
	f->off = zf->hdr_offset;
}

/* `d/f` -> `out/d/f` */
static char* unzip_outname(struct unzip *z, ffstr lname, ffstr rpath)
{
	return ffsz_allocfmt("%S%c%S%Z", &rpath, FFPATH_SLASH, &lname);
}

static int unzip_read(struct unzip *z, ffstr *input, ffstr *output)
{
	for (;;) {
		int r = ffzipread_process(&z->rzip, input, output);
		switch ((enum FFZIPREAD_R)r) {

		case FFZIPREAD_FILEINFO:
			unzip_f_info(z);
			break;

		case FFZIPREAD_FILEHEADER: {
			const ffzipread_fileinfo_t *zf = ffzipread_fileinfo(&z->rzip);
			fcom_dbglog("file header for %S", &zf->name);

			ffmem_free(z->oname);
			z->oname = unzip_outname(z, zf->name, z->cmd->chdir);
			return r;
		}

		case FFZIPREAD_FILEDONE: {
			const ffzipread_fileinfo_t *zf = ffzipread_fileinfo(&z->rzip);
			uint percent = (int)FFINT_DIVSAFE(z->rzip.file_rd * 100, z->rzip.file_wr);
			fcom_dbglog("%S: %U => %U (%u%%)"
				, &zf->name, z->rzip.file_rd, z->rzip.file_wr, percent);
			return r;
		}

		case FFZIPREAD_DONE:
		case FFZIPREAD_DATA:
		case FFZIPREAD_MORE:
			return r;

		case FFZIPREAD_SEEK:
			z->roff = ffzipread_offset(&z->rzip);
			return r;

		case FFZIPREAD_WARNING:
		case FFZIPREAD_ERROR:
			fcom_errlog("%s", ffzipread_error(&z->rzip));
			if (r == FFZIPREAD_ERROR)
				return r;
			break;
		}
	}
}

static void unzip_reset(struct unzip *z)
{
	z->total_uncomp = 0;
	z->total_comp = 0;
	z->ifile = 0;
	z->files.len = 0;
	z->roff = -1;
}

static void unzip_run(fcom_op *op)
{
	struct unzip *z = op;
	int r, rc = 1;
	enum { I_IN, I_INFO, I_PARSE, I_FILEREAD, I_OUT_OPEN, I_WRITE };

	while (!FFINT_READONCE(z->stop)) {
		switch (z->state) {

		case I_IN:
			if (0 > (r = core->com->input_next(z->cmd, &z->iname, &z->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					rc = 0;
				}
				goto end;
			}

			z->state = I_INFO;
			// fallthrough

		case I_INFO: {
			uint flags = fcom_file_cominfo_flags_i(z->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(z->in, z->iname.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;

			fffileinfo fi = {};
			r = core->file->info(z->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;

			if ((z->base.len == 0 || z->cmd->recursive)
					&& fffile_isdir(fffileinfo_attr(&fi))) {
				fffd fd = core->file->fd(z->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(z->cmd, fd);
			}

			// if (0 != core->com->input_allowed(z->cmd, z->iname)) {
			// 	z->state = ;
			// 	continue;
			// }

			unzip_reset(z);

			ffzipread_open(&z->rzip, fffileinfo_size(&fi));

			z->state = I_PARSE;
			continue;
		}

		case I_FILEREAD:
			r = core->file->read(z->in, &z->zipdata, z->roff);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) {
				fcom_errlog("incomplete archive");
				goto end;
			}
			z->roff = -1;
			z->state = I_PARSE;
			// fallthrough

		case I_PARSE:
			r = unzip_read(z, &z->zipdata, &z->plain);
			switch (r) {

			case FFZIPREAD_FILEDONE: {
				const struct file *f = ffslice_itemT(&z->files, z->ifile, struct file);
				z->total_comp += f->zsize;

				core->file->mtime_set(z->out, f->mtime);

				if (!f_isdir(f)) {
					uint attr = f->attr_unix;
#ifdef FF_WIN
					attr = f->attr_win;
#endif
					if (attr != 0)
						core->file->attr_set(z->out, attr);

					core->file->close(z->out);
					z->del_on_close = 0;
					fcom_verblog("unzip: %s", z->oname);
				}

				z->ifile++;
			}
				// fallthrough

			case FFZIPREAD_DONE: {
				if (z->ifile == z->files.len) {
					// no more files to unpack
					uint percent = FFINT_DIVSAFE(z->total_comp * 100, z->total_uncomp);
					fcom_verblog("%U <- %U(%u%%)"
						, z->total_uncomp, z->total_comp, percent);
					z->state = I_IN;
					continue;
				}

				const struct file *f = ffslice_itemT(&z->files, z->ifile, struct file);
				ffzipread_fileread(&z->rzip, f->off, f->zsize);
				continue;
			}

			case FFZIPREAD_MORE:
			case FFZIPREAD_SEEK:
				z->state = I_FILEREAD; continue;

			case FFZIPREAD_FILEHEADER:
				z->state = I_OUT_OPEN; continue;

			case FFZIPREAD_DATA:
				z->state = I_WRITE; continue;

			case FFZIPREAD_ERROR:
				goto end;
			}
			continue;

		case I_OUT_OPEN: {
			const struct file *f = ffslice_itemT(&z->files, z->ifile, struct file);

			if (f_isdir(f)) {
				r = core->file->dir_create(z->oname, FCOM_FILE_DIR_RECURSIVE);
				if (r == FCOM_FILE_ERR) goto end;
				z->state = I_PARSE;
				continue;
			}

			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(z->cmd);
			r = core->file->open(z->out, z->oname, flags);
			if (r == FCOM_FILE_ERR) goto end;

			core->file->trunc(z->out, f->size);

			z->del_on_close = !z->cmd->stdout && !z->cmd->test;
			z->state = I_PARSE;
			continue;
		}

		case I_WRITE:
			r = core->file->write(z->out, z->plain, -1);
			if (r == FCOM_FILE_ERR) goto end;
			z->total_uncomp += z->plain.len;
			z->state = I_PARSE;
			continue;
		}
	}

end:
	fcom_cominfo *cmd = z->cmd;
	unzip_close(z);
	core->com->complete(cmd, rc);
}

static void unzip_signal(fcom_op *op, uint signal)
{
	struct unzip *z = op;
	FFINT_WRITEONCE(z->stop, 1);
}

const fcom_operation fcom_op_unzip = {
	unzip_create, unzip_close,
	unzip_run, unzip_signal,
	unzip_help,
};
