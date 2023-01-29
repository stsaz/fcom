/** fcom: pack files into .zip
2022, Simon Zolin */

#include <fcom.h>
#include <ffpack/zipwrite.h>
#include <FFOS/path.h>
#include <FFOS/ffos-extern.h>

const fcom_core *core;

struct zip {
	uint st;
	fcom_cominfo *cmd;
	ffzipwrite wzip;
	ffstr iname, base;
	ffstr plain, zipdata;
	fcom_file_obj *in, *out;
	fffileinfo fi;
	ffvec buf;
	uint stop;
	uint in_file_notfound :1;
	uint del_on_close :1;
	int64 woff;

	const fcom_hash *crc32;
	fcom_hash_obj *crc32_obj;

	uint method;
	byte comp_level;
	byte comp_workers;
};

#define MIN_COMPRESS_SIZE 32

static const char* zip_help()
{
	return "\
Pack files into .zip.\n\
Implies '--recursive'.\n\
Usage:\n\
  fcom zip INPUT... [OPTIONS] -o OUTPUT.zip\n\
    OPTIONS:\n\
    -m, --method=STR    Compression method:\n\
                          deflate (default), zstd, store\n\
    -l, --level=INT     Compression level:\n\
                          deflate: 1..9; default:6\n\
                          zstd:   -7..22; default:3\n\
    -j, --workers=INT   N of threads for compression (zstd)\n\
";
}

static int arg_method(ffcmdarg_scheme *as, void *obj, ffstr *val)
{
	struct zip *z = obj;
	static const ffstr methods_str[] = {
		FFSTR_INITZ("store"), FFSTR_INITZ("deflate"), FFSTR_INITZ("zstd"),
	};
	static const uint methods[] = {
		ZIP_STORED, ZIP_DEFLATED, ZIP_ZSTANDARD,
	};
	ffslice s;
	ffslice_set(&s, methods_str, FF_COUNT(methods_str));
	int i = ffslicestr_find(&s, val);
	if (i < 0)
		return 0xbad;
	z->method = methods[i];
	return 0;
}

#define O(member)  FF_OFF(struct zip, member)

static int args_parse(struct zip *z, fcom_cominfo *cmd)
{
	cmd->recursive = 1;
	z->method = ZIP_DEFLATED;
	z->comp_level = 0xff;

	static const ffcmdarg_arg args[] = {
		{ 'm', "method",	FFCMDARG_TSTR,	(ffsize)arg_method },
		{ 'l', "level",	FFCMDARG_TINT8,	O(comp_level) },
		{ 'j', "workers",	FFCMDARG_TINT8,	O(comp_workers) },
		{}
	};
	int r = core->com->args_parse(cmd, args, z);
	if (r != 0)
		return r;

	if (cmd->output.len == 0) {
		fcom_fatlog("Use --out to set output file name");
		return -1;
	}

	return 0;
}

#undef O

static void zip_close(fcom_op *op)
{
	struct zip *z = op;
	ffzipwrite_destroy(&z->wzip);
	core->file->destroy(z->in);
	if (z->del_on_close)
		core->file->delete(z->cmd->output.ptr, 0);
	core->file->destroy(z->out);
	z->crc32->close(z->crc32_obj);
	ffvec_free(&z->buf);
	ffmem_free(z);
}

static fcom_op* zip_create(fcom_cominfo *cmd)
{
	struct zip *z = ffmem_new(struct zip);
	z->cmd = cmd;

	if (0 != args_parse(z, cmd))
		goto end;

	if (NULL == (z->crc32 = core->com->provide("zip.fcom_crc32", 0)))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	z->in = core->file->create(&fc);
	z->out = core->file->create(&fc);

	z->wzip.timezone_offset = core->tz.real_offset;

	ffsize cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&z->buf, cap, 1);
	return z;

end:
	zip_close(z);
	return NULL;
}


static int zip_crc32_process(void *obj, ffzipwrite *w, ffstr *input, ffstr *output)
{
	struct zip *z = w->udata;
	*output = *input;
	z->crc32->update(z->crc32_obj, input->ptr, input->len);
	z->crc32->fin(z->crc32_obj, (byte*)&w->crc, 4);
	ffstr_shift(input, input->len);
	return 0;
}

static const ffzipwrite_filter zip_crc32_filter = {
	NULL, NULL, zip_crc32_process
};


static int zip_file_add(struct zip *z)
{
	ffzipwrite_conf conf = {};
	conf.name = z->iname;
	conf.mtime = fffileinfo_mtime(&z->fi);

#ifdef FF_WIN
	conf.attr_win = fffileinfo_attr(&z->fi);
#else
	conf.attr_unix = fffileinfo_attr(&z->fi);
	conf.uid = z->fi.st_uid;
	conf.gid = z->fi.st_gid;
#endif

	z->crc32_obj = z->crc32->create();
	conf.crc32_filter = &zip_crc32_filter;

	conf.compress_method = z->method;
	if (fffileinfo_size(&z->fi) < MIN_COMPRESS_SIZE)
		conf.compress_method = ZIP_STORED;

	if (z->comp_level != 0xff) {
		conf.deflate_level = z->comp_level;
		conf.zstd_level = z->comp_level;
	}

	conf.zstd_workers = z->comp_workers;

	int r;
	if (0 != (r = ffzipwrite_fileadd(&z->wzip, &conf))) {
		if (r == -2)
			return -2;
		fcom_errlog("ffzipwrite_fileadd: %s", ffzipwrite_error(&z->wzip));
		return -1;
	}

	z->wzip.udata = z;
	return 0;
}

