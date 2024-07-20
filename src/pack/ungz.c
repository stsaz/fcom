/** fcom: unpack file from .gz
2023, Simon Zolin */

static const char* ungz_help()
{
	return "\
Decompress file from .gz.\n\
Usage:\n\
  `fcom ungz` INPUT [OPTIONS] [-o OUTPUT]\n\
";
}

#include <fcom.h>
#include <ffsys/path.h>
#include <ffpack/gzread.h>

extern const fcom_core *core;

struct ungz {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	ffstr iname, basename;
	char *oname;
	fcom_file_obj *in, *out;
	uint64 in_off;
	ffgzread ungz;
	ffstr data, zdata;
	fftime mtime;
	uint stop;
	uint ungz_opened :1;
	uint next_chunk_begin :1;
	uint out_opened :1;
	uint del_on_close :1;

	uint64 in_total, out_total;
};

static int args_parse(struct ungz *z, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{}
	};
	int r = core->com->args_parse(cmd, args, z, FCOM_COM_AP_INOUT);
	if (r != 0)
		return r;

	if (cmd->chdir.len == 0)
		ffstr_dupz(&cmd->chdir, ".");
	return 0;
}

static void ungz_close(fcom_op *op)
{
	struct ungz *z = op;
	ffgzread_close(&z->ungz);
	core->file->destroy(z->in);
	if (z->del_on_close)
		core->file->del(z->oname, 0);
	core->file->destroy(z->out);
	ffmem_free(z->oname);
	ffmem_free(z);
}

