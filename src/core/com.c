/** fcom: core: operation manager; fcom_command implementation.
2022, Simon Zolin */

#include <fcom.h>
#include <ffsys/std.h>
#include <ffsys/pipe.h>
#include <ffbase/map.h>
#include <ffbase/fntree.h>

extern fcom_core *core;

#define syserrlog(fmt, ...)  fcom_syserrlog("com: " fmt, ##__VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog("com: " fmt, ##__VA_ARGS__)
#define dbglog(fmt, ...)  fcom_dbglog("com: " fmt, ##__VA_ARGS__)

struct com {
	fflist cmds; // struct cmd*[]
	ffmap mods; // "name" -> struct mod*
};
static struct com com;

#include <core/mods.h>

struct cmd {
	ffchain_item sib;
	fcom_cominfo cmd;
	const struct fcom_operation *opif;
	fcom_op *op;
	fcom_task task;
	uint argi;

	fntree_block *ftree;
	fntree_cursor ftree_cur;
	ffvec ftree_name;
	fffd ftree_dir;
	uint isdir :1;
	uint set_ftree :1;
	uint args_parsed :1;
	int result;
};

void com_init()
{
	fflist_init(&com.cmds);
	mods_init();
}

void com_destroy()
{
	mods_free();
}

static void com_signal_all(uint signal)
{
	ffchain_item *it;
	FFLIST_WALK(&com.cmds, it) {
		struct cmd *c = FF_STRUCTPTR(struct cmd, sib, it);
		if (c->op == NULL)
			continue;
		c->opif->signal(c->op, signal);
	}
}

static fcom_cominfo* cmd_create()
{
	struct cmd *c = ffmem_new(struct cmd);
	c->result = 1;
	c->cmd.input_fd = FFFILE_NULL;
	ffstr path = {};
	c->ftree = fntree_create(path);
	c->ftree_dir = FFFILE_NULL;
	fflist_add(&com.cmds, &c->sib);
	return &c->cmd;
}

static void cmd_destroy(fcom_cominfo *cmd)
{
	if (cmd == NULL)
		return;

	struct cmd *c = FF_STRUCTPTR(struct cmd, cmd, cmd);

	if (cmd->on_complete != NULL) {
		fcom_dbglog("%s: calling on_complete(): %d", cmd->operation, c->result);
		cmd->on_complete(cmd->opaque, c->result);
	}

	ffstr *it;

	FFSLICE_WALK(&cmd->input, it) {
		ffstr_free(it);
	}
	ffvec_free(&cmd->input);

	FFSLICE_WALK(&cmd->include, it) {
		ffstr_free(it);
	}
	ffvec_free(&cmd->include);

	FFSLICE_WALK(&cmd->exclude, it) {
		ffstr_free(it);
	}
	ffvec_free(&cmd->exclude);

	fntree_free_all(c->ftree);
	ffvec_free(&c->ftree_name);
	if (c->ftree_dir != FFFILE_NULL)
		fffile_close(c->ftree_dir);
	ffstr_free(&cmd->output);
	ffstr_free(&cmd->chdir);
	if (cmd->input_fd != FFFILE_NULL && cmd->input_fd != ffstdin)
		fffile_close(cmd->input_fd);
	fflist_rm(&com.cmds, &c->sib);

	for (uint i = 0;  i < cmd->argc;  i++) {
		ffmem_free(cmd->argv[i]);
	}
	ffmem_free(cmd->argv);

	char *op = cmd->operation;
	ffmem_free(c);
	fcom_dbglog("%s: command finished", op);
	ffmem_free(op);
}

static void cmd_run_async(void *param);

static void cmd_async(fcom_cominfo *cmd)
{
	struct cmd *c = FF_STRUCTPTR(struct cmd, cmd, cmd);
	fcom_dbglog("%s: async", cmd->operation);
	core->task(&c->task, cmd_run_async, c);
}

