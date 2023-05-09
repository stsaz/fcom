/** fcom: move files
2022, Simon Zolin */

#include <fcom.h>
#include <FFOS/path.h>
#include <FFOS/ffos-extern.h>

static const fcom_core *core;

struct move {
	uint st;
	fcom_cominfo *cmd;
	uint stop;
	fcom_file_obj *in;
	ffvec oname, replace_buf;

	byte unbranch, unbranch_flat;
	byte replace_once;
	ffstr replace_pair, search, replace;
};

static int args_replace(ffcmdarg_scheme *as, struct move *m, ffstr *val)
{
	ffstr_dupstr(&m->replace_pair, val);
	int r = ffstr_splitby(&m->replace_pair, '/', &m->search, &m->replace);
	if (r < 0 || m->search.len == 0) {
		fcom_fatlog("Invalid search-replace pattern. Expecting 'SEARCH/REPLACE'.");
		return 0xbad;
	}
	return 0;
}

#define O(member)  FF_OFF(struct move, member)

static int args_parse(struct move *m, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{ 'u',	"unbranch",	FFCMDARG_TSWITCH, O(unbranch) },
		{ 0,	"unbranch-flat",	FFCMDARG_TSWITCH, O(unbranch_flat) },
		{ 'r',	"replace",	FFCMDARG_TSTR | FFCMDARG_FNOTEMPTY, (ffsize)args_replace },
		{ 0,	"replace-once",	FFCMDARG_TSTR | FFCMDARG_FNOTEMPTY, O(replace_once) },
		{}
	};
	if (0 != core->com->args_parse(cmd, args, m))
		return -1;

	if (m->unbranch && m->unbranch_flat) {
		fcom_fatlog("--unbranch and --unbranch-flat can't be used together");
		return -1;

	} else if (!(m->unbranch || m->unbranch_flat || m->replace_pair.len != 0)) {
		fcom_fatlog("Please use --unbranch and/or --replace");
		return -1;
	}

	return 0;
}

#undef O

static const char* move_help()
{
	return "\
Move and/or rename files.\n\
Usage:\n\
  fcom move INPUT... [OPTIONS]\n\
    OPTIONS:\n\
    -u, --unbranch      Move and rename a file out of its directory structure, e.g.\n\
                          \"fcom --unbranch ./a\"\n\
                         moves/renames \"./a/b/file\" -> \"./a - b - file\"\n\
        --unbranch-flat Move a file out of its directory structure, e.g.\n\
                          \"fcom --unbranch-flat ./a\"\n\
                         moves \"./a/b/file\" -> \"./file\"\n\
    -r, --replace='SEARCH/REPLACE'\n\
                        Replace SEARCH text in file name with REPLACE\n\
        --replace-once  Replace only the first occurrence\n\
";
}

static void move_close(fcom_op *op)
{
	struct move *m = op;
	ffstr_free(&m->replace_pair);
	core->file->destroy(m->in);
	ffvec_free(&m->oname);
	ffvec_free(&m->replace_buf);
	ffmem_free(m);
}

