/** fcom: unpack file from .xz
2023, Simon Zolin */

static const char* unxz_help()
{
	return "\
Decompress file from .xz.\n\
Usage:\n\
  `fcom unxz` INPUT [OPTIONS] [-o OUTPUT]\n\
";
}

#include <fcom.h>
#include <ffsys/path.h>
#include <ffpack/xzread.h>

static const fcom_core *core;

struct unxz {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	ffstr iname, basename;
	char *oname;
	fcom_file_obj *in, *out;
	uint64 in_off;
	ffxzread unxz;
	uint64 isize;
	ffstr data, zdata;
	uint stop;
	uint unxz_opened :1;
	uint next_chunk_begin :1;
	uint out_opened :1;
	uint del_on_close :1;

	uint64 in_total, out_total;
};

static int args_parse(struct unxz *z, fcom_cominfo *cmd)
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

static void unxz_close(fcom_op *op)
{
	struct unxz *z = op;
	ffxzread_close(&z->unxz);
	core->file->destroy(z->in);
	if (z->del_on_close)
		core->file->del(z->oname, 0);
	core->file->destroy(z->out);
	ffmem_free(z->oname);
	ffmem_free(z);
}

static fcom_op* unxz_create(fcom_cominfo *cmd)
{
	struct unxz *z = ffmem_new(struct unxz);
	z->cmd = cmd;

	if (0 != args_parse(z, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fcom_cmd_file_conf(&fc, cmd);
	z->in = core->file->create(&fc);
	z->out = core->file->create(&fc);
	return z;

end:
	unxz_close(z);
	return NULL;
}

/** Prepare output file name
idir/iname.xz -> odir/iname */
static char* out_name(struct unxz *z, ffstr in, ffstr base)
{
	char *ofn = NULL;
	if (z->cmd->output.len != 0) {
		ofn = ffsz_dup(z->cmd->output.ptr);

	} else {
		ffpath_splitpath_str(in, NULL, &in);
		ffstr name, ext;
		ffpath_splitname_str(in, &name, &ext);
		if (ffstr_eqz(&ext, "xz"))
			in = name;
		ofn = ffsz_allocfmt("%S%c%S"
			, &z->cmd->chdir, FFPATH_SLASH, &in);
	}

	fcom_dbglog("output file name: %s", ofn);
	return ofn;
}

static int xzread(struct unxz *z, ffstr *in, ffstr *out)
{
	if (!z->unxz_opened) {
		z->unxz_opened = 1;
		if (0 != ffxzread_open(&z->unxz, z->isize)) {
			fcom_errlog("ffxzread_open");
			return 'erro';
		}
	}

	ffsize n = in->len;
	int r = ffxzread_process(&z->unxz, in, out);
	z->in_total += n - in->len;

	switch ((enum FFXZREAD_R)r) {
	case FFXZREAD_INFO: {
		ffxzread_info *info = ffxzread_getinfo(&z->unxz);
		fcom_dbglog("info: osize:%U", info->uncompressed_size);
		return 'info';
	}

	case FFXZREAD_DATA:
		z->out_total += out->len;
		return 'data';

	case FFXZREAD_DONE:
		fcom_verblog("%s: %U => %U (%u%%)"
			, z->oname, z->in_total, z->out_total, (uint)FFINT_DIVSAFE(z->out_total * 100, z->in_total));
		ffxzread_close(&z->unxz);
		z->unxz_opened = 0;
		return 'done';

	case FFXZREAD_MORE:
		return 'more';

	case FFXZREAD_SEEK:
		z->in_off = ffxzread_offset(&z->unxz);
		return 'more';

	case FFXZREAD_ERROR:
		fcom_errlog("ffxzread_process: %s  offset:0x%xU", ffxzread_error(&z->unxz), z->in_off);
		return 'erro';
	}
	return 'erro';
}

static void unxz_run(fcom_op *op)
{
	struct unxz *z = op;
	int r, rc = 1;
	enum { I_IN, I_IN_OPEN, I_OUT_OPEN, I_READ, I_DECOMP, I_WRITE, };

	while (!FFINT_READONCE(z->stop)) {
		switch (z->st) {

		case I_IN:
			if (0 > (r = core->com->input_next(z->cmd, &z->iname, &z->basename, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
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

			fffileinfo fi = {};
			r = core->file->info(z->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;
			z->isize = fffileinfo_size(&fi);

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
			r = xzread(z, &z->zdata, &z->data);
			switch (r) {
			case 0:
			case 'info':
				continue;
			case 'done':
				z->next_chunk_begin = 1;
				z->del_on_close = 0;
				// fallthrough
			case 'more':
				z->st = I_READ;
				continue;
			case 'erro':
				goto end;
			}

			z->st = I_WRITE;
			if (!z->out_opened) {
				z->out_opened = 1;
				if (!z->cmd->stdout)
					z->oname = out_name(z, z->iname, z->basename);
				z->st = I_OUT_OPEN;
			}
			continue;

		case I_OUT_OPEN: {
			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(z->cmd);
			r = core->file->open(z->out, z->oname, flags);
			if (r == FCOM_FILE_ERR) goto end;
			z->del_on_close = !z->cmd->stdout && !z->cmd->test;

			fffileinfo fi = {};
			r = core->file->info(z->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;

			core->file->mtime_set(z->out, fffileinfo_mtime1(&fi));

			z->st = I_WRITE;
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
	unxz_close(z);
	core->com->complete(cmd, rc);
	}
}

static void unxz_signal(fcom_op *op, uint signal)
{
	struct unxz *z = op;
	FFINT_WRITEONCE(z->stop, 1);
}

static const fcom_operation fcom_op_unxz = {
	unxz_create, unxz_close,
	unxz_run, unxz_signal,
	unxz_help,
};


static void xz_init(const fcom_core *_core) { core = _core; }
static void xz_destroy() {}
static const fcom_operation* xz_provide_op(const char *name)
{
	if (ffsz_eq(name, "unxz"))
		return &fcom_op_unxz;
	return NULL;
}
FF_EXPORT const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	xz_init, xz_destroy, xz_provide_op,
};