static void cmd_complete(fcom_cominfo *cmd, int code)
{
	struct cmd *c = FF_STRUCTPTR(struct cmd, cmd, cmd);
	c->result = code;
	int primary = (cmd->on_complete == NULL);
	cmd_destroy(cmd);
	if (primary)
		core->exit(code);
}

#define FFCMDARG_ERROR 0xbad

#ifdef FF_WIN
static int wc_expand(struct fcom_cominfo *cmd, ffstr *s)
{
	int rc = FFCMDARG_ERROR;
	ffstr dir, name;
	ffpath_splitpath(s->ptr, s->len, &dir, &name);
	if (dir.len == 0)
		ffstr_setz(&dir, ".");
	char *dirz = ffsz_dupstr(&dir);

	ffdirscan ds = {};
	ds.wildcard = ffsz_dupstr(&name);
	if (0 != ffdirscan_open(&ds, dirz, FFDIRSCAN_USEWILDCARD)) {
		syserrlog("dir open: %s", dirz);
		goto err;
	}

	const char *fn;
	while (NULL != (fn = ffdirscan_next(&ds))) {
		dbglog("wildcard expand: %s", fn);
		ffstr *p = ffvec_zpushT(&cmd->input, ffstr);
		ffsize cap = 0;
		ffstr_growfmt(p, &cap, "%S\\%s", &dir, fn);
	}

	rc = 0;

err:
	ffmem_free(dirz);
	ffmem_free((char*)ds.wildcard);
	ffdirscan_close(&ds);
	return rc;
}
#endif

static int args_input(struct fcom_cominfo *cmd, ffstr s)
{
	if (s.len && s.ptr[0] == '-') {
		errlog("Unknown option: %S", &s);
		return FFCMDARG_ERROR;
	}

	if (s.len == 0) {
		cmd->stdin = 1;
	} else if (s.ptr[0] == '@') {
		if (cmd->input_fd != FFFILE_NULL) {
			errlog("Only 1 '@' notation for input files is supported");
			return FFCMDARG_ERROR;
		}
		if (s.len == 1) {
			cmd->input_fd = ffstdin;
			dbglog("reading input names from STDIN");
		} else {
			const char *fn = s.ptr + 1;
			if (FFFILE_NULL == (cmd->input_fd = fffile_open(fn, FFFILE_READONLY))) {
				syserrlog("file open: %s", fn);
				return FFCMDARG_ERROR;
			}
			dbglog("reading input names from file '%s'", fn);
		}
		return 0;

#ifdef FF_WIN
	} else if (ffstr_findany(&s, "*?", 2) >= 0
		&& !ffstr_matchz(&s, "\\\\?\\")) {
		return wc_expand(cmd, &s);
#endif

	}

	ffstr *p = ffvec_zpushT(&cmd->input, ffstr);
	p->ptr = ffsz_dupstr(&s);
	p->len = s.len;
	return 0;
}

static int args_include(struct fcom_cominfo *cmd, ffstr s)
{
	ffstr *p = ffvec_zpushT(&cmd->include, ffstr);
	ffstr_dupstr(p, &s);
	return 0;
}

static int args_exclude(struct fcom_cominfo *cmd, ffstr s)
{
	ffstr *p = ffvec_zpushT(&cmd->exclude, ffstr);
	ffstr_dupstr(p, &s);
	return 0;
}

static int args_buffer(struct fcom_cominfo *cmd, ffint64 i)
{
	if (i == 0) {
		errlog("--buffer: incorrect value");
		return FFCMDARG_ERROR;
	}
	cmd->buffer_size = i;
	return 0;
}

#define R_DONE 100

#if defined FF_WIN
	#define OS_STR  "windows"
#elif defined FF_BSD
	#define OS_STR  "freebsd"
#elif defined FF_APPLE
	#define OS_STR  "macos"
#else
	#define OS_STR  "linux"
#endif

static const char* app_ver()
{
	return "fcom v" FCOM_VER " (" OS_STR ")\n";
}

