/** fcom interfaces.
Copyright (c) 2017 Simon Zolin */

#pragma once

#include <FF/array.h>
#include <FF/data/parse.h>
#include <FF/sys/taskqueue.h>
#include <FF/sys/timer-queue.h>
#include <FFOS/file.h>


/** THE CORE - manage modules, provide runtime helper functions. */

#define FCOM_VER_MK(maj, minor) \
	(((maj) << 8) | (minor))
#define FCOM_VER_MAJ  0
#define FCOM_VER_MIN  1
#define FCOM_VER_STR  "0.1"
#define FCOM_CONF_FN  "fcom.conf"

enum FCOM_LOGLEV {
	FCOM_LOGERR,
	FCOM_LOGWARN,
	FCOM_LOGINFO,
	FCOM_LOGVERB,
	FCOM_LOGDBG,
	_FCOM_LEVMASK = 0x0f,

	FCOM_LOGSYS = 0x10,
};

typedef struct fcom_conf {
	uint loglev;
} fcom_conf;

enum FCOM_TASK {
	FCOM_TASK_ADD,
	FCOM_TASK_DEL,
};

enum FCOM_CMD {
	FCOM_READCONF,
	FCOM_SETCONF,
	FCOM_MODADD,
	FCOM_RUN,
	FCOM_STOP,
};

typedef struct fcom_core fcom_core;
struct fcom_core {
	fcom_conf *conf;
	fffd kq;

	/**
	@cmd: enum FCOM_CMD */
	int (*cmd)(uint cmd, ...);

	/**
	@flags: enum FCOM_LOGLEV */
	void (*log)(uint flags, const char *fmt, ...);

	/** Return NULL on error. */
	char* (*getpath)(const char *name, size_t len);

	/** Return NULL on error. */
	char* (*env_expand)(char *dst, size_t cap, const char *src);

	/** Return NULL on error. */
	const void* (*iface)(const char *name);

	/**
	@cmd: enum FCOM_TASK */
	void (*task)(uint cmd, fftask *tsk);

	/**
	@interval: msec.  >0: periodic;  <0: one-shot;  0: disable.
	Return 0 on success. */
	int (*timer)(fftmrq_entry *tmr, int interval, uint flags);
};

#define fcom_core_readconf(conffn) \
	cmd(FCOM_READCONF, (char*)(conffn))
#define fcom_core_setconf(conf) \
	cmd(FCOM_SETCONF, (fcom_conf*)(conf))
#define fcom_core_modadd(name, confctx) \
	cmd(FCOM_MODADD, (ffstr*)(name), (ffpars_ctx*)(confctx))

enum FCOM_SIG {
	FCOM_SIGINIT,
	FCOM_SIGSTART,
	FCOM_SIGFREE,
};

typedef struct fcom_mod fcom_mod;
struct fcom_mod {
	/**
	@signo: enum FCOM_SIG. */
	int (*sig)(uint signo);
	const void* (*iface)(const char *name);
	int (*conf)(const char *name, ffpars_ctx *ctx);

	uint ver;
	const char *name;
	const char *desc;
};

/** When adding a new module, the core gets "fcom_getmod" function address from module and calls it.
Return NULL on error. */
typedef const fcom_mod* (*fcom_getmod_t)(const fcom_core *core);


/** LOG - print messages to stdout/err.
module -> fcom_core.log() -> logger()
*/

typedef int (*fcom_log)(uint flags, const char *fmt, va_list va);

#define fcom_logchk(level, lev)  (((level) & _FCOM_LEVMASK) >= ((lev) & _FCOM_LEVMASK))

#define fcom_dbglog(dbglev, mod, fmt, ...) \
do { \
	if (fcom_logchk((core)->conf->loglev, FCOM_LOGDBG)) \
		(core)->log(FCOM_LOGDBG, mod ": " fmt, __VA_ARGS__); \
} while (0)

#define fcom_verblog(mod, fmt, ...)  (core)->log(FCOM_LOGVERB, mod ": " fmt, __VA_ARGS__)
#define fcom_infolog(mod, fmt, ...)  (core)->log(FCOM_LOGINFO, mod ": " fmt, __VA_ARGS__)
#define fcom_warnlog(mod, fmt, ...)  (core)->log(FCOM_LOGWARN, mod ": " fmt, __VA_ARGS__)
#define fcom_errlog(mod, fmt, ...)  (core)->log(FCOM_LOGERR, mod ": " fmt, __VA_ARGS__)
#define fcom_syswarnlog(mod, fmt, ...)  (core)->log(FCOM_LOGWARN | FCOM_LOGSYS, mod ": " fmt, __VA_ARGS__)
#define fcom_syserrlog(mod, fmt, ...)  (core)->log(FCOM_LOGERR | FCOM_LOGSYS, mod ": " fmt, __VA_ARGS__)


/** Initialize/destroy core.  These functions are called from the application executable. */
FF_EXTN const fcom_core* core_create(fcom_log log, char **argv, char **env);
FF_EXTN void core_free(void);
