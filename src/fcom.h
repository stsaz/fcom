/** fcom interfaces.
Copyright (c) 2017 Simon Zolin */

#pragma once

#include <FF/array.h>
#include <FF/data/parse.h>
#include <FF/sys/taskqueue.h>
#include <FF/sys/timer-queue.h>
#include <FFOS/file.h>

/*
CORE
LOG
COMMAND
FOPS
FSYNC
*/

/** THE CORE - manage modules, provide runtime helper functions. */

#define FCOM_VER_MK(maj, minor) \
	(((maj) << 8) | (minor))
#define FCOM_VER_MAJ  0
#define FCOM_VER_MIN  7
#define FCOM_VER  FCOM_VER_MK(FCOM_VER_MAJ, FCOM_VER_MIN)
#define FCOM_VER_STR  "0.7"
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
	byte workers;
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

	/** Assign command to worker.  Must be called on main thread.
	uint assign(fffd *kq, uint flags)
	flags: FCOM_CMD_INTENSE
	Return worker ID. */
	FCOM_WORKER_ASSIGN,

	/** Release command from worker.
	void release(uint wid, uint flags)
	flags: FCOM_CMD_INTENSE
	*/
	FCOM_WORKER_RELEASE,

	/** Get the number of available workers.
	Worker is considered to be busy only if it has a command with FCOM_CMD_INTENSE.
	Return 0: workers are busy;  1: at least 1 worker is free. */
	FCOM_WORKER_AVAIL,

	/** Add a cross-worker task.
	void task_xpost(fftask *task, uint wid) */
	FCOM_TASK_XPOST,

	/** Remove a cross-worker task.
	void task_xdel(fftask *task, uint wid) */
	FCOM_TASK_XDEL,
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

	/** Add task to the main worker.
	@cmd: enum FCOM_TASK */
	void (*task)(uint cmd, fftask *tsk);

	/** Set timer on the main worker.
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

#define FCOM_MODFUNCNAME  "fcom_getmod" //name of the function which is exported by a module
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
typedef const fcom_core* (*core_create_t)(fcom_log log, char **argv, char **env);
typedef void (*core_free_t)(void);


/** COMMAND - manage the chain of modules (filters).
mod1.iface1    mod2.iface1
       \       /
        core.com
*/

enum FCOM_CMD_F {
	FCOM_CMD_FIRST = 1, //filter is the first in chain
	FCOM_CMD_LAST = 2, //filter is the last in chain
	FCOM_CMD_FWD = 4, //moved forward through chain from the previous filter

	FCOM_CMD_EMPTY = 0x10000, //don't auto-create filter from "fcom_cmd.name"

	/** Command is an intensive CPU user. */
	FCOM_CMD_INTENSE = 0x20000,
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
	ffarr2 include_files; //ffstr[]
	ffarr2 exclude_files; //ffstr[]

	struct {
		uint width;
		uint height;
		uint format; //enum FFPIC_FMT
		uint out_format; //enum FFPIC_FMT
	} pic;

	const char *outdir;
	const char *date_as_fn;
	ffstr search;
	ffstr replace;
	fftime mtime;
	byte fsort; //enum FCOM_CMD_SORT
	byte deflate_level; //default:-1
	byte jpeg_quality; //default:-1
	byte png_comp; //default:-1
	byte pic_colors; //default:-1
	uint err :1
		, skip_err :1
		, in_seek :1
		, out_seek :1
		, in_last :1 //the last input data block
		, out_overwrite :1
		, out_resume :1
		, out_preserve_date :1
		, out_notrunc :1
		, out_attr_win :1
		, out_std :1
		, recurse :1
		, read_only :1
		, benchmark :1
		, show :1
		, in_backward :1 //optimize for backward reading (from file end to the beginning)
		, out_fn_copy :1 //output.fn is transient, store its data internally
		;
} fcom_cmd;

// enum FCOM_FILT_OPEN
#define FCOM_OPEN_ERR  NULL //filter has failed to initialize
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
	/** Return object or enum FCOM_FILT_OPEN. */
	void* (*open)(fcom_cmd *cmd);
	void (*close)(void *p, fcom_cmd *cmd);

	/** Return enum FCOM_FILT_R. */
	int (*process)(void *p, fcom_cmd *cmd);
} fcom_filter;