static int args_help(struct fcom_cominfo *cmd)
{
	cmd->help = 1;
	return 0;
}

static int op_get(struct cmd *c)
{
	if (c->cmd.operation == NULL) {
		if (!(c->cmd.argc != 0 && c->cmd.argv[0][0] != '-')) {
			ffstdout_write(app_ver(), ffsz_len(app_ver()));
			const char *short_help =
				"General usage:\n"
				"\n"
				"  fcom [GLOBAL-OPTIONS] OPERATION [INPUT] [-o OUTPUT] [OPTIONS]\n"
				"\n"
				"Run `fcom -h` for more info.\n";
			ffstdout_write(short_help, ffsz_len(short_help));
			return 1;
		}

		c->cmd.operation = ffsz_dup(c->cmd.argv[0]);
		c->argi = 1; // skip operation arg
	}

	return 0;
}

static void cmd_run_async(void *param)
{
	struct cmd *c = param;
	fcom_dbglog("%s: run", c->cmd.operation);
	c->opif->run(c->op);
}

static int cmd_run(fcom_cominfo *cmd)
{
	struct cmd *c = FF_STRUCTPTR(struct cmd, cmd, cmd);

	if (0 != op_get(c))
		goto err;

	if (NULL == (c->opif = com_provide(cmd->operation, FCOM_COM_PROVIDE_PRIM)))
		goto err;

	if (NULL == (c->op = c->opif->create(cmd)))
		goto err;

	if (c->cmd.on_complete != NULL) {
		core->task(&c->task, cmd_run_async, c);
		return 0;
	}

	c->opif->run(c->op);
	return 0;

err:
	cmd_destroy(cmd);
	return -1;
}

static int input_names_read(struct cmd *c)
{
	fcom_cominfo *cmd = &c->cmd;
	int rc = -1;
	ffvec in_list = {};

	for (;;) {
		ffvec_grow(&in_list, 64*1024, 1);
		dbglog("reading from input names file...");
		ffssize r;
		if (cmd->input_fd == ffstdin)
			r = ffpipe_read(cmd->input_fd, ffslice_end(&in_list, 1), ffvec_unused(&in_list));
		else
			r = fffile_read(cmd->input_fd, ffslice_end(&in_list, 1), ffvec_unused(&in_list));
		if (r == 0) {
			break;
		} else if (r < 0) {
			syserrlog("input names file read");
			goto end;
		}
		dbglog("input names file: read %L bytes", r);
		in_list.len += r;
	}

	ffstr view = FFSTR_INITSTR(&in_list);
	while (view.len != 0) {
		ffstr name;
		ffstr_splitby(&view, '\n', &name, &view);
		ffstr_trimwhite(&name);
		if (name.len == 0)
			continue;

		if (NULL == fntree_add(&c->ftree, name, 0)) {
			fcom_errlog("fntree_add");
			goto end;
		}
	}

	rc = 0;

end:
	ffvec_free(&in_list);
	return rc;
}

/**
Note: all directories must be always included because user expects -I '*.txt' to work */
static int cmd_input_allowed(fcom_cominfo *cmd, ffstr name, uint flags)
{
	int k = 1;
	ffstr *it;
	uint wcflags = FFS_WC_ICASE;

	int isdir = (flags == FCOM_COM_IA_DIR);
	if (flags == FCOM_COM_IA_AUTO && cmd->include.len) {
		fffileinfo fi;
		if (!fffile_info_path(name.ptr, &fi))
			isdir = fffile_isdir(fffileinfo_attr(&fi));
	}

	if (cmd->include.len != 0 && !isdir) {
		k = 0;
		FFSLICE_WALK(&cmd->include, it) {
			if (0 == ffs_wildcard(it->ptr, it->len, name.ptr, name.len, wcflags)) {
				dbglog("include: '%S' by '%S'", &name, it);
				k = 1;
				break;
			}
		}

		if (!k)
			return 1;
	}

	FFSLICE_WALK(&cmd->exclude, it) {
		if (0 == ffs_wildcard(it->ptr, it->len, name.ptr, name.len, wcflags)) {
			dbglog("exclude: '%S' by '%S'", &name, it);
			k = 0;
			break;
		}
	}

	if (!k)
		return 2;
	return 0;
}

