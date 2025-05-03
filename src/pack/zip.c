/** fcom: pack files into .zip
2022, Simon Zolin */

static const char* zip_help()
{
	return "\
Pack files into .zip.\n\
Usage:\n\
  `fcom zip` INPUT... [OPTIONS] [-C OUT_DIR] [-o OUTPUT.zip]\n\
\n\
OPTIONS:\n\
    `-m`, `--method` STR    Compression method:\n\
                          `deflate` (default), `zstd`, `store`\n\
    `-l`, `--level` INT     Compression level:\n\
                          deflate: 1..9; default:6\n\
                          zstd:   -7..22; default:3\n\
    `-j`, `--workers` INT   N of threads for compression (zstd)\n\
        `--each`          Separate archive per each input argument\n\
";
}

#include <fcom.h>
#include <ffpack/zip-write.h>
#include <ffsys/path.h>
#include <ffsys/globals.h>

const fcom_core *core;

struct zip {
	fcom_cominfo cominfo;

	uint			st;
	uint			stop;
	fcom_cominfo*	cmd;

	ffstr			iname, base;
	ffstr			plain;
	fcom_file_obj*	in;
	fffileinfo		fi;
	uint64			nfile;
	uint			in_file_notfound :1;
	uint			input_complete :1;

	ffzipwrite		wzip;
	ffstr			zipdata;
	char*			oname;
	fcom_file_obj*	out;
	int64			woff;
	uint			del_on_close :1;

	const fcom_hash *crc32;
	fcom_hash_obj *crc32_obj;

	uint	method;
	uint	comp_level;
	uint	comp_workers;
	u_char	each;
};

#define MIN_COMPRESS_SIZE 32

static int arg_method(void *obj, ffstr val)
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
	int i = ffslicestr_find(&s, &val);
	if (i < 0)
		return 0xbad;
	z->method = methods[i];
	return 0;
}

/**
*                        -> INPUT.zip
* -C OUT_DIR             -> OUT_DIR/INPUT.zip
* -C OUT_DIR -o FILE.zip -> OUT_DIR/FILE.zip
*            -o FILE.zip -> FILE.zip
*/
static char* out_name(struct zip *z, ffstr input, ffstr output, ffstr chdir)
{
	ffvec v = {};
	if (!output.len) {
		ffstr name;
		ffpath_splitpath_str(input, NULL, &name);
		if (chdir.len)
			ffvec_addfmt(&v, "%S%c", &chdir, FFPATH_SLASH);
		ffvec_addfmt(&v, "%S.zip%Z", &name);
	} else {
		if (chdir.len)
			ffvec_addfmt(&v, "%S%c", &chdir, FFPATH_SLASH);
		ffvec_addfmt(&v, "%S%Z", &output);
	}

	fcom_dbglog("output file: '%s'", v.ptr);
	return v.ptr;
}

#define O(member)  (void*)FF_OFF(struct zip, member)

static int args_parse(struct zip *z, fcom_cominfo *cmd)
{
	cmd->recursive = 1;
	z->method = ZIP_DEFLATED;
	z->comp_level = 0xff;

	static const struct ffarg args[] = {
		{ "--each",		'1',	O(each) },
		{ "--level",	'u',	O(comp_level) },
		{ "--method",	'S',	arg_method },
		{ "--workers",	'u',	O(comp_workers) },
		{ "-j", 		'u',	O(comp_workers) },
		{ "-l", 		'u',	O(comp_level) },
		{ "-m", 		'S',	arg_method },
		{}
	};
	int r = core->com->args_parse(cmd, args, z, FCOM_COM_AP_INOUT);
	if (r)
		return r;
	return 0;
}

#undef O