static fcom_op* ungz_create(fcom_cominfo *cmd)
{
	struct ungz *z = ffmem_new(struct ungz);
	z->cmd = cmd;

	if (0 != args_parse(z, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fcom_cmd_file_conf(&fc, cmd);
	z->in = core->file->create(&fc);
	z->out = core->file->create(&fc);
	return z;

end:
	ungz_close(z);
	return NULL;
}

/** Prepare output file name
idir/iname.gz -> odir/gzname (from .gz)
idir/iname.gz -> odir/iname */
static char* out_name(struct ungz *z, ffstr gzname, ffstr in, ffstr base)
{
	char *ofn = NULL;
	if (z->cmd->output.len != 0) {
		ofn = ffsz_dup(z->cmd->output.ptr);

	} else {
		ffpath_splitpath_win(gzname.ptr, gzname.len, NULL, &gzname);
		if (gzname.len != 0) {
			in = gzname;
		} else {
			ffpath_splitpath_str(in, NULL, &in);
			ffstr name, ext;
			ffpath_splitname_str(in, &name, &ext);
			if (ffstr_eqz(&ext, "gz"))
				in = name;
		}
		ofn = ffsz_allocfmt("%S%c%S"
			, &z->cmd->chdir, FFPATH_SLASH, &in);
	}

	fcom_dbglog("output file name: %s", ofn);
	return ofn;
}

static int gzread(struct ungz *z, ffstr *in, ffstr *out)
{
	if (!z->ungz_opened) {
		z->ungz_opened = 1;
		if (0 != ffgzread_open(&z->ungz, -1)) {
			fcom_errlog("ffgzread_open");
			return 'erro';
		}
	}

	ffsize n = in->len;
	int r = ffgzread_process(&z->ungz, in, out);
	z->in_total += n - in->len;

	switch ((enum FFGZREAD_R)r) {
	case FFGZREAD_INFO: {
		ffgzread_info *info = ffgzread_getinfo(&z->ungz);
		fcom_dbglog("info: name:%S  mtime:%u  osize:%U  crc32:%xu"
			, &info->name, (int)info->mtime, info->uncompressed_size, info->uncompressed_crc);
		return 'info';
	}

	case FFGZREAD_DATA:
		fcom_dbglog("gzip read: %L -> %L", n - in->len, out->len);
		z->out_total += out->len;
		return 'data';

	case FFGZREAD_DONE:
		fcom_verblog("%s: %U => %U (%u%%)"
			, z->oname, z->in_total, z->out_total, (uint)FFINT_DIVSAFE(z->out_total * 100, z->in_total));
		ffgzread_close(&z->ungz);
		z->ungz_opened = 0;
		return 'done';

	case FFGZREAD_MORE:
		fcom_dbglog("gzip read: %L -> 0", n - in->len);
		return 'more';

	case FFGZREAD_SEEK:
		z->in_off = ffgzread_offset(&z->ungz);
		return 'more';

	case FFGZREAD_WARNING:
		fcom_warnlog("ffgzread_process: %s  offset:0x%xU", ffgzread_error(&z->ungz), z->in_off);
		return 0;
	case FFGZREAD_ERROR:
		fcom_errlog("ffgzread_process: %s  offset:0x%xU", ffgzread_error(&z->ungz), z->in_off);
		return 'erro';
	}
	return 'erro';
}

static void ungz_run(fcom_op *op)
{
	struct ungz *z = op;
	int r, rc = 1;
	enum { I_IN, I_IN_OPEN, I_OUT_OPEN, I_READ, I_DECOMP, I_WRITE, };

	while (!FFINT_READONCE(z->stop)) {
		switch (z->st) {

		case I_IN:
			if (0 > (r = core->com->input_next(z->cmd, &z->iname, &z->basename, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					core->file->mtime_set(z->out, z->mtime);
					rc = 0;
				}
				goto end;
			}

			z->st = I_IN_OPEN;
			// fallthrough

		case I_IN_OPEN: {
			uint flags = fcom_file_cominfo_flags_i(z->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(z->in, z->iname.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;

			z->st = I_READ;
		}
			// fallthrough

		case I_READ:
			r = core->file->read(z->in, &z->zdata, z->in_off);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(z->cmd);
				return;
			}
			if (r == FCOM_FILE_EOF) {
				r = core->file->flush(z->out, 0);
				if (r == FCOM_FILE_ASYNC) {
					core->com->async(z->cmd);
					return;
				}

				if (!z->next_chunk_begin) {
					fcom_warnlog("file incomplete");
					goto end;
				}
				z->st = I_IN;
				continue;
			}
			z->in_off += z->zdata.len;
			z->next_chunk_begin = 0;

			z->st = I_DECOMP;
			// fallthrough

		case I_DECOMP:
			r = gzread(z, &z->zdata, &z->data);
			switch (r) {
			case 0:
				continue;

			case 'info': {
				ffgzread_info *info = ffgzread_getinfo(&z->ungz);
				z->mtime.sec = info->mtime + FFTIME_1970_SECONDS;
				if (!z->out_opened) {
					z->out_opened = 1;
					if (!z->cmd->stdout)
						z->oname = out_name(z, info->name, z->iname, z->basename);
					z->st = I_OUT_OPEN;
				}
				continue;
			}

			case 'done':
				z->next_chunk_begin = 1;
				z->del_on_close = 0;
				continue;

			case 'more':
				z->st = I_READ;
				continue;

			case 'erro':
				goto end;
			}

			z->st = I_WRITE;
			continue;

		case I_OUT_OPEN: {
			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(z->cmd);
			r = core->file->open(z->out, z->oname, flags);
			if (r == FCOM_FILE_ERR) goto end;
			z->del_on_close = !z->cmd->stdout && !z->cmd->test;

			z->st = I_DECOMP;
			continue;
		}

		case I_WRITE:
			r = core->file->write(z->out, z->data, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(z->cmd);
				return;
			}

			z->st = I_DECOMP;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = z->cmd;
	ungz_close(z);
	core->com->complete(cmd, rc);
	}
}

static void ungz_signal(fcom_op *op, uint signal)
{
	struct ungz *z = op;
	FFINT_WRITEONCE(z->stop, 1);
}

const fcom_operation fcom_op_ungz = {
	ungz_create, ungz_close,
	ungz_run, ungz_signal,
	ungz_help,
};
