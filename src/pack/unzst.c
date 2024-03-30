/** fcom: unpack file from .zst
2023, Simon Zolin */

static const char* unzst_help()
{
	return "\
Decompress file from .zst.\n\
Usage:\n\
  `fcom unzst` INPUT [OPTIONS] [-o OUTPUT]\n\
";
}

#include <fcom.h>
#include <ffsys/path.h>
#include <zstd/zstd-ff.h>

extern const fcom_core *core;

struct unzst {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	ffstr iname, basename;
	char *oname;
	fcom_file_obj *in, *out;
	zstd_decoder *zst;
	ffstr data, zdata;
	ffvec buf;
	uint stop;
	uint out_opened :1;
	uint del_on_close :1;

	uint64 in_total, out_total;
};

static int args_parse(struct unzst *z, fcom_cominfo *cmd)
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

static void unzst_close(fcom_op *op)
{
	struct unzst *z = op;
	zstd_decode_free(z->zst);
	core->file->destroy(z->in);
	if (z->del_on_close)
		core->file->del(z->oname, 0);
	core->file->destroy(z->out);
	ffmem_free(z->oname);
	ffvec_free(&z->buf);
	ffmem_free(z);
}

static fcom_op* unzst_create(fcom_cominfo *cmd)
{
	struct unzst *z = ffmem_new(struct unzst);
	z->cmd = cmd;

	if (0 != args_parse(z, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fcom_cmd_file_conf(&fc, cmd);
	z->in = core->file->create(&fc);
	z->out = core->file->create(&fc);

	ffsize cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&z->buf, cap, 1);
	return z;

end:
	unzst_close(z);
	return NULL;
}

/** Prepare output file name
idir/iname.zst -> odir/iname */
static char* out_name(struct unzst *z, ffstr in, ffstr base)
{
	char *ofn = NULL;
	if (z->cmd->output.len != 0) {
		ofn = ffsz_dup(z->cmd->output.ptr);

	} else {
		ffpath_splitpath_str(in, NULL, &in);
		ffstr name, ext;
		ffpath_splitname_str(in, &name, &ext);
		if (ffstr_eqz(&ext, "zst"))
			in = name;
		ofn = ffsz_allocfmt("%S%c%S"
			, &z->cmd->chdir, FFPATH_SLASH, &in);
	}

	fcom_dbglog("output file name: %s", ofn);
	return ofn;
}

static int zst_read(struct unzst *z, ffstr *input, ffstr *output)
{
	zstd_buf in, out;
	zstd_buf_set(&in, input->ptr, input->len);
	zstd_buf_set(&out, z->buf.ptr, z->buf.cap);
	int r = zstd_decode(z->zst, &in, &out);
	ffstr_shift(input, in.pos);
	ffstr_set(output, z->buf.ptr, out.pos);
	z->in_total += in.pos;
	z->out_total += out.pos;

	fcom_dbglog("zstd_decode: %L (%U) -> %L (%U)"
		, in.pos, z->in_total, out.pos, z->out_total);

	if (r < 0) {
		fcom_errlog("zstd_decode: %s", zstd_error(r));
		return 'erro';

	} else if (out.pos == 0) {
		return 'more';
	}

	return 'data';
}

static void unzst_run(fcom_op *op)
{
	struct unzst *z = op;
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

			z->st = I_READ;
		}
			// fallthrough

		case I_READ:
			r = core->file->read(z->in, &z->zdata, -1);
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

				fcom_verblog("%s: %U => %U (%u%%)"
					, z->oname, z->in_total, z->out_total, (uint)FFINT_DIVSAFE(z->out_total * 100, z->in_total));
				zstd_decode_free(z->zst);  z->zst = NULL;
				z->in_total = z->out_total = 0;
				core->file->close(z->out);
				z->out_opened = 0;
				z->del_on_close = 0;

				z->st = I_IN;
				continue;
			}

			if (!z->out_opened) {
				z->out_opened = 1;

				zstd_dec_conf zc = {};
				zstd_decode_init(&z->zst, &zc);

				if (!z->cmd->stdout)
					z->oname = out_name(z, z->iname, z->basename);
				z->st = I_OUT_OPEN;
				continue;
			}

			z->st = I_DECOMP;
			// fallthrough

		case I_DECOMP:
			r = zst_read(z, &z->zdata, &z->data);
			switch (r) {
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

			fffileinfo fi = {};
			r = core->file->info(z->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;

			core->file->mtime_set(z->out, fffileinfo_mtime1(&fi));

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
	unzst_close(z);
	core->com->complete(cmd, rc);
	}
}

static void unzst_signal(fcom_op *op, uint signal)
{
	struct unzst *z = op;
	FFINT_WRITEONCE(z->stop, 1);
}

const fcom_operation fcom_op_unzst = {
	unzst_create, unzst_close,
	unzst_run, unzst_signal,
	unzst_help,
};