static void zip_close(fcom_op *op)
{
	struct zip *z = op;

	ffzipwrite_destroy(&z->wzip);
	if (z->del_on_close)
		core->file->del(z->oname, 0);
	core->file->destroy(z->out);
	ffmem_free(z->oname);

	core->file->destroy(z->in);

	if (z->crc32_obj)
		z->crc32->close(z->crc32_obj);
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
	fcom_cmd_file_conf(&fc, cmd);
	z->in = core->file->create(&fc);
	z->out = core->file->create(&fc);

	z->wzip.timezone_offset = core->tz.real_offset;
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


static int zip_input_next(struct zip *z)
{
	int r;
	if (0 > (r = core->com->input_next(z->cmd, &z->iname, &z->base, 0))) {
		if (r == FCOM_COM_RINPUT_NOMORE) {
			ffzipwrite_finish(&z->wzip);
			return 'done';
		}
		return 'erro';
	}
	return 0;
}

static int zip_input_info(struct zip *z)
{
	uint flags = fcom_file_cominfo_flags_i(z->cmd);
	flags |= FCOM_FILE_READ;
	int r = core->file->open(z->in, z->iname.ptr, flags);
	if (r == FCOM_FILE_ERR) {
		if (fferr_notexist(fferr_last())) {
			z->in_file_notfound = 1;
			return 'next';
		}
		return 'erro';
	}

	r = core->file->info(z->in, &z->fi);
	if (r == FCOM_FILE_ERR) return 'erro';

	if (core->com->input_allowed(z->cmd, z->iname, fffile_isdir(fffileinfo_attr(&z->fi)))) {
		return 'next';
	}

	if ((z->base.len == 0 || z->cmd->recursive)
		&& fffile_isdir(fffileinfo_attr(&z->fi))) {
		fffd fd = core->file->fd(z->in, FCOM_FILE_ACQUIRE);
		core->com->input_dir(z->cmd, fd);
	}
	return 0;
}

static int zip_read(struct zip *z, ffstr *output)
{
	switch (core->file->read(z->in, output, -1)) {
	case FCOM_FILE_ERR: return 'erro';
	case FCOM_FILE_ASYNC: return 'asyn';
	case FCOM_FILE_EOF:
		ffzipwrite_filefinish(&z->wzip);
	}
	return 0;
}

static int zip_file_write(struct zip *z, ffstr data)
{
	switch (core->file->write(z->out, data, z->woff)) {
	case FCOM_FILE_ERR: return 'erro';
	case FCOM_FILE_ASYNC: return 'asyn';
	}
	z->woff = -1;
	return 0;
}

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

static int zip_out_open(struct zip *z)
{
	uint flags = FCOM_FILE_WRITE;
	flags |= fcom_file_cominfo_flags_o(z->cmd);
	int r = core->file->open(z->out, z->oname, flags);
	if (r == FCOM_FILE_ERR) return 'erro';
	z->del_on_close = !z->cmd->stdout && !z->cmd->test;
	z->woff = -1;
	return 0;
}

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

static void zip_done(struct zip *z)
{
	core->file->close(z->out);
	ffmem_free(z->oname); z->oname = NULL;
	z->del_on_close = 0;

	ffzipwrite_destroy(&z->wzip);
	ffmem_zero_obj(&z->wzip);
	z->wzip.timezone_offset = core->tz.real_offset;
}

static void zip_run_each(struct zip *z)
{
	int r, rc = 1;
	enum { I_IN, I_OUT_OPEN, I_INFO, I_ADD, I_FILEREAD, I_PROC, I_WRITE, I_DONE, };

	while (!FFINT_READONCE(z->stop)) {
		switch (z->st) {

		case I_IN:
			switch (zip_input_next(z)) {
			case 'done':
				z->input_complete = 1;
				z->st = I_PROC;
				continue;
			case 'erro': goto end;
			}

			if (!z->base.len) {
				if (z->nfile != 0) {
					ffzipwrite_finish(&z->wzip);
					z->st = I_PROC;
					continue;
				}
				z->st = I_OUT_OPEN;
				continue;
			}

			z->st = I_INFO;
			continue;

		case I_OUT_OPEN:
			if (z->input_complete) {
				rc = z->in_file_notfound;
				goto end;
			}

			z->nfile++;
			z->oname = out_name(z, z->iname, FFSTR_Z(""), z->cmd->chdir);
			if (zip_out_open(z))
				goto end;
			z->st = I_INFO;
			// fallthrough

		case I_INFO:
			switch (zip_input_info(z)) {
			case 'erro': goto end;
			case 'next':
				z->st = I_IN;
				continue;
			}
			z->st = I_ADD;
			continue;

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
			switch (zip_read(z, &z->plain)) {
			case 'erro': goto end;
			case 'asyn':
				core->com->async(z->cmd);
				return;
			}
			z->st = I_PROC;
			// fallthrough

		case I_PROC:
			switch (zip_write(z, &z->plain, &z->zipdata)) {
			case FFZIPWRITE_MORE:
				z->st = I_FILEREAD; break;

			case FFZIPWRITE_DATA:
				z->st = I_WRITE; break;

			case FFZIPWRITE_FILEDONE:
				z->st = I_IN; break;

			case FFZIPWRITE_DONE:
				z->st = I_DONE; break;

			case FFZIPWRITE_ERROR:
				goto end;
			}
			continue;

		case I_WRITE:
			switch (zip_file_write(z, z->zipdata)) {
			case 'erro': goto end;
			case 'asyn':
				core->com->async(z->cmd);
				return;
			}
			z->st = I_PROC;
			continue;

		case I_DONE:
			zip_done(z);
			z->st = I_OUT_OPEN;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = z->cmd;
	zip_close(z);
	core->com->complete(cmd, rc);
	}
}

static void zip_run(fcom_op *op)
{
	struct zip *z = op;
	int r, rc = 1;
	enum { I_OUT_OPEN, I_IN, I_INFO, I_ADD, I_FILEREAD, I_PROC, I_WRITE, I_DONE, };

	if (z->each) {
		zip_run_each(z);
		return;
	}

	while (!FFINT_READONCE(z->stop)) {
		switch (z->st) {

		case I_OUT_OPEN:
			z->oname = out_name(z, ((ffstr*)z->cmd->input.ptr)[0], z->cmd->output, z->cmd->chdir);
			if (zip_out_open(z))
				goto end;
			z->st = I_IN;
			// fallthrough

		case I_IN:
			switch (zip_input_next(z)) {
			case 'done':
				z->st = I_PROC;
				continue;
			case 'erro': goto end;
			}
			z->st = I_INFO;
			// fallthrough

		case I_INFO:
			switch (zip_input_info(z)) {
			case 'erro': goto end;
			case 'next':
				z->st = I_IN;
				continue;
			}
			z->st = I_ADD;
			continue;

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
			switch (zip_read(z, &z->plain)) {
			case 'erro': goto end;
			case 'asyn':
				core->com->async(z->cmd);
				return;
			}
			z->st = I_PROC;
			// fallthrough

		case I_PROC:
			switch (zip_write(z, &z->plain, &z->zipdata)) {
			case FFZIPWRITE_MORE:
				z->st = I_FILEREAD; break;

			case FFZIPWRITE_DATA:
				z->st = I_WRITE; break;

			case FFZIPWRITE_FILEDONE:
				z->st = I_IN; break;

			case FFZIPWRITE_DONE:
				z->st = I_DONE; break;

			case FFZIPWRITE_ERROR:
				goto end;
			}
			continue;

		case I_WRITE:
			switch (zip_file_write(z, z->zipdata)) {
			case 'erro': goto end;
			case 'asyn':
				core->com->async(z->cmd);
				return;
			}
			z->st = I_PROC;
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
FF_EXPORT const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	zip_init, zip_destroy, zip_provide_op,
};
