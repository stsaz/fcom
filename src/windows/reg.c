/** fcom: Windows Registry utils
2023, Simon Zolin */

static const char* reg_help()
{
	return "\
Windows Registry utils: search.\n\
Usage:\n\
  fcom reg search [HKEY...] TEXT... [-o OUTPUT]\n\
\n\
HKEY: HKEY_CLASSES_ROOT | HKEY_CURRENT_USER | HKEY_LOCAL_MACHINE | HKEY_USERS\n\
  Default: HKEY_CURRENT_USER and HKEY_LOCAL_MACHINE\n\
";
}

#include <fcom.h>
#include <ffsys/winreg.h>
#include <ffsys/globals.h>
#include <ffbase/list.h>

static const fcom_core *core;

struct reg {
	uint st;
	fcom_cominfo *cmd;
	fcom_file_obj *out;
	uint oper;
	uint stop;
	ffvec buf;

	// search:
	ffvec values; // ffstr[]
	ffwinreg_enum e;
	ffwinreg key;
	fflist blocks;
	fflist keys;
	ffchain_item *lr, *lw;
	uint search_state;
	struct {
		uint subkeys
			, subkeys_all
			, vals
			, vals_all;
	} stat;
};

#include <windows/reg-search.h>

static int args_parse(struct reg *g, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{}
	};
	if (0 != core->com->args_parse(cmd, args, g))
		return -1;

	if (0 != reg_search_args(g))
		return -1;

	return 0;
}

static void reg_close(fcom_op *op)
{
	struct reg *g = op;
	reg_search_close(g);
	core->file->destroy(g->out);
	ffvec_free(&g->buf);
	ffmem_free(g);
}

static fcom_op* reg_create(fcom_cominfo *cmd)
{
	struct reg *g = ffmem_new(struct reg);
	g->cmd = cmd;

	if (0 != args_parse(g, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	g->out = core->file->create(&fc);

	return g;

end:
	reg_close(g);
	return NULL;
}

static void reg_run(fcom_op *op)
{
	struct reg *g = op;
	int r, rc = 1;
	enum { I_OUT_OPEN, I_FIND, };

	while (!FFINT_READONCE(g->stop)) {
		switch (g->st) {

		case I_OUT_OPEN: {
			uint oflags = FCOM_FILE_WRITE;
			oflags |= fcom_file_cominfo_flags_o(g->cmd);
			r = core->file->open(g->out, g->cmd->output.ptr, oflags);
			if (r == FCOM_FILE_ERR) goto end;

			g->st = I_FIND;
			continue;
		}

		case I_FIND: {
			ffstr kv;
			r = reg_search_next(g, &kv);

			switch (r) {
			case 'data':
				r = core->file->write(g->out, kv, -1);
				if (r == FCOM_FILE_ERR) goto end;
				continue;

			case 'erro': goto end;

			case 'done':
				rc = 0;
				goto end;
			}

			continue;
		}
		}
	}

end:
	{
	fcom_cominfo *cmd = g->cmd;
	reg_close(g);
	core->com->complete(cmd, rc);
	}
}

static void reg_signal(fcom_op *op, uint signal)
{
	struct reg *g = op;
	FFINT_WRITEONCE(g->stop, 1);
}

static const fcom_operation fcom_op_reg = {
	reg_create, reg_close,
	reg_run, reg_signal,
	reg_help,
};


static void reg_init(const fcom_core *_core) { core = _core; }
static void reg_destroy() {}
static const fcom_operation* reg_provide_op(const char *name)
{
	if (ffsz_eq(name, "reg"))
		return &fcom_op_reg;
	return NULL;
}
FF_EXP const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	reg_init, reg_destroy, reg_provide_op,
};
