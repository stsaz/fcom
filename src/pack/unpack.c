/** fcom: Unpack files from all supported archive types
2023, Simon Zolin */

static const char* unpack_help()
{
	return "\
Unpack files from all supported archive types.\n\
Usage:\n\
  fcom unpack INPUT... [-C OUTPUT_DIR]\n\
    OPTIONS:\n\
        --autodir   Add to OUTPUT_DIR a directory with name = input archive name.\n\
                     Same as manual 'unpack arc.xxx -C odir/arc'.\n\
";
}

#include <fcom.h>
#include <ffsys/path.h>
#include <ffsys/pipe.h>

const fcom_core *core;

struct unpack {
	uint state;
	fcom_cominfo *cmd;
	ffstr iname, base;
	uint stop;
	int result;
	fffd pr, pw;

	// conf:
	byte autodir;
};

#define O(member)  FF_OFF(struct unpack, member)

static int unpack_args_parse(struct unpack *u, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{ 0, "autodir",	FFCMDARG_TSWITCH,	O(autodir) },
		{}
	};
	int r = core->com->args_parse(cmd, args, u);
	if (r != 0)
		return r;

	return 0;
}

#undef O

static void unpack_close(fcom_op *op)
{
	struct unpack *u = op;
	ffpipe_close(u->pr);
	ffpipe_close(u->pw);
	ffmem_free(u);
}

static fcom_op* unpack_create(fcom_cominfo *cmd)
{
	struct unpack *u = ffmem_new(struct unpack);
	u->cmd = cmd;
	u->pr = u->pw = FFPIPE_NULL;

	if (0 != unpack_args_parse(u, cmd))
		goto end;

	return u;

end:
	unpack_close(u);
	return NULL;
}

/** Find operation name by file extension */
static const char* op_find_ext(ffstr ext, uint level)
{
	static const char exts[][4] = {
		"7z",
		"gz",
		"iso",
		"tar",
		"xz",
		"zip",
		"zipx",
		"zst",
	};
	static const char ops[][6] = {
		"un7z",
		"ungz",
		"uniso",
		"untar",
		"unxz",
		"unzip",
		"unzip",
		"unzst",
	};
	int r;
	if (0 > (r = ffcharr_findsorted(exts, FF_COUNT(exts), sizeof(exts[0]), ext.ptr, ext.len))) {
		if (level == 0)
			fcom_errlog("unknown archive file extension .%S", &ext);
		return NULL;
	}
	return ops[r];
}

/** Get names of operations we need to perform */
static const char* unpack_detect(struct unpack *u, const char **oper2, ffstr iname)
{
	ffstr name, ext, ext2 = {};
	ffpath_splitpath_str(iname, NULL, &name);
	ffpath_splitname_str(name, &name, &ext);
	ffpath_splitname_str(name, NULL, &ext2);

	if (ffstr_eqz(&ext, "tgz")) {
		ffstr_setz(&ext, "gz");
		ffstr_setz(&ext2, "tar");
	}

	if (ffstr_eqz(&ext, "txz")) {
		ffstr_setz(&ext, "xz");
		ffstr_setz(&ext2, "tar");
	}

	const char *opname;
	if (NULL == (opname = op_find_ext(ext, 0)))
		return NULL;

	// ungz file.tar.gz | untar
	if (ext2.len)
		*oper2 = op_find_ext(ext2, 1);

	return opname;
}

static void unpack_run(fcom_op *op);

static void unpack_op_complete(void *param, int result)
{
	struct unpack *u = param;
	uint level = (ffsize)u & 1;
	u = (void*)((ffsize)u & ~1);

	if (level == 1) {
		// gz process is complete: make tar reader return 0
		ffpipe_close(u->pw);  u->pw = FFPIPE_NULL;
		return;
	}

	u->result = result;
	unpack_run(u);
}

/** Exec a child operation */
static int unpack_child(struct unpack *u, const char *opname, uint level)
{
	fcom_cominfo *c = core->com->create();
	c->operation = ffsz_dup(opname);

	ffstr *p = ffvec_zpushT(&c->input, ffstr);
	if (level == 0 || level == 1)
		ffstr_dupstr(p, &u->iname);
	else
		ffstr_dupz(p, "");

	if (level == 1) {
		c->stdout = 1;
		c->fd_stdout = u->pw;

		c->on_complete = unpack_op_complete;
		c->opaque = (void*)((ffsize)u | 1);

	} else {
		if (u->cmd->output.len != 0)
			ffstr_dupstr(&c->output, &u->cmd->output);
		if (u->cmd->chdir.len != 0)
			ffstr_dupstr(&c->chdir, &u->cmd->chdir);

		if (u->autodir) {
			ffvec a = {};
			*ffvec_pushT(&a, char*) = ffsz_dup("--autodir");
			ffvec_zpushT(&a, char*);
			c->argv = a.ptr;
			c->argc = a.len - 1;
		}

		c->on_complete = unpack_op_complete;
		c->opaque = u;

		if (level == 2) {
			c->stdin = 1;
			c->fd_stdin = u->pr;
		}
	}

	c->test = u->cmd->test;
	c->buffer_size = u->cmd->buffer_size;
	c->directio = u->cmd->directio;
	c->overwrite = u->cmd->overwrite;

	if (0 != core->com->run(c))
		return -1;
	return 0;
}

static void unpack_run(fcom_op *op)
{
	struct unpack *u = op;
	int r, rc = 1;
	enum { I_IN, I_COMPLETE };

	for (;;) {
		switch (u->state) {
		case I_IN:
			if (0 > (r = core->com->input_next(u->cmd, &u->iname, &u->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					rc = 0;
				}
				goto end;
			}

			const char *opname, *opname2 = NULL;
			opname = unpack_detect(u, &opname2, u->iname);
			if (opname == NULL)
				continue;

			if (opname2 != NULL) {
				if (0 != ffpipe_create(&u->pr, &u->pw)) {
					fcom_syserrlog("ffpipe_create");
					goto end;
				}

				if (0 != ffpipe_nonblock(u->pr, 1)
					|| 0 != ffpipe_nonblock(u->pw, 1)) {
					fcom_syserrlog("fffile_nonblock");
					goto end;
				}

				if (0 != unpack_child(u, opname, 1))
					goto end;
				if (0 != unpack_child(u, opname2, 2))
					goto end;

			} else {
				if (0 != unpack_child(u, opname, 0))
					goto end;
			}

			u->state = I_COMPLETE;
			return;

		case I_COMPLETE:
			if (u->result != 0) {
				rc = u->result;
				goto end;
			}
			if (u->pw != FFPIPE_NULL) {
				core->com->async(u->cmd);
				return;
			}
			ffpipe_close(u->pr);  u->pr = FFPIPE_NULL;
			u->state = I_IN;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = u->cmd;
	unpack_close(u);
	core->com->complete(cmd, rc);
	}
}

static void unpack_signal(fcom_op *op, uint signal)
{
	struct unpack *u = op;
	FFINT_WRITEONCE(u->stop, 1);
}

static const fcom_operation fcom_op_unpack = {
	unpack_create, unpack_close,
	unpack_run, unpack_signal,
	unpack_help,
};


static void unpack_init(const fcom_core *_core) { core = _core; }
static void unpack_destroy() {}
static const fcom_operation* unpack_provide_op(const char *name)
{
	if (ffsz_eq(name, "unpack"))
		return &fcom_op_unpack;
	return NULL;
}
FF_EXP const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	unpack_init, unpack_destroy, unpack_provide_op,
};