/** Protect against division by zero. */
#define FFINT_DIVSAFE(val, by) \
	((by) != 0 ? (val) / (by) : 0)

static int zip_write(struct zip *z, ffstr *in, ffstr *out)
{
	for (;;) {
		int r = ffzipwrite_process(&z->wzip, in, out);
		switch (r) {

		case FFZIPWRITE_FILEDONE: {
			z->crc32->close(z->crc32_obj);
			z->crc32_obj = NULL;

			ffzipwrite *wz = &z->wzip;
			fcom_verblog("%s: %U => %U (%u%%)"
				, z->iname.ptr, wz->file_rd, wz->file_wr
				, (uint)FFINT_DIVSAFE(wz->file_wr * 100, wz->file_rd));
			break;
		}

		case FFZIPWRITE_SEEK:
			z->woff = ffzipwrite_offset(&z->wzip);
			continue;

		case FFZIPWRITE_ERROR:
			fcom_errlog("ffzipwrite_process: %s", ffzipwrite_error(&z->wzip));
		}
		return r;
	}
}

static void zip_run(fcom_op *op)
{
	struct zip *z = op;
	int r, rc = 1;
	enum { I_OUT_OPEN, I_IN, I_INFO, I_ADD, I_FILEREAD, I_PROC, I_DONE, };

	while (!FFINT_READONCE(z->stop)) {
		switch (z->st) {

		case I_OUT_OPEN: {
			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(z->cmd);
			r = core->file->open(z->out, z->cmd->output.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;
			z->del_on_close = !z->cmd->stdout && !z->cmd->test;
			z->woff = -1;
			z->st = I_IN;
		}
			// fallthrough

		case I_IN:
			if (0 > (r = core->com->input_next(z->cmd, &z->iname, &z->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					ffzipwrite_finish(&z->wzip);
					z->st = I_PROC;
					continue;
				}
				goto end;
			}

			z->st = I_INFO;
			// fallthrough

		case I_INFO: {
			uint flags = fcom_file_cominfo_flags_i(z->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(z->in, z->iname.ptr, flags);
			if (r == FCOM_FILE_ERR) {
				if (fferr_notexist(fferr_last())) {
					z->in_file_notfound = 1;
					z->st = I_IN;
					continue;
				}
				goto end;
			}

			r = core->file->info(z->in, &z->fi);
			if (r == FCOM_FILE_ERR) goto end;

			if ((z->base.len == 0 || z->cmd->recursive)
				&& fffile_isdir(fffileinfo_attr(&z->fi))) {
				fffd fd = core->file->fd(z->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(z->cmd, fd);
			}

			if (0 != core->com->input_allowed(z->cmd, z->iname)) {
				z->st = I_IN;
				continue;
			}

			z->st = I_ADD;
			continue;
		}

		case I_ADD:
			if (0 != (r = zip_file_add(z))) {
				if (r == -2) {
					z->st = I_IN;
					continue;
				}
				goto end;
			}

			if (fffile_isdir(fffileinfo_attr(&z->fi))) {
				ffzipwrite_filefinish(&z->wzip);
				z->st = I_PROC;
				continue;
			}

			z->st = I_FILEREAD;
			// fallthrough

		case I_FILEREAD:
			r = core->file->read(z->in, &z->plain, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF)
				ffzipwrite_filefinish(&z->wzip);
			z->st = I_PROC;
			// fallthrough

		case I_PROC:
			r = zip_write(z, &z->plain, &z->zipdata);
			switch (r) {
			case FFZIPWRITE_MORE:
				z->st = I_FILEREAD; break;

			case FFZIPWRITE_DATA:
				r = core->file->write(z->out, z->zipdata, z->woff);
				if (r == FCOM_FILE_ERR) goto end;
				z->woff = -1;
				break;

			case FFZIPWRITE_FILEDONE:
				z->st = I_IN; break;

			case FFZIPWRITE_DONE:
				z->st = I_DONE;
				break;

			case FFZIPWRITE_ERROR:
				goto end;
			}
			continue;

		case I_DONE:
			core->file->close(z->out);
			z->del_on_close = 0;
			rc = z->in_file_notfound;
			goto end;
		}
	}

end:
	{
	fcom_cominfo *cmd = z->cmd;
	zip_close(z);
	core->com->complete(cmd, rc);
	}
}

static void zip_signal(fcom_op *op, uint signal)
{
	struct zip *z = op;
	FFINT_WRITEONCE(z->stop, 1);
}

static const fcom_operation fcom_op_zip = {
	zip_create, zip_close,
	zip_run, zip_signal,
	zip_help,
};


static void zip_init(const fcom_core *_core) { core = _core; }
static void zip_destroy() {}
extern const fcom_operation fcom_op_unzip;
static const fcom_operation* zip_provide_op(const char *name)
{
	if (ffsz_eq(name, "zip"))
		return &fcom_op_zip;
	else if (ffsz_eq(name, "unzip"))
		return &fcom_op_unzip;
	return NULL;
}
FF_EXP const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	zip_init, zip_destroy, zip_provide_op,
};
