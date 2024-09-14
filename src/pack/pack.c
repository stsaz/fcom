/** fcom: Pack files into any supported archive type
2023, Simon Zolin */

static const char* pack_help()
{
	return "\
Pack files into any supported archive type.\n\
Usage:\n\
  fcom pack INPUT... [-C OUTPUT_DIR] -o OUTPUT\n\
";
}

#include <fcom.h>
#include <util/util.h>
#include <ffsys/path.h>
#include <ffsys/pipe.h>

const fcom_core *core;

struct pack {
	fcom_cominfo cominfo;
	uint state;
	fcom_cominfo *cmd;
	ffstr iname, base;
	uint stop;
	int result;
	fffd pr, pw;
};

/** Find operation name by file extension */
static const char* op_find_ext(ffstr ext)
{
	static const char ext_op[][2][4] = {
		{"gz",	"gz"},
		{"iso",	"iso"},
		{"tar",	"tar"},
		{"zip",	"zip"},
		{"zipx","zip"},
		{"zst",	"zst"},
	};
	int r;
	if (0 > (r = ffcharr_find_sorted_padding(ext_op, FF_COUNT(ext_op), 4, 4, ext.ptr, ext.len)))
		return NULL;
	return ext_op[r][1];
}

/** Get names of the operations we need to perform */
static const char* pack_detect(struct pack *p, const char **oper2, ffstr oname)
{
	ffstr name, ext, ext2;
	ffpath_splitpath_str(oname, NULL, &name);
	ffpath_splitname_str(name, &name, &ext2);
	if (ffpath_splitname_str(name, NULL, &ext) < 0) {
		ext = ext2;
		ext2.len = 0;
	}

	if (ffstr_eqz(&ext, "tgz")) {
		ffstr_setz(&ext, "tar");
		ffstr_setz(&ext2, "gz");
	}

	const char *opname;
	if (!(opname = op_find_ext(ext))) {
		fcom_errlog("unknown archive file extension .%S", &ext);
		return NULL;
	}

	// tar | gz > file.tar.gz
	if (ext2.len)
		*oper2 = op_find_ext(ext2);

	return opname;
}

static void pack_run(fcom_op *op);

static void pack_op_complete(void *param, int result)
{
	uint level = (size_t)param & 1;
	struct pack *p = (void*)((size_t)param & ~1);

	if (level == 1) {
		// tar process is complete: make gz input reader return 0
		ffpipe_close(p->pw);  p->pw = FFPIPE_NULL;
		return;
	}

	p->result = result;
	pack_run(p);
}

static void vec_str_dup(ffvec *dst, ffvec *src)
{
	ffstr *it;
	FFSLICE_WALK(src, it) {
		ffstr *s = ffvec_zpushT(dst, ffstr);
		ffstr_dupstr(s, it);
	}
}

/** Exec a child operation */
static int pack_child(struct pack *p, const char *opname, uint level)
{
	fcom_cominfo *c = core->com->create();
	c->operation = ffsz_dup(opname);

	c->overwrite = p->cmd->overwrite;
	c->test = p->cmd->test;

	c->on_complete = pack_op_complete;
	c->opaque = p;

	if (level == 0 || level == 1) {
		vec_str_dup(&c->input, &p->cmd->input);
		vec_str_dup(&c->include, &p->cmd->include);
		vec_str_dup(&c->exclude, &p->cmd->exclude);
	} else {
		ffstr *s = ffvec_zpushT(&c->input, ffstr);
		ffstr_dupz(s, "");
	}

	if (level == 1) {
		c->stdout = 1;
		c->fd_stdout = p->pw;
		c->opaque = (void*)((size_t)p | 1);

	} else {
		ffstrz_dup_str0(&c->output, p->cmd->output);
		ffstr_dup_str0(&c->chdir, p->cmd->chdir);

		if (level == 2) {
			c->stdin = 1;
			c->fd_stdin = p->pr;
		}
	}

	if (core->com->run(c))
		return -1;
	return 0;
}

static int pack_begin(struct pack *p, ffstr oname)
{
	const char *opname, *opname2 = NULL;
	if (!(opname = pack_detect(p, &opname2, oname)))
		return -1;

	if (opname2) {
		if (ffpipe_create2(&p->pr, &p->pw, FFPIPE_NONBLOCK)) {
			fcom_syserrlog("ffpipe_create");
			return -1;
		}
		if (pack_child(p, opname, 1))
			return -1;
		if (pack_child(p, opname2, 2))
			return -1;

	} else {
		if (pack_child(p, opname, 0))
			return -1;
	}
	return 0;
}

#define O(member)  FF_OFF(struct pack, member)

static int pack_args_parse(struct pack *p, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{}
	};
	int r = core->com->args_parse(cmd, args, p, FCOM_COM_AP_INOUT);
	if (r)
		return r;

	if (!p->cmd->output.len) {
		fcom_errlog("Please specify output file name with `-o NAME`");
		return -1;
	}

	return 0;
}

#undef O

static void pack_close(fcom_op *op)
{
	struct pack *p = op;
	ffpipe_close(p->pr);
	ffpipe_close(p->pw);
	ffmem_free(p);
}

static fcom_op* pack_create(fcom_cominfo *cmd)
{
	struct pack *p = ffmem_new(struct pack);
	p->cmd = cmd;
	p->pr = p->pw = FFPIPE_NULL;

	if (pack_args_parse(p, cmd))
		goto end;

	return p;

end:
	pack_close(p);
	return NULL;
}

static void pack_run(fcom_op *op)
{
	struct pack *p = op;
	int r, rc = 1;
	enum { I_IN, I_COMPLETE };

	while (!FFINT_READONCE(p->stop)) {
		switch (p->state) {
		case I_IN:
			// if (0 > (r = core->com->input_next(p->cmd, &p->iname, &p->base, 0))) {
			// 	if (r == FCOM_COM_RINPUT_NOMORE) {
			// 		rc = 0;
			// 	}
			// 	goto end;
			// }

			r = pack_begin(p, p->cmd->output);
			if (r) goto end;

			p->state = I_COMPLETE;
			return;

		case I_COMPLETE:
			if (p->result) {
				rc = p->result;
				goto end;
			}
			if (p->pw != FFPIPE_NULL) {
				core->com->async(p->cmd);
				return;
			}
			ffpipe_close(p->pr);  p->pr = FFPIPE_NULL;
			rc = 0;
			goto end;

			// p->state = I_IN;
			// continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = p->cmd;
	pack_close(p);
	core->com->complete(cmd, rc);
	}
}

static void pack_signal(fcom_op *op, uint signal)
{
	struct pack *p = op;
	FFINT_WRITEONCE(p->stop, 1);
}

static const fcom_operation fcom_op_pack = {
	pack_create, pack_close,
	pack_run, pack_signal,
	pack_help,
};

FCOM_MOD_DEFINE(pack, fcom_op_pack, core)
