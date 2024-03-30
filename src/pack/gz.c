/** fcom: pack file into .gz
2023, Simon Zolin */

static const char* gz_help()
{
	return "\
Compress file into .gz.\n\
Usage:\n\
  `fcom gz` INPUT [OPTIONS] [-o OUTPUT.gz]\n\
\n\
OPTIONS:\n\
    `-l`, `--level` INT     Compression level: 1..9; default:6\n\
";
}

#include <fcom.h>
#include <ffsys/globals.h>
#include <ffsys/path.h>
#include <ffpack/gzwrite.h>

const fcom_core *core;

struct gz {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	ffstr iname, basename;
	char *oname;
	fcom_file_obj *in, *out;
	ffgzwrite gz;
	ffstr data, zdata;
	ffvec buf;
	uint stop;
	uint del_on_close :1;

	uint64 in_total, out_total;

	uint level;
};

#define O(member)  (void*)FF_OFF(struct gz, member)

static int args_parse(struct gz *z, fcom_cominfo *cmd)
{
	z->level = 6;

	static const struct ffarg args[] = {
		{ "--level",	'u',	O(level) },
		{ "-l",			'u',	O(level) },
		{}
	};
	int r = core->com->args_parse(cmd, args, z, FCOM_COM_AP_INOUT);
	if (r != 0)
		return r;

	return 0;
}

#undef O

static void gz_close(fcom_op *op)
{
	struct gz *z = op;
	ffgzwrite_destroy(&z->gz);
	core->file->destroy(z->in);
	if (z->del_on_close)
		core->file->del(z->oname, 0);
	core->file->destroy(z->out);
	ffmem_free(z->oname);
	ffvec_free(&z->buf);
	ffmem_free(z);
}

static fcom_op* gz_create(fcom_cominfo *cmd)
{
	struct gz *z = ffmem_new(struct gz);
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
	gz_close(z);
	return NULL;
}

/** Prepare output file name
idir/iname -> idir/iname.gz
idir/iname -> odir/iname.gz (--chdir) */
static char* out_name(struct gz *z, ffstr in, ffstr base)
{
	char *ofn = NULL;
	if (z->cmd->output.len != 0) {
		ofn = ffsz_dup(z->cmd->output.ptr);

	} else if (z->cmd->chdir.len != 0) {
		ffstr name;
		if (base.len == 0)
			base = in;
		ffpath_splitpath(base.ptr, base.len, NULL, &name);
		if (name.len != 0)
			ffstr_shift(&in, name.ptr - base.ptr);

		ofn = ffsz_allocfmt("%S%c%S.gz"
			, &z->cmd->chdir, FFPATH_SLASH, &in);

	} else {
		ofn = ffsz_allocfmt("%S.gz", &in);
	}
	fcom_dbglog("output file name: %s", ofn);
	return ofn;
}

static int gzcomp(struct gz *z, ffstr *in, ffstr *out)
{
	ffsize n = in->len;
	int r = ffgzwrite_process(&z->gz, in, out);
	z->in_total += n - in->len;

	switch ((enum FFGZWRITE_R)r) {
	case FFGZWRITE_DATA:
		z->out_total += out->len;
		return 'data';

	case FFGZWRITE_DONE:
		fcom_verblog("%U => %U (%u%%)"
			, z->in_total, z->out_total, (uint)FFINT_DIVSAFE(z->out_total * 100, z->in_total));
		return 'done';

	case FFGZWRITE_MORE:
		return 'more';

	case FFGZWRITE_ERROR:
		fcom_errlog("ffgzwrite_process: %s", ffgzwrite_error(&z->gz));
		return 'erro';
	}
	return 'erro';
}

static void gz_run(fcom_op *op)
{
	struct gz *z = op;
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

			fffileinfo fi = {};
			r = core->file->info(z->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;

			ffgzwrite_conf gzconf = {};
			gzconf.deflate_level = z->level;
			gzconf.deflate_mem = 256;
			ffpath_splitpath_str(z->iname, NULL, &gzconf.name);
			gzconf.mtime = fffileinfo_mtime(&fi).sec;
			if (0 != ffgzwrite_init(&z->gz, &gzconf)) {
				fcom_errlog("ffgzwrite_init: %s", ffgzwrite_error(&z->gz));
				goto end;
			}

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
				ffgzwrite_finish(&z->gz);

			z->st = I_COMP;
			// fallthrough

		case I_COMP:
			r = gzcomp(z, &z->data, &z->zdata);
			switch (r) {
			case 'erro':
				goto end;
			case 'more':
				z->st = I_READ;
				continue;
			case 'done':
				z->del_on_close = 0;
				z->st = I_IN;
				continue;
			}

			z->st = I_WRITE;
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
	gz_close(z);
	core->com->complete(cmd, rc);
	}
}

static void gz_signal(fcom_op *op, uint signal)
{
	struct gz *z = op;
	FFINT_WRITEONCE(z->stop, 1);
}

static const fcom_operation fcom_op_gz = {
	gz_create, gz_close,
	gz_run, gz_signal,
	gz_help,
};


static void gz_init(const fcom_core *_core) { core = _core; }
static void gz_destroy() {}
extern const fcom_operation fcom_op_ungz;
static const fcom_operation* gz_provide_op(const char *name)
{
	if (ffsz_eq(name, "gz"))
		return &fcom_op_gz;
	else if (ffsz_eq(name, "ungz"))
		return &fcom_op_ungz;
	return NULL;
}
FF_EXPORT const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	gz_init, gz_destroy, gz_provide_op,
};