static const fntree_block* _fntr_cur_i(fntree_cursor *c, uint i)
{
	if (i >= c->depth)
		return NULL;
	return c->block_stk[i];
}

static int cmd_input_next(fcom_cominfo *cmd, ffstr *name, ffstr *ubase, uint flags)
{
	struct cmd *c = FF_STRUCTPTR(struct cmd, cmd, cmd);

	if (!c->set_ftree) {
		c->set_ftree = 1;
		ffstr *it;
		FFSLICE_WALK(&c->cmd.input, it) {
			if (NULL == fntree_add(&c->ftree, *it, 0)) {
				fcom_errlog("fntree_add: '%S'", it);
				return FCOM_COM_RINPUT_ERR;
			}
		}
	}

	if (c->cmd.input_fd != FFFILE_NULL) {
		if (0 != input_names_read(c))
			return FCOM_COM_RINPUT_ERR;
		if (cmd->input_fd != ffstdin) {
			fffile_close(cmd->input_fd);
		}
		cmd->input_fd = FFFILE_NULL;
	}

	if (c->isdir) {
		c->isdir = 0;
		ffdirscan ds = {};
		uint flags = 0;

#ifdef FF_LINUX
		if (c->ftree_dir != FFFILE_NULL) {
			ds.fd = c->ftree_dir;
			c->ftree_dir = FFFILE_NULL;
			flags = FFDIRSCAN_USEFD;
		}
#endif

		if (0 != ffdirscan_open(&ds, c->ftree_name.ptr, flags)) {
			fcom_syserrlog("ffdirscan_open: %s", c->ftree_name.ptr);
			return FCOM_COM_RINPUT_ERR;
		}
		ffstr path = FFSTR_INITZ(c->ftree_name.ptr);
		fntree_block *b = fntree_from_dirscan(path, &ds, 0);
		fntree_attach((fntree_entry*)c->ftree_cur.cur, b);
		ffdirscan_close(&ds);
	}

	fntree_block *b;
	ffstr base;
	for (;;) {
		b = c->ftree;
		fntree_entry *it;
		if (flags & FCOM_COM_INPUT_DIRFIRST)
			it = fntree_cur_next_r_ctx(&c->ftree_cur, &b);
		else
			it = fntree_cur_next_r(&c->ftree_cur, &b);
		if (it == NULL) {
			fcom_dbglog("no more input files");
			return FCOM_COM_RINPUT_NOMORE;
		}

		ffstr nm = fntree_name(it);
		if (NULL != _fntr_cur_i(&c->ftree_cur, 0)) {
			const fntree_block *bbase = _fntr_cur_i(&c->ftree_cur, 1);
			if (bbase == NULL)
				bbase = b;
			base = fntree_path(bbase);
		} else {
			ffstr_null(&base);
		}

		ffstr path = fntree_path(b);
		c->ftree_name.len = 0;
		if (path.len != 0)
			ffvec_addfmt(&c->ftree_name, "%S%c", &path, FFPATH_SLASH);
		ffvec_addfmt(&c->ftree_name, "%S%Z", &nm);

		ffstr_set(name, c->ftree_name.ptr, c->ftree_name.len - 1);
		break;
	}

	dbglog("input file name: '%S' / '%S'", name, &base);
	if (ubase != NULL)
		*ubase = base;
	return FCOM_COM_RINPUT_OK;
}

static void cmd_input_dir(fcom_cominfo *cmd, fffd dir)
{
	struct cmd *c = FF_STRUCTPTR(struct cmd, cmd, cmd);
	c->isdir = 1;

#ifdef FF_LINUX
	c->ftree_dir = dir;
	return;
#endif

	fffile_close(dir);
}

#define O(member)  (void*)FF_OFF(struct fcom_cominfo, member)

