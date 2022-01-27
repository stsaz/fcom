/** File operations.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FF/number.h>
#include <FF/crc.h>
#include <FF/data/pe.h>
#include <FF/data/pe-fmt.h>
#include <FFOS/dir.h>
#include <FFOS/file.h>
#include <FFOS/process.h>
#ifdef FF_WIN
#include <FF/gui/winapi.h>
#endif

#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, ##__VA_ARGS__)
#define infolog(dbglev, fmt, ...)  fcom_infolog(FILT_NAME, fmt, ##__VA_ARGS__)
#define verblog(fmt, ...)  fcom_verblog(FILT_NAME, fmt, ##__VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, ##__VA_ARGS__)
#define syserrlog(fmt, ...)  fcom_syserrlog(FILT_NAME, fmt, ##__VA_ARGS__)

const fcom_core *core;
const fcom_command *com;


// MODULE
static int f_sig(uint signo);
static const void* f_iface(const char *name);
static const fcom_mod f_mod = {
	.sig = &f_sig, .iface = &f_iface,
};

// QUICK FILE OPS
static int fop_mkdir(const char *fn, uint flags);
static int fop_del(const char *fn, uint flags);
static int fop_move(const char *src, const char *dst, uint flags);
static int fop_time(const char *fn, const fftime *t, uint flags);
int fop_del_many(const char **names, ffsize n, uint flags);
static const fcom_fops f_ops_iface = {
	&fop_mkdir, &fop_del, &fop_move, &fop_time, fop_del_many,
};

// COPY
static void* f_copy_open(fcom_cmd *cmd);
static void f_copy_close(void *p, fcom_cmd *cmd);
static int f_copy_process(void *p, fcom_cmd *cmd);
static const fcom_filter f_copy_filt = {
	&f_copy_open, &f_copy_close, &f_copy_process,
};


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &f_mod;
}

struct oper {
	const char *name;
	const char *mod;
	const void *iface;
};

#ifdef FF_WIN
extern const fcom_filter wregfind_filt;
#else
static const fcom_filter wregfind_filt;
#endif
extern const fcom_filter txcnt_filt;
extern const fcom_filter utf8_filt;

#include <fileops/crc.h>
#include <fileops/disk.h>
#include <fileops/hex-print.h>
#include <fileops/list.h>
#include <fileops/mount.h>
#include <fileops/pe.h>
#include <fileops/rename.h>
#include <fileops/touch.h>

static const struct oper cmds[] = {
	{ "copy", "file.copy", &f_copy_filt },
	{ "touch", "file.touch", &f_touch_filt },
	{ "rename", "file.rename", &f_rename_filt },
	{ "textcount", "file.textcount", &txcnt_filt },
	{ "utf8", "file.utf8", &utf8_filt },
	{ "hexprint", "file.hexprint", &hexprint_filt },
	{ "crc", "file.crc", &f_crc_filt },
#ifdef FF_WIN
	{ "wregfind", "file.wregfind", &wregfind_filt },
#else
	{ NULL, "file.wregfind", &wregfind_filt },
#endif
	{ NULL, "file.ops", &f_ops_iface },
	{ "peinfo", "file.pe", &f_peinfo },
	{ "disk", "file.disk", &disk_filt },
	{ "mount", "file.mount", &mount_filt },
	{ "list", "file.list", &flist_filt },
};

static const void* f_iface(const char *name)
{
	const struct oper *op;
	FFARR_WALKNT(cmds, FFCNT(cmds), op, struct oper) {
		if (ffsz_eq(name, op->mod + FFSLEN("file.")))
			return op->iface;
	}
	return NULL;
}

static int f_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		com = core->iface("core.com");

		const struct oper *op;
		FFARR_WALKNT(cmds, FFCNT(cmds), op, struct oper) {
			if (op->name != NULL
				&& 0 != com->reg(op->name, op->mod))
				return -1;
		}
		break;
	}
	case FCOM_SIGSTART:
		break;
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}


#define FILT_NAME  "makedir"
static int fop_mkdir(const char *fn, uint flags)
{
	if (flags & FOP_TEST)
		goto done;

	if (flags & FOP_RECURS) {
		if (0 != ffdir_rmake((char*)fn, 0))
			goto err;
	} else {
		if (0 != ffdir_make(fn))
			goto err;
	}

done:
	verblog("%s", fn);
	return 0;

err:
	syserrlog("%s", fn);
	return -1;
}
#undef FILT_NAME

#define FILT_NAME  "remove"

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

int fop_del_many(const char **names, ffsize n, uint flags)
{
	if (flags & FOP_TEST)
		goto done;

	if (flags & FOP_TRASH) {
#ifdef FF_WIN
		if (0 != ffui_fop_del(names, n, FFUI_FOP_ALLOWUNDO))
			goto err;
#else
		if (0 != ffui_glib_trash(names, n))
			goto err;
#endif
		goto done;
	}

done:
	verblog("fop_del_many: ok");
	return 0;

err:
	syserrlog("fop_del_many");
	return -1;
}

static int fop_del(const char *fn, uint flags)
{
	const char *serr = NULL;

	if (flags & FOP_TEST)
		goto done;

	if (flags & FOP_TRASH) {
#ifdef FF_WIN
		if (0 != ffui_fop_del(&fn, 1, FFUI_FOP_ALLOWUNDO))
			goto err;
#else
		if (0 != ffui_glib_trash(&fn, 1))
			goto err;
#endif
		goto done;
	}

	if (flags & FOP_DIR) {
		fffileinfo fi;
		if (0 != fffile_info_path(fn, &fi)) {
			serr = fffile_info_S;
			goto err;
		}
		if (fffile_isdir(fffileinfo_attr(&fi)))
			flags |= FOP_DIRONLY;
	}

	if (flags & FOP_DIRONLY) {
		if (0 != ffdir_remove(fn)) {
			serr = ffdir_rm_S;
			goto err;
		}
	} else {
		if (0 != fffile_remove(fn)) {
			serr = fffile_rm_S;
			goto err;
		}
	}

done:
	verblog("%s", fn);
	return 0;

err:
	syserrlog("%s: %s", serr, fn);
	return -1;
}
#undef FILT_NAME

#define FILT_NAME  "move"
static int fop_move(const char *src, const char *dst, uint flags)
{
	fffileinfo fi;
	if (!(flags & FOP_OVWR) && 0 == fffile_infofn(dst, &fi)) {
		errlog("%s => %s: target file exists", src, dst);
		goto err;
	}

	if (flags & FOP_TEST)
		goto done;

	if (0 != fffile_rename(src, dst)) {

		if ((flags & FOP_RECURS) && fferr_nofile(fferr_last())) {
			if (0 != ffdir_make_path((char*)dst, 0))
				goto err;
			if (0 != fffile_rename(src, dst))
				goto err;
			goto done;
		}
	}

done:
	verblog("'%s' => '%s'", src, dst);
	return 0;

err:
	syserrlog("'%s' => '%s'", src, dst);
	return -1;
}
#undef FILT_NAME

#define FILT_NAME  "ftime"
static int fop_time(const char *fn, const fftime *t, uint flags)
{
	int r = -1;
	fffd f = FF_BADFD;
	const char *serr = NULL;

	if (flags & FOP_TEST)
		goto done;

	if (FF_BADFD == (f = fffile_open(fn, O_WRONLY))) {
		serr = fffile_open_S;
		goto err;
	}
	if (0 != fffile_settime(f, t)) {
		serr = fffile_settime_S;
		goto err;
	}

done:
	verblog("%s", fn);
	r = 0;

err:
	if (r != 0)
		syserrlog("%s: %s", serr, fn);
	fffile_safeclose(f);
	return r;
}
#undef FILT_NAME


#define FILT_NAME  "copy"
static void* f_copy_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}
static void f_copy_close(void *p, fcom_cmd *cmd)
{
}
static int f_copy_process(void *p, fcom_cmd *cmd)
{
	if (NULL == (cmd->input.fn = com->arg_next(cmd, 0)))
		return FCOM_DONE;
	if (cmd->output.fn == NULL)
		return FCOM_ERR;
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(cmd));
	com->ctrl(cmd, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(cmd));
	return FCOM_NEXTDONE;
}
#undef FILT_NAME
