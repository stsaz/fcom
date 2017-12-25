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
#define FCOM_VER_MIN  2
#define FCOM_VER  FCOM_VER_MK(FCOM_VER_MAJ, FCOM_VER_MIN)
#define FCOM_VER_STR  "0.2"
#define FCOM_CONF_FN  "fcom.conf"

enum FCOM_LOGLEV {
	FCOM_LOGERR,
	FCOM_LOGWARN,
	FCOM_LOGINFO,
	FCOM_LOGVERB,
	FCOM_LOGDBG,
	_FCOM_LEVMASK = 0x0f,

	FCOM_LOGSYS = 0x10,
	FCOM_LOGNOPFX = 0x20, //no prefix
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


/** COMMAND - manage the chain of modules (filters).
mod1.iface1    mod2.iface1
       \       /
        core.com
*/

enum FCOM_CMD_F {
	FCOM_CMD_FIRST = 1, //filter is the first in chain
	FCOM_CMD_LAST = 2, //filter is the last in chain
	FCOM_CMD_FWD = 4, //moved forward through chain from the previous filter
};

enum FCOM_CMD_SORT {
	FCOM_CMD_SORT_ALPHA,
	FCOM_CMD_SORT_DIRS_FILES,
	FCOM_CMD_SORT_FILES_DIRS,
};

/** Configuration of a command, shared data between filters. */
typedef struct fcom_cmd {
	const char *name;
	uint flags; //enum FCOM_CMD_F

	// data being transferred from one module to the next in chain
	ffstr in;
	ffstr out;

	struct {
		const char *fn;
		fftime mtime;
		uint64 size;
		uint64 offset;
		uint attr;
	} input, output;

	ffarr2 members; //char*[]

	const char *outdir;
	const char *date_as_fn;
	fftime mtime;
	byte fsort; //enum FCOM_CMD_SORT
	byte deflate_level;
	uint err :1
		, skip_err :1
		, in_seek :1
		, in_last :1 //the last input data block
		, out_overwrite :1
		, out_notrunc :1
		, out_attr_win :1
		, out_std :1
		, recurse :1
		, read_only :1
		, benchmark :1
		, show :1
		;
} fcom_cmd;

// enum FCOM_FILT_OPEN
#define FCOM_SKIP  ((void*)-1) //filter refuses to be added to the chain
#define FCOM_OPEN_DUMMY ((void*)1) //filter doesn't have a context
#define FCOM_OPEN_SYSERR ((void*)2) //print system error message

enum FCOM_FILT_R {
	FCOM_MORE, //get data from the previous filter
	FCOM_BACK, //same as FCOM_MORE, but pass the current output data to the previous filter

	FCOM_DATA, //pass data to the next filter
	FCOM_DONE, //same as FCOM_DATA, but also remove this filter
	FCOM_OUTPUTDONE, //same as FCOM_DONE, but also close all previous filters
	FCOM_NEXTDONE, //close all next filters and return to this filter

	FCOM_ERR, //close the chain with an error
	FCOM_SYSERR, //same as FCOM_ERR, but also print system error
	FCOM_FIN, //close the chain
	FCOM_ASYNC, //pause processing until explicitly resumed by the current filter
};

/** A module implements this interface to act as a filter in command's chain. */
typedef struct fcom_filter {
	/** Return object or enum FCOM_FILT_OPEN; NULL on error. */
	void* (*open)(fcom_cmd *cmd);
	void (*close)(void *p, fcom_cmd *cmd);

	/** Return enum FCOM_FILT_R. */
	int (*process)(void *p, fcom_cmd *cmd);
} fcom_filter;

enum FCOM_CMD_CTL {
	FCOM_CMD_MONITOR,

	/** Add a filter to the chain.
	Return a pointer that can later be passed as a parameter to FCOM_CMD_FILTADD_AFTER. */
	FCOM_CMD_FILTADD_PREV,
	FCOM_CMD_FILTADD,
	FCOM_CMD_FILTADD_AFTER,
	FCOM_CMD_FILTADD_LAST,
};

enum FCOM_CMD_ARG {
	FCOM_CMD_ARG_PEEK = 1, // get next argument, but don't increment cursor
};

/** Execute commands. */
typedef struct fcom_command {
	/** Create context for a new command.  Add filter associated with command name. */
	void* (*create)(fcom_cmd *c);

	/** Close command context and all its filters. */
	void (*close)(void *p);

	/** Call filters within the chain. */
	int (*run)(void *p);

	/** Set command's parameters.
	@cmd: enum FCOM_CMD_CTL */
	size_t (*ctrl)(fcom_cmd *c, uint cmd, ...);

	/** Add/get command's arguments.
	@flags: enum FCOM_CMD_ARG
	*/
	int (*arg_add)(fcom_cmd *c, const ffstr *arg, uint flags);
	char* (*arg_next)(fcom_cmd *c, uint flags);

	/** Associate a command name with filter. */
	int (*reg)(const char *op, const char *mod);
} fcom_command;

#define fcom_cmd_filtadd(c, modname)  ctrl(c, FCOM_CMD_FILTADD, modname)
#define fcom_cmd_filtadd_prev(c, modname)  ctrl(c, FCOM_CMD_FILTADD_PREV, modname)

/** Get the name of data input/output filter. */
#define FCOM_CMD_FILT_IN(cmd)  (ffsz_eq((cmd)->input.fn, "@stdin") ? "core.stdin" : "core.file-in")
#define FCOM_CMD_FILT_OUT(cmd)  (((cmd)->out_std) ? "core.stdout" : "core.file-out")

#define fcom_cmd_seek(cmd, off) \
	(cmd)->input.offset = off,  (cmd)->in_seek = 1

struct fcom_cmd_mon {
	void (*onsig)(fcom_cmd *cmd, uint sig);
};
/** Associate monitor interface with a command. */
#define fcom_cmd_monitor(cmd, mon)  ctrl(cmd, FCOM_CMD_MONITOR, mon)