static const struct ffarg args_global[] = {
	{ "--Exclude",		'+S',	args_exclude },
	{ "--Include",		'+S',	args_include },
	{ "--Recursive",	'1',	O(recursive) },

	{ "--buffer",		'Z',	args_buffer },
	{ "--chdir",		'=S',	O(chdir) },
	{ "--directio",		'1',	O(directio) },
	{ "--help",			'0',	args_help },
	{ "--no-prealloc",	'1',	O(no_prealloc) },
	{ "--out",			'=s',	O(outputz) },
	{ "--overwrite",	'1',	O(overwrite) },
	{ "--test",			'1',	O(test) },

	{ "-C",				'=S',	O(chdir) },
	{ "-E",				'+S',	args_exclude },
	{ "-I",				'+S',	args_include },
	{ "-R",				'1',	O(recursive) },
	{ "-T",				'1',	O(test) },

	{ "-f",				'1',	O(overwrite) },
	{ "-h",				'0',	args_help },
	{ "-o",				'=s',	O(outputz) },
	{ "\0\1",			'+S',	args_input },
	{}
};

#undef O

static void help_info_write(ffstr s)
{
	ffstr l, k;
	ffvec v = {};
	int use_color = !ffstd_attr(ffstdout, FFSTD_VTERM, FFSTD_VTERM);

	const char *clr = FFSTD_CLR_B(FFSTD_PURPLE);
	while (s.len) {
		ffstr_splitby(&s, '`', &l, &s);
		ffstr_splitby(&s, '`', &k, &s);
		if (use_color) {
			ffvec_addfmt(&v, "%S%s%S%s"
				, &l, clr, &k, FFSTD_CLR_RESET);
		} else {
			ffvec_addfmt(&v, "%S%S"
				, &l, &k);
		}
	}

	ffstdout_write(v.ptr, v.len);
	ffvec_free(&v);
}

/** Process both general and operation-specific command-line arguments */
static int cmd_args_parse(fcom_cominfo *cmd, const struct ffarg *args, void *obj, uint flags)
{
	struct cmd *c = FF_STRUCTPTR(struct cmd, cmd, cmd);
	FCOM_ASSERT(!c->args_parsed);
	c->args_parsed = 1;

	fcom_cominfo *ucmd = obj;
	*ucmd = *cmd;

	uint n_args = 0;
	for (;  args[n_args].name[0];  n_args++) {}

	ffvec a = {};
	ffvec_allocT(&a, FF_COUNT(args_global) + n_args, struct ffarg);
	uint n_g = 0;
	if (flags & FCOM_COM_AP_INOUT)
		n_g = FF_COUNT(args_global);
	a.len = ffarg_merge(a.ptr, a.cap, args, n_args, args_global, n_g, 0);

	struct ffargs as = {};
	int r = ffargs_process_argv(&as, a.ptr, obj, FFARGS_O_PARTIAL | FFARGS_O_DUPLICATES, cmd->argv + c->argi, cmd->argc - c->argi);
	if (r < 0) {
		if (r != -R_DONE)
			errlog("command-line: %s", as.error);
		goto end;
	}

	*cmd = *ucmd;

	if (cmd->help) {
		const char *s = c->opif->help();
		help_info_write(FFSTR_Z(s));
		r = 1;
		goto end;
	}

	if (cmd->outputz != NULL)
		ffstr_setz(&cmd->output, cmd->outputz);
	if (ffstr_eqz(&cmd->output, "STDOUT")) {
		ffstr_free(&cmd->output);
		cmd->stdout = 1;
		core->stdout_busy = 1;
	}

end:
	ffvec_free(&a);
	return r;
}

const struct fcom_command _fcom_com = {
	com_provide,
	com_signal_all,
	cmd_create,
	cmd_run, cmd_async,
	cmd_destroy, cmd_complete,
	cmd_input_next, cmd_input_dir, cmd_input_allowed,
	cmd_args_parse,
};