static fcom_op* move_create(fcom_cominfo *cmd)
{
	struct move *m = ffmem_new(struct move);

	if (0 != args_parse(m, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	m->in = core->file->create(&fc);

	m->cmd = cmd;
	return m;

end:
	move_close(m);
	return NULL;
}

/** Prepare new file name.
"parent/base/a/file" -> "parent/base - a - file" */
static void unbranch(struct move *m, ffstr in, ffstr base, ffstr *out)
{
	ffstr parent;
	if (ffpath_splitpath_str(base, &parent, &base) >= 0) {
		ffvec_addfmt(&m->oname, "%S%c", &parent, FFPATH_SLASH);
		ffstr_shift(&in, parent.len + 1);
	}

	int last = 0;
	while (!last) {
		ffstr dir;
		if (0 > ffstr_splitbyany(&in, FFPATH_SLASHES, &dir, &in))
			last = 1;
		ffvec_addstr(&m->oname, &dir);
		if (!last)
			ffvec_addsz(&m->oname, " - ");
	}
	ffstr_setstr(out, &m->oname);
	m->oname.len = 0;
}

/** Prepare new file name.
"parent/base/a/file" -> "parent/file" */
static void unbranch_flat(struct move *m, ffstr in, ffstr base, ffstr *out)
{
	ffstr parent;
	if (ffpath_splitpath_str(base, &parent, NULL) >= 0) {
		ffvec_addfmt(&m->oname, "%S%c", &parent, FFPATH_SLASH);
	}

	ffstr name;
	ffpath_splitpath_str(in, NULL, &name);
	ffvec_addstr(&m->oname, &name);
	ffstr_setstr(out, &m->oname);
	m->oname.len = 0;
}

/** Search and replace in a file name
name: [Input] old file name;  [Output] new file name
Return 0 if replaced */
static int replace(struct move *m, ffstr *name)
{
	ffstr path, nameonly;
	if (ffpath_splitpath_str(*name, &path, &nameonly) >= 0)
		path.len++;

	m->replace_buf.len = 0;
	ffvec_addstr(&m->replace_buf, &path);

	uint flags = 0;
	if (!m->replace_once)
		flags |= FFSTR_REPLACE_ALL;
#ifdef FF_WIN
	flags |= FFSTR_REPLACE_ICASE;
#endif
	if (0 == ffstr_growadd_replace((ffstr*)&m->replace_buf, &m->replace_buf.cap, &nameonly, &m->search, &m->replace, flags)) {
		return -1;
	}
	ffstr_setstr(name, &m->replace_buf);
	return 0;
}

static void move_run(fcom_op *op)
{
	struct move *m = op;
	int r, rc = 1;
	enum { I_IN, };

	while (!FFINT_READONCE(m->stop)) {
		switch (m->st) {
		case I_IN: {
			ffstr in, base, out;
			if (0 > (r = core->com->input_next(m->cmd, &in, &base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					rc = 0;
				}
				goto end;
			}

			uint flags = fcom_file_cominfo_flags_i(m->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(m->in, in.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;

			fffileinfo fi;
			r = core->file->info(m->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;
			if (fffile_isdir(fffileinfo_attr(&fi))) {
				fffd fd = core->file->fd(m->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(m->cmd, fd);
			}

			if (0 != core->com->input_allowed(m->cmd, in))
				continue;

			core->file->close(m->in);

			if (fffile_isdir(fffileinfo_attr(&fi)))
				continue;

			out = in;
			if (m->unbranch)
				unbranch(m, in, base, &out);
			else if (m->unbranch_flat)
				unbranch_flat(m, in, base, &out);

			if (m->search.len != 0) {
				replace(m, &out);
			}

			if (ffstr_eq2(&in, &out))
				continue;

			if (m->cmd->test) {
				fcom_verblog("move: %S -> %S", &in, &out);
				continue;
			}

			r = core->file->move(in, out, FCOM_FILE_MOVE_SAFE);
			if (r == FCOM_FILE_ERR) goto end;
			continue;
		}
		}
	}

end:
	{
	fcom_cominfo *cmd = m->cmd;
	move_close(m);
	core->com->destroy(cmd);
	core->exit(rc);
	}
}

static void move_signal(fcom_op *op, uint signal)
{
	struct move *m = op;
	FFINT_WRITEONCE(m->stop, 1);
}

static const fcom_operation fcom_op_move = {
	move_create, move_close,
	move_run, move_signal,
	move_help,
};


static void move_init(const fcom_core *_core) { core = _core; }
static void move_destroy() {}
static const fcom_operation* move_provide_op(const char *name)
{
	if (ffsz_eq(name, "move"))
		return &fcom_op_move;
	return NULL;
}
FF_EXP const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	move_init, move_destroy, move_provide_op,
};