enum FCOM_CMD_CTL {
	FCOM_CMD_MONITOR,
	FCOM_CMD_UDATA,
	FCOM_CMD_SETUDATA,

	/** Call run() asynchronously within the worker thread associated with the command. */
	FCOM_CMD_RUNASYNC,

	/** Get kqueue descriptor associated with the command.
	fffd kq() */
	FCOM_CMD_KQ,

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

#define fcom_cmd_outseek(cmd, off) \
	(cmd)->output.offset = off,  (cmd)->out_seek = 1

struct fcom_cmd_mon {
	/** Called within the worker thread when command object is about to be destroyed. */
	void (*onsig)(fcom_cmd *cmd, uint sig);
};
/** Associate monitor interface with a command. */
#define fcom_cmd_monitor(cmd, mon)  ctrl(cmd, FCOM_CMD_MONITOR, mon)


/** FOPS - operations with files */

enum FOP_F {
	FOP_TEST = 1,
	FOP_OVWR = 2,
	// FOP_RESUME = 4,
	FOP_KEEPDATE = 8,
	FOP_RECURS = 0x10,
	FOP_DIRONLY = 0x20,
	FOP_DIR = 0x40,
};

typedef struct fcom_fops {
	/** File operations.
	@flags: enum FOP_F
	Return 0 on success. */
	int (*mkdir)(const char *fn, uint flags);
	int (*del)(const char *fn, uint flags);
	int (*move)(const char *src, const char *dst, uint flags);
	int (*time)(const char *fn, const fftime *t, uint flags);
} fcom_fops;


/** FSYNC - synchronize files */

struct dir;
struct file;
typedef struct dir fsync_dir;

struct fsync_file {
	char *name;
	uint64 size;
	fftime mtime;
	uint attr;
};

enum FSYNC_CMP {
	// FSYNC_CMP_NAME = 1,
	FSYNC_CMP_SIZE = 2,
	FSYNC_CMP_MTIME = 4,
	FSYNC_CMP_ATTR = 8,
	FSYNC_CMP_MOVE = 0x10,
	FSYNC_CMP_DEFAULT = FSYNC_CMP_SIZE | FSYNC_CMP_MTIME | FSYNC_CMP_MOVE,
	FSYNC_CMP_MTIME_SEC = FSYNC_CMP_MTIME | 0x20,
};

enum FSYNC_ST {
	FSYNC_ST_EQ, // left == right
	FSYNC_ST_SRC, // left -- <empty>
	FSYNC_ST_DEST, // <empty> -- right
	FSYNC_ST_MOVED, // newpath/left == oldpath/right
	FSYNC_ST_NEQ, // left != right
	_FSYNC_ST_MASK = 0x0f,

	_FSYNC_ST_NMASK = 0x0ff0,
	FSYNC_ST_SMALLER = 0x0100,
	FSYNC_ST_LARGER = 0x0200,
	FSYNC_ST_OLDER = 0x0400,
	FSYNC_ST_NEWER = 0x0800,
	FSYNC_ST_ATTR = 0x1000,

	/** 2 file pairs with FSYNC_ST_MOVED are returned.
	File pair for a moved "oldpath/right" is redundant. */
	FSYNC_ST_MOVED_DST = 0x2000,
};

struct fsync_cmp {
	struct file *left;
	struct file *right;
	uint status; //enum FSYNC_ST
};

enum FSYNC_CMD {
	/** Full file name.
	@param: struct file*
	Return char* (newly allocated) */
	FSYNC_FULLNAME,

	/** Full directory name.
	@param: struct file*
	Return char* (static) */
	FSYNC_DIRNAME,
};

typedef struct fcom_fsync {
	/** Scan directories and create a file tree. */
	fsync_dir* (*scan_tree)(const char *fn);

	/** Initialize file tree compare context.
	Destroy with cmp_trees(cmp, NULL).
	@flags: enum FSYNC_CMP */
	void* (*cmp_init)(fsync_dir *left, fsync_dir *right, uint flags);

	/** Get next change from comparing 2 directory trees.
	@result: if NULL, destroy file tree compare context.
	Return -1 if finished. */
	int (*cmp_trees)(void *cmp, struct fsync_cmp *result);

	/** Free the memory allocated for a file tree. */
	void (*tree_free)(fsync_dir *d);

	/** Get property.
	@cmd: enum FSYNC_CMD */
	void* (*get)(uint cmd, ...);
} fcom_fsync;
