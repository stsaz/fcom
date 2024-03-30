/** fcom: pack file into .zst
2023, Simon Zolin */

static const char* zst_help()
{
	return "\
Compress file into .zst.\n\
Usage:\n\
  `fcom zst` INPUT [OPTIONS] [-o OUTPUT.zst]\n\
\n\
OPTIONS:\n\
    `-l`, `--level` INT     Compression level: -7..22; default:3\n\
    `-w`, `--workers` INT   N of threads for compression; default:1\n\
";
}

#include <fcom.h>
#include <ffsys/globals.h>
#include <ffsys/path.h>
#include <zstd/zstd-ff.h>

const fcom_core *core;

struct zst {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	ffstr iname, basename;
	char *oname;
	fcom_file_obj *in, *out;
	zstd_encoder *zst;
	uint zst_flags;
	ffstr data, zdata;
	ffvec buf;
	uint stop;
	uint del_on_close :1;

	uint64 in_total, out_total;

	uint level;
	uint workers;
};

#define O(member)  (void*)FF_OFF(struct zst, member)

static int args_parse(struct zst *z, fcom_cominfo *cmd)
{
	z->level = 3;
	z->workers = 1;

	static const struct ffarg args[] = {
		{ "--level",	'u',	O(level) },
		{ "--workers",	'u',	O(workers) },
		{ "-l",			'u',	O(level) },
		{ "-w",			'u',	O(workers) },
		{}
	};
	int r = core->com->args_parse(cmd, args, z, FCOM_COM_AP_INOUT);
	if (r != 0)
		return r;

	if (cmd->chdir.len == 0)
		ffstr_dupz(&cmd->chdir, ".");

	return 0;
}

#undef O

static void zst_close(fcom_op *op)
{
	struct zst *z = op;
	zstd_encode_free(z->zst);
	core->file->destroy(z->in);
	if (z->del_on_close)
		core->file->del(z->oname, 0);
	core->file->destroy(z->out);
	ffmem_free(z->oname);
	ffvec_free(&z->buf);
	ffmem_free(z);
}

static fcom_op* zst_create(fcom_cominfo *cmd)
{
	struct zst *z = ffmem_new(struct zst);
	z->cmd = cmd;

	if (0 != args_parse(z, cmd))
		goto end;

	zstd_enc_conf zc = {};
	zc.level = z->level;
	zc.workers = z->workers;
	zstd_encode_init(&z->zst, &zc);

	struct fcom_file_conf fc = {};
	fcom_cmd_file_conf(&fc, cmd);
	z->in = core->file->create(&fc);
	z->out = core->file->create(&fc);

	ffsize cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&z->buf, cap, 1);
	return z;

end:
	zst_close(z);
	return NULL;
}

/** Prepare output file name
idir/iname -> odir/iname.zst */
static char* out_name(struct zst *z, ffstr in, ffstr base)
{
	char *ofn = NULL;
	if (z->cmd->output.len != 0) {
		ofn = ffsz_dup(z->cmd->output.ptr);

	} else {
		ffpath_splitpath_str(in, NULL, &in);
		ofn = ffsz_allocfmt("%S%c%S.zst"
			, &z->cmd->chdir, FFPATH_SLASH, &in);
	}

	fcom_dbglog("output file name: %s", ofn);
	return ofn;
}

static void zst_run(fcom_op *op)
{
	struct zst *z = op;
	int r, rc = 1;
	enum { I_IN, I_IN_OPEN, I_OUT_OPEN, I_READ, I_COMP, I_WRITE, };

	while (!FFINT_READONCE(z->stop)) {
		switch (z->st) {

		case I_IN:
			if (0 > (r = core->com->input_next(z->cmd, &z->iname, &z->basename, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE)
					rc = 0;
				goto end;
			}
			z->st = I_IN_OPEN;
			// fallthrough

		case I_IN_OPEN: {
			uint flags = fcom_file_cominfo_flags_i(z->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(z->in, z->iname.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;

			if (!z->cmd->stdout) {
				if (NULL == (z->oname = out_name(z, z->iname, z->basename)))
					goto end;
			}

			z->st = I_OUT_OPEN;
		}
			// fallthrough

		case I_OUT_OPEN: {
			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(z->cmd);
			r = core->file->open(z->out, z->oname, flags);
			if (r == FCOM_FILE_ERR) goto end;
			z->del_on_close = !z->cmd->stdout && !z->cmd->test;

			z->st = I_READ;
		}
			// fallthrough

		case I_READ:
			r = core->file->read(z->in, &z->data, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(z->cmd);
				return;
			}
			if (r == FCOM_FILE_EOF)
				z->zst_flags = ZSTD_FFINISH;

			z->st = I_COMP;
			// fallthrough

		case I_COMP: {
			zstd_buf in, out;
			zstd_buf_set(&in, z->data.ptr, z->data.len);
			zstd_buf_set(&out, z->buf.ptr, z->buf.cap);
			r = zstd_encode(z->zst, &in, &out, z->zst_flags);
			ffstr_shift(&z->data, in.pos);
			ffstr_set(&z->zdata, z->buf.ptr, out.pos);
			z->in_total += in.pos;
			z->out_total += out.pos;

			if (r < 0) {
				fcom_errlog("zstd_encode: %s", zstd_error(r));
				goto end;
			}

			fcom_dbglog("zstd_encode: %L (%U) -> %L (%U)"
				, in.pos, z->in_total, out.pos, z->out_total);

			if (out.pos == 0) {
				if (z->zst_flags == ZSTD_FFINISH) {
					fcom_verblog("%U => %U (%u%%)"
						, z->in_total, z->out_total, (uint)FFINT_DIVSAFE(z->out_total * 100, z->in_total));

					z->del_on_close = 0;
					z->st = I_IN;
					continue;
				}
				z->st = I_READ;
				continue;
			}

			z->st = I_WRITE;
		}
			// fallthrough

		case I_WRITE:
			r = core->file->write(z->out, z->zdata, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(z->cmd);
				return;
			}

			z->st = I_COMP;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = z->cmd;
	zst_close(z);
	core->com->complete(cmd, rc);
	}
}

static void zst_signal(fcom_op *op, uint signal)
{
	struct zst *z = op;
	FFINT_WRITEONCE(z->stop, 1);
}

static const fcom_operation fcom_op_zst = {
	zst_create, zst_close,
	zst_run, zst_signal,
	zst_help,
};


static void zst_init(const fcom_core *_core) { core = _core; }
static void zst_destroy() {}
extern const fcom_operation fcom_op_unzst;
static const fcom_operation* zst_provide_op(const char *name)
{
	if (ffsz_eq(name, "zst"))
		return &fcom_op_zst;
	if (ffsz_eq(name, "unzst"))
		return &fcom_op_unzst;
	return NULL;
}
FF_EXPORT const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	zst_init, zst_destroy, zst_provide_op,
};
