/** fcom: Unpack files from all supported archive types
2023, Simon Zolin */

static const char* unpack_help()
{
	return "\
Unpack files from all supported archive types.\n\
Usage:\n\
  `fcom unpack` INPUT... [-C OUTPUT_DIR]\n\
\n\
OPTIONS:\n\
    `-l`, `--list`      Just show the file list\n\
        `--plain`     Plain file names\n\
        `--autodir`   Add to OUTPUT_DIR a directory with name = input archive name.\n\
                     Same as manual 'unpack arc.xxx -C odir/arc'.\n\
";
}

#include <fcom.h>
#include <util/util.h>
#include <ffsys/path.h>
#include <ffsys/pipe.h>

const fcom_core *core;

struct unpack {
	fcom_cominfo cominfo;

	uint state;
	fcom_cominfo *cmd;
	ffstr iname, base;
	uint stop;
	int result;
	fffd pr, pw;

	// conf:
	byte list, list_plain;
	byte autodir;
};

/** Find operation name by file extension */
static const char* op_find_ext(ffstr ext, uint level)
{
	static const char ext_op[][2][6] = {
		{"7z",	"un7z"},
		{"gz",	"ungz"},
		{"iso",	"uniso"},
		{"tar",	"untar"},
		{"xz",	"unxz"},
		{"zip",	"unzip"},
		{"zipx","unzip"},
		{"zst",	"unzst"},
	};
	int r;
	if (0 > (r = ffcharr_find_sorted_padding(ext_op, FF_COUNT(ext_op), 6, 6, ext.ptr, ext.len)))
		return NULL;
	return ext_op[r][1];
}

/** Get names of operations we need to perform */
static const char* unpack_detect(struct unpack *u, const char **oper2, ffstr iname)
{
	ffstr name, ext = {}, ext2 = {};
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
	if (!(opname = op_find_ext(ext, 0)))
		return NULL;

	// ungz file.tar.gz | untar
	if (ext2.len)
		*oper2 = op_find_ext(ext2, 1);

	return opname;
}

static void unpack_run(fcom_op *op);

static void unpack_op_complete(void *param, int result)
{
	uint level = (ffsize)param & 1;
	struct unpack *u = (void*)((ffsize)param & ~1);

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
		ffstrz_dup_str0(&c->output, u->cmd->output);
		ffstr_dup_str0(&c->chdir, u->cmd->chdir);

		ffvec a = {};

		if (u->autodir)
			*ffvec_pushT(&a, char*) = ffsz_dup("--autodir");

		if (u->list)
			*ffvec_pushT(&a, char*) = ffsz_dup("--list");

		if (u->list_plain)
			*ffvec_pushT(&a, char*) = ffsz_dup("--plain");

		if (a.len) {
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

	if (core->com->run(c))
		return -1;
	return 0;
}

static int unpack_begin(struct unpack *u)
{
	const char *opname, *opname2 = NULL;
	if (!(opname = unpack_detect(u, &opname2, u->iname)))
		return 'next';

	if (opname2) {
		if (ffpipe_create2(&u->pr, &u->pw, FFPIPE_NONBLOCK)) {
			fcom_syserrlog("ffpipe_create");
			return -1;
		}
		if (unpack_child(u, opname, 1))
			return -1;
		if (unpack_child(u, opname2, 2))
			return -1;

	} else {
		if (unpack_child(u, opname, 0))
			return -1;
	}
	return 0;
}

#define O(member)  (void*)FF_OFF(struct unpack, member)

static int unpack_args_parse(struct unpack *u, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--autodir",				'1',	O(autodir) },
		{ "--list",					'1',	O(list) },
		{ "--plain",				'1',	O(list_plain) },
		{ "-l",						'1',	O(list) },
		{}
	};
	int r = core->com->args_parse(cmd, args, u, FCOM_COM_AP_INOUT);
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

static void unpack_run(fcom_op *op)
{
	struct unpack *u = op;
	int r, rc = 1;
	enum { I_IN, I_COMPLETE };

	while (!FFINT_READONCE(u->stop)) {
		switch (u->state) {
		case I_IN:
			if (0 > (r = core->com->input_next(u->cmd, &u->iname, &u->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					rc = 0;
				}
				goto end;
			}

			switch (unpack_begin(u)) {
			case 'next':
				continue;
			case -1:
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

FCOM_MOD_DEFINE(unpack, fcom_op_unpack, core)
