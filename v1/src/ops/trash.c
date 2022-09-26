/** fcom: move files to user's trash directory
2022, Simon Zolin */

#ifdef _WIN32
#include <util/winapi-shell.h>
#else
#include <FFOS/process.h>
#endif
#include <fcom.h>
#include <FFOS/path.h>

static const fcom_core *core;

struct trash {
	uint st;
	fcom_cominfo *cmd;
	fcom_file_obj *in;
	uint stop;
	ffvec names; //char*[]
};

static const char* trash_help()
{
	return "\
Move files to user's trash directory.\n\
Usage:\n\
  fcom trash INPUT\n\
";
}

static int args_parse(struct trash *l, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{}
	};
	return core->com->args_parse(cmd, args, l);
}

static void trash_close(fcom_op *op);

static fcom_op* trash_create(fcom_cominfo *cmd)
{
	struct trash *l = ffmem_new(struct trash);
	l->cmd = cmd;

	if (0 != args_parse(l, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	l->in = core->file->create(&fc);
	return l;

end:
	trash_close(l);
	return NULL;
}

static void trash_close(fcom_op *op)
{
	struct trash *l = op;
	core->file->destroy(l->in);

	char **p;
	FFSLICE_WALK(&l->names, p) {
		ffmem_free(*p);
	}
	ffvec_free(&l->names);

	ffmem_free(l);
}

#ifdef FF_LINUX
/** Exec and wait
Return exit code or -1 on error */
static inline int _ffui_ps_exec_wait(const char *filename, const char **argv, const char **env)
{
	ffps_execinfo info = {};
	info.argv = argv;
	info.env = env;
	ffps ps = ffps_exec_info(filename, &info);
	if (ps == FFPS_NULL)
		return -1;

	int code;
	if (0 != ffps_wait(ps, -1, &code))
		return -1;

	return code;
}

/** Move files to Trash */
static inline int ffui_glib_trash(const char **names, ffsize n)
{
	ffvec v = {};
	if (NULL == ffvec_allocT(&v, 3 + n, char*))
		return -1;
	char **p = (char**)v.ptr;
	*p++ = "/usr/bin/gio";
	*p++ = "trash";
	for (ffsize i = 0;  i != n;  i++) {
		*p++ = (char*)names[i];
	}
	*p++ = NULL;
	int r = _ffui_ps_exec_wait(((char**)v.ptr)[0], (const char**)v.ptr, (const char**)environ);
	ffvec_free(&v);
	return r;
}
#endif

static void trash_run(fcom_op *op)
{
	struct trash *l = op;
	int r, rc = 1;
	ffstr name;
	while (!FFINT_READONCE(l->stop)) {
		switch (l->st) {
		case 0: {
			if (0 > (r = core->com->input_next(l->cmd, &name, NULL, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
#ifdef FF_LINUX
					if (0 != (r = ffui_glib_trash(l->names.ptr, l->names.len))) {
						fcom_fatlog("can't move files to trash (KDE): error code %d", r);
						goto end;
					}

#else
					if (0 != (r = ffui_fop_del(l->names.ptr, l->names.len, FFUI_FOP_ALLOWUNDO))) {
						fcom_sysfatlog("can't move files to trash (Windows)");
						goto end;
					}
#endif

					if (core->verbose) {
						const char **namez;
						FFSLICE_WALK(&l->names, namez) {
							fcom_verblog("trash: %s", *namez);
						}
						fcom_verblog("moved %L files to Trash", l->names.len);
					}

					rc = 0;
				}
				goto end;
			}

			char **p = ffvec_pushT(&l->names, char*);
			*p = ffsz_dupstr(&name);
			continue;
		}
		}
	}

end:
	fcom_cominfo *cmd = l->cmd;
	trash_close(l);
	core->com->complete(cmd, rc);
}

static void trash_signal(fcom_op *op, uint signal)
{
	struct trash *l = op;
	FFINT_WRITEONCE(l->stop, 1);
}

static const fcom_operation fcom_op_trash = {
	trash_create, trash_close,
	trash_run, trash_signal,
	trash_help,
};


static void trash_init(const fcom_core *_core) { core = _core; }
static void trash_destroy() {}
static const fcom_operation* trash_provide_op(const char *name)
{
	if (ffsz_eq(name, "trash"))
		return &fcom_op_trash;
	return NULL;
}
FF_EXP const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	trash_init, trash_destroy, trash_provide_op,
};
