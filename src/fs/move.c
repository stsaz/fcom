/** fcom: move files
2022, Simon Zolin */

static const char* move_help()
{
	return "\
Move and/or rename files.\n\
Usage:\n\
  `fcom move` INPUT... [OPTIONS]\n\
\n\
OPTIONS:\n\
    `-u`, `--unbranch`      Move and rename a file out of its directory structure, e.g.\n\
                            `fcom move --unbranch ./a`\n\
                          moves/renames \"./a/b/file\" -> \"./a - b - file\"\n\
    `-uf`, `--unbranch-flat` Move a file out of its directory structure, e.g.\n\
                            `fcom move --unbranch-flat ./a`\n\
                          moves \"./a/b/file\" -> \"./file\"\n\
    `-s`, `--search` 'SEARCH'\n\
    `-r`, `--replace` 'REPLACE'\n\
                        Replace SEARCH text in file name with REPLACE\n\
        `--replace-once`  Replace only the first occurrence\n\
    `-t`, `--tree`          Preserve directory tree, e.g.\n\
                            `fcom move --tree a/b/c -C out`\n\
                          moves \"a/b/c\" to \"out/a/b/c\"\n\
    `-k`, `--skip-errors`   Don't fail on error\n\
";
}

#include <fcom.h>
#include <ffsys/path.h>
#include <ffsys/globals.h>

static const fcom_core *core;

struct move {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	uint stop;
	fcom_file_obj *in;
	ffvec oname, replace_buf;

	byte unbranch, unbranch_flat;
	byte replace_once;
	byte tree;
	byte skip_errors;
	ffstr search, replace;
};

/** --unbranch: prepare output file name.
"parent/base/a/file" -> "parent/base - a - file" */
static ffstr name_unbranch(struct move *m, ffstr in, ffstr base)
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
	ffstr out = *(ffstr*)&m->oname;
	m->oname.len = 0;
	return out;
}

/** --unbranch-flat: prepare output file name.
"parent/base/a/file" -> "parent/file" */
static ffstr name_unbranch_flat(struct move *m, ffstr in, ffstr base)
{
	ffstr parent;
	if (ffpath_splitpath_str(base, &parent, NULL) >= 0) {
		ffvec_addfmt(&m->oname, "%S%c", &parent, FFPATH_SLASH);
	}

	ffstr name;
	ffpath_splitpath_str(in, NULL, &name);
	ffvec_addstr(&m->oname, &name);
	ffstr out = *(ffstr*)&m->oname;
	m->oname.len = 0;
	return out;
}

/** --tree: prepare output file name.
"a/b/file" -> "chdir/a/b/file" */
static ffstr name_tree(struct move *m, ffstr in, ffstr base)
{
	ffvec_addfmt(&m->oname, "%S%c%S"
		, &m->cmd->chdir, FFPATH_SLASH, &in);
	ffstr out = *(ffstr*)&m->oname;
	m->oname.len = 0;
	return out;
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

static int move_input_next(struct move *m, ffstr *in, ffstr *out)
{
	int r;
	ffstr base;
	if (0 > (r = core->com->input_next(m->cmd, in, &base, 0))) {
		if (r == FCOM_COM_RINPUT_NOMORE) {
			return 'done';
		}
		return 'erro';
	}

	if (m->tree) {
		*out = name_tree(m, *in, base);
		return 0;
	}

	uint flags = fcom_file_cominfo_flags_i(m->cmd);
	flags |= FCOM_FILE_READ;
	r = core->file->open(m->in, in->ptr, flags);
	if (r == FCOM_FILE_ERR) return 'erro';

	fffileinfo fi;
	r = core->file->info(m->in, &fi);
	if (r == FCOM_FILE_ERR) return 'erro';

	if (core->com->input_allowed(m->cmd, *in, fffile_isdir(fffileinfo_attr(&fi))))
		return 'next';

	if (fffile_isdir(fffileinfo_attr(&fi))) {
		fffd fd = core->file->fd(m->in, FCOM_FILE_ACQUIRE);
		core->com->input_dir(m->cmd, fd);
	}

	core->file->close(m->in);

	if (fffile_isdir(fffileinfo_attr(&fi))
		&& !(m->search.len && !m->unbranch && !m->unbranch_flat))
		return 'next';

	*out = *in;
	if (m->unbranch)
		*out = name_unbranch(m, *in, base);
	else if (m->unbranch_flat)
		*out = name_unbranch_flat(m, *in, base);

	if (m->search.len) {
		replace(m, out);
	}

	return 0;
}

static int move_process(struct move *m, ffstr in, ffstr out)
{
	if (ffstr_eq2(&in, &out))
		return 0;

	if (m->cmd->test) {
		fcom_verblog("move: %S -> %S", &in, &out);
		return 0;
	}

	int r = core->file->move(in, out, FCOM_FILE_MOVE_SAFE);
	if (r == FCOM_FILE_ERR && !m->skip_errors) return 'erro';
	return 0;
}


#define O(member)  (void*)FF_OFF(struct move, member)

static int args_parse(struct move *m, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--replace",			'S',	O(replace) },
		{ "--replace-once",		'1',	O(replace_once) },
		{ "--search",			'S',	O(search) },
		{ "--skip-errors",		'1',	O(skip_errors) },
		{ "--tree",				'1',	O(tree) },
		{ "--unbranch",			'1',	O(unbranch) },
		{ "--unbranch-flat",	'1',	O(unbranch_flat) },
		{ "-k",					'1',	O(skip_errors) },
		{ "-r",					'S',	O(replace) },
		{ "-s",					'S',	O(search) },
		{ "-t",					'1',	O(tree) },
		{ "-u",					'1',	O(unbranch) },
		{ "-uf",				'1',	O(unbranch_flat) },
		{}
	};
	if (0 != core->com->args_parse(cmd, args, m, FCOM_COM_AP_INOUT))
		return -1;

	if (m->unbranch && m->unbranch_flat) {
		fcom_fatlog("--unbranch and --unbranch-flat can't be used together");
		return -1;

	} else if (m->tree && !cmd->chdir.len) {
		fcom_fatlog("Please use --tree with --chdir");
		return -1;

	} else if (!(cmd->chdir.len || m->unbranch || m->unbranch_flat
		|| m->search.len)) {
		fcom_fatlog("Please use --chdir / --unbranch / --replace");
		return -1;
	}

	return 0;
}

#undef O

static void move_close(fcom_op *op)
{
	struct move *m = op;
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

static void move_run(fcom_op *op)
{
	struct move *m = op;
	int rc = 1;
	enum { I_IN, I_MOVE, };
	ffstr in, out;

	while (!FFINT_READONCE(m->stop)) {
		switch (m->st) {
		case I_IN:
			switch (move_input_next(m, &in, &out)) {
			case 'next':
				continue;

			case 'done':
				rc = 0;
				goto end;

			case 'erro':
				goto end;
			}

			m->st = I_MOVE;
			// fallthrough

		case I_MOVE:
			if (move_process(m, in, out))
				goto end;
			m->st = I_IN;
			continue;
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

FCOM_MOD_DEFINE(move, fcom_op_move, core)
