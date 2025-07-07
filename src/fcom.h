/** fcom: public interface for inter-module communication */

#pragma once
#include <ffsys/error.h>
#include <util/taskqueue.h>
#include <ffsys/file.h>
#include <ffsys/timerqueue.h>
#include <ffsys/time.h>
#include <ffbase/vector.h>
#include <ffbase/args.h>
#include <assert.h>
#undef stdin
#undef stdout

#define FCOM_VER "1.0.22"
#define FCOM_CORE_VER 10021

typedef unsigned char byte;
typedef unsigned char u_char;
typedef unsigned short ushort;
typedef unsigned int uint;
typedef unsigned long long uint64;
typedef long long int64;

#ifdef __cplusplus
#define FCOM_EXPORT extern "C" FF_EXPORT
#else
#define FCOM_EXPORT FF_EXPORT
#endif

// CORE

#define FCOM_ASSERT  assert

enum FCOM_LOG {
	FCOM_LOG_FATAL,
	FCOM_LOG_ERR,
	FCOM_LOG_WARN,
	FCOM_LOG_INFO,
	FCOM_LOG_VERBOSE,
	FCOM_LOG_DBG,
	FCOM_LOG_SYSERR = 0x10,
};

/**
flags: enum FCOM_LOG */
typedef void (*fcom_log_func)(uint flags, const char *fmt, va_list args);

typedef struct fcom_core fcom_core;

struct fcom_core_conf {
	uint timer_resolution_msec;
	char *app_path;
	fcom_log_func log;
	uint codepage;
	uint debug :1;
	uint verbose :1;
	uint stdout_color :1;
};

/** Core initializer.
core.so exports this interface as "fcom_coreinit" */
struct fcom_coreinit {
	void (*init)();
	void (*destroy)();
	fcom_core* (*conf)(struct fcom_core_conf *conf);
	int (*run)();
};

typedef struct fcom_command fcom_command;
typedef struct fcom_file fcom_file;
typedef fftask fcom_task;
typedef fftimerqueue_node fcom_timer;
typedef void (*fcom_task_func)(void *param);

typedef struct fcom_kevent {
	fcom_task_func func;
	void *param;
} fcom_kevent;

static inline void fcom_kevent_set(fcom_kevent *kev, fcom_task_func func, void *param)
{
	kev->func = func;
	kev->param = param;
}

enum FCOM_CORE_CLOCK {
	FCOM_CORE_MONOTONIC,
	FCOM_CORE_UTC,
};

/** Core runtime interface */
struct fcom_core {
	/** Operation command interface */
	const fcom_command *com;
	/** File submodule interface */
	const fcom_file *file;
	fftime_zone tz;
	const char **env;
	uint codepage;

	/** Exit from fcom_coreinit.run() */
	void (*exit)(int exit_code);

	/** Get absolute path for files in fcom/ directory.
	Return newly allocated string, free with ffmem_free(). */
	char* (*path)(const char *name);

	/** Attach file descriptor to KQ */
	int (*kq_attach)(fffd fd, fcom_kevent *kev, uint flags);

	/** Add/remove asynchronous task */
	void (*task)(fcom_task *task, fcom_task_func func, void *param);

	/** Set/disable timer.
	interval_msec:
	  >0: periodic
	  <0: one-shot
	  =0: disable */
	void (*timer)(fcom_timer *timer, int interval_msec, fcom_task_func func, void *param);

	/** Get current time
	dt: Optional
	flags: enum FCOM_CORE_CLOCK */
	fftime (*clock)(ffdatetime *dt, uint flags);

	/** Print log message */
	void (*log)(uint flags, const char *fmt, ...);
	void (*logv)(uint flags, const char *fmt, va_list args);

	/** Get random number */
	uint (*random)();

	uint debug :1;
	uint verbose :1;
	uint stdout_color :1;
	uint stdout_busy :1;
};

#define fcom_sysfatlog(fmt, ...)  (core)->log(FCOM_LOG_FATAL | FCOM_LOG_SYSERR, fmt, ##__VA_ARGS__)
#define fcom_fatlog(fmt, ...)  (core)->log(FCOM_LOG_FATAL, fmt, ##__VA_ARGS__)
#define fcom_syserrlog(fmt, ...)  (core)->log(FCOM_LOG_ERR | FCOM_LOG_SYSERR, fmt, ##__VA_ARGS__)
#define fcom_errlog(fmt, ...)  (core)->log(FCOM_LOG_ERR, fmt, ##__VA_ARGS__)
#define fcom_warnlog(fmt, ...)  (core)->log(FCOM_LOG_WARN, fmt, ##__VA_ARGS__)
#define fcom_syswarnlog(fmt, ...)  (core)->log(FCOM_LOG_WARN | FCOM_LOG_SYSERR, fmt, ##__VA_ARGS__)
#define fcom_infolog(fmt, ...)  (core)->log(FCOM_LOG_INFO, fmt, ##__VA_ARGS__)
#define fcom_verblog(fmt, ...) \
	do { if (core->verbose) (core)->log(FCOM_LOG_VERBOSE, fmt, ##__VA_ARGS__); } while (0)
#define fcom_dbglog(fmt, ...) \
	do { if (core->debug) (core)->log(FCOM_LOG_DBG, fmt, ##__VA_ARGS__); } while (0)


// COMMAND

/** Shared data for all active modules
All strings are transient (caller must allocate data and give it up). */
typedef struct fcom_cominfo {
	char **argv;
	uint argc;

	char *operation;

	// input file names
	ffvec input; // ffstr[], NULL-terminated
	ffvec include, exclude; // ffstr[]
	/** Read input file names from this fd */
	fffd input_fd;

	// input file
	byte stdin;
	byte recursive;
	fffd fd_stdin;

	ffstr output; // NULL-terminated
	char *outputz;
	ffstr chdir;
	byte stdout;
	byte overwrite;
	byte test;
	byte no_prealloc;
	fffd fd_stdout;

	uint buffer_size;
	byte directio;
	byte help;

	/** The function to call after the operation completes */
	void (*on_complete)(void *opaque, int result);
	void *opaque;
} fcom_cominfo;

enum FCOM_COM_PROVIDE {
	FCOM_COM_PROVIDE_PRIM = 1, // require `fcom_operation` interface
};

enum FCOM_COM_INPUT {
	/** Default behaviour: process directory then enter it:
	  (dir[0], dir[0][0], dir[1], ...) */

	/** Process complete current directory's contents BEFORE entering subdirectories:
	  (dir[0], dir[1], ..., dir[N], dir[0][0], ...) */
	FCOM_COM_INPUT_DIRFIRST = 1,
};

enum FCOM_COM_RINPUT {
	FCOM_COM_RINPUT_ERR = -2,
	FCOM_COM_RINPUT_NOMORE = -1, // no more input files
	FCOM_COM_RINPUT_OK,
};

enum FCOM_COM_IA {
	FCOM_COM_IA_FILE,
	FCOM_COM_IA_DIR,
	FCOM_COM_IA_AUTO, // detect from file name
};

enum FCOM_COM_AP {
	/** Use global command-line arguments for input/output. */
	FCOM_COM_AP_INOUT = 1,
};

/** This submodule executes new operations and manages all running operations. */
struct fcom_command {
	/** Find interface for primary/secondary operation in a module, loading it if necessary.
	flags: enum FCOM_COM_PROVIDE */
	const void* (*provide)(const char *operation, uint flags);

	/** Pass the signal to all active operations */
	void (*signal_all)(uint signal);

	/** Create operation context */
	fcom_cominfo* (*create)();

	/** Begin operation execution */
	int (*run)(fcom_cominfo *c);

	/** Operation is waiting for asynchronous signal */
	void (*async)(fcom_cominfo *c);

	/** Destroy object */
	void (*destroy)(fcom_cominfo *c);

	/** Operation signals about its completion */
	void (*complete)(fcom_cominfo *c, int code);

	/** Get next input file name.
	name: NULL-terminated file path
	base: [optional] user-specified base directory
	flags: enum FCOM_COM_INPUT
	Return enum FCOM_COM_RINPUT */
	int (*input_next)(fcom_cominfo *c, ffstr *name, ffstr *base, uint flags);

	/** File name returned by input_next() is a directory.
	This is required for recursive input file names scanning.
	dir: directory descriptor.  Don't use it afterwards! */
	void (*input_dir)(fcom_cominfo *c, fffd dir);

	/** Check if the name is included or/and not excluded.
	flags: enum FCOM_COM_IA
	Return 0 if allowed */
	int (*input_allowed)(fcom_cominfo *c, ffstr name, uint flags);

	/** Parse command-line arguments
	flags: enum FCOM_COM_AP */
	int (*args_parse)(fcom_cominfo *cmd, const struct ffarg *args, void *obj, uint flags);
};


// MODULE & OPERATION

/*
Operations:
  * Primary: the one that is directly responsible for executing command-line operation from user.
  * Secondary: provide on-demand support for the primary operations.
*/

typedef struct fcom_operation fcom_operation;

/** The module exports this interface as "fcom_module" */
struct fcom_module {
	const char *version;
	uint ver_core;
	void (*init)(const fcom_core *core);
	void (*destroy)();
	const fcom_operation* (*provide_op)(const char *name);
};

typedef void fcom_op;

/** Primary operation interface used by fcom_command */
struct fcom_operation {
	fcom_op* (*create)(fcom_cominfo *cmd);
	void (*close)(fcom_op *op);
	void (*run)(fcom_op *op);
	void (*signal)(fcom_op *op, uint signal);
	const char* (*help)();
};

#define FCOM_MOD_DEFINE1(modname, provide_op_func, gcore) \
	static void modname##_init(const fcom_core *_core) { gcore = _core; } \
	static void modname##_destroy() {} \
	FCOM_EXPORT const struct fcom_module fcom_module = { \
		FCOM_VER, FCOM_CORE_VER, \
		modname##_init, modname##_destroy, modname##_provide_op, \
	};

#define FCOM_MOD_DEFINE(modname, iface_obj, gcore) \
	static void modname##_init(const fcom_core *_core) { gcore = _core; } \
	static void modname##_destroy() {} \
	static const fcom_operation* modname##_provide_op(const char *name) \
	{ \
		if (ffsz_eq(name, #modname)) \
			return &iface_obj; \
		return NULL; \
	} \
	FCOM_EXPORT const struct fcom_module fcom_module = { \
		FCOM_VER, FCOM_CORE_VER, \
		modname##_init, modname##_destroy, modname##_provide_op, \
	};


// FILE

struct fcom_file_conf {
	uint buffer_size;
	uint n_buffers;

	/** FDs used for FCOM_FILE_STDxx.
	By default stdin/stdout are used. */
	fffd fd_stdin, fd_stdout;
};

/** Fill default config for a file descriptor */
static inline void fcom_cmd_file_conf(struct fcom_file_conf *fc, const fcom_cominfo *cmd)
{
	fc->buffer_size = cmd->buffer_size;
	fc->fd_stdin = cmd->fd_stdin;
	fc->fd_stdout = cmd->fd_stdout;
}

enum FCOM_FILE_OPEN {
	FCOM_FILE_READ,
	FCOM_FILE_WRITE,
	FCOM_FILE_READWRITE,
	FCOM_FILE_CREATENEW = 4,
	FCOM_FILE_CREATE = 8,
	FCOM_FILE_STDIN = 0x10,
	FCOM_FILE_STDOUT = 0x20,
	FCOM_FILE_DIRECTIO = 0x40,
	FCOM_FILE_FAKEWRITE = 0x80,
	FCOM_FILE_NO_PREALLOC = 0x0100,
	FCOM_FILE_INFO_NOFOLLOW = 0x0200, // info(): don't follow symlinks
	FCOM_FILE_READAHEAD = 0x0400, // Windows: enable "read-ahead"
	FCOM_FILE_NOCACHE = 0x0800,
};

static inline uint fcom_file_cominfo_flags_i(fcom_cominfo *cmd)
{
	uint f = 0;
	if (cmd->stdin)
		f |= FCOM_FILE_STDIN;
	if (cmd->directio)
		f |= FCOM_FILE_DIRECTIO;
	return f;
}

static inline uint fcom_file_cominfo_flags_o(fcom_cominfo *cmd)
{
	uint f = 0;
	if (cmd->overwrite)
		f |= FCOM_FILE_CREATE;
	else
		f |= FCOM_FILE_CREATENEW;

	if (cmd->stdout)
		f |= FCOM_FILE_STDOUT;
	if (cmd->test)
		f |= FCOM_FILE_FAKEWRITE;
	if (cmd->directio)
		f |= FCOM_FILE_DIRECTIO;
	return f;
}

enum FCOM_FILE_RET {
	FCOM_FILE_OK,
	FCOM_FILE_EOF,
	FCOM_FILE_ASYNC,
	FCOM_FILE_ERR,
};

enum FCOM_FILE_BEH {
	FCOM_FBEH_SEQ, // Sequential access behaviour
	FCOM_FBEH_RANDOM, // Random access behaviour
	FCOM_FBEH_TRUNC_PREALLOC, // Truncate the currently preallocated space
};

enum FCOM_FILE_FD {
	FCOM_FILE_GET,
	FCOM_FILE_ACQUIRE, // Acquire descriptor.  Only open() can be called afterwards.
};

enum FCOM_FILE_MOVE_F {
	/** Fail if target file already exists */
	FCOM_FILE_MOVE_SAFE = 1,
};

enum FCOM_FILE_DIR_F {
	/** Recursively create parent directories if necessary */
	FCOM_FILE_DIR_RECURSIVE = 1,
};

typedef void fcom_file_obj;

/** Bufferred file I/O interface.
This interface allows:
* Completely asynchronous file operations
* User-space data caching on reading and writing
* Controlling kernel caching behaviour */
struct fcom_file {
	fcom_file_obj* (*create)(struct fcom_file_conf *conf);
	void (*destroy)(fcom_file_obj *f);

	/**
	flags: enum FCOM_FILE_OPEN
	Return enum FCOM_FILE_RET */
	int (*open)(fcom_file_obj *f, const char *name, uint flags);

	void (*close)(fcom_file_obj *f);

	int (*read)(fcom_file_obj *f, ffstr *d, int64 off);

	/**
	off: -1: use the current offset */
	int (*write)(fcom_file_obj *f, ffstr d, int64 off);
	int (*write_fmt)(fcom_file_obj *_f, const char *fmt, ...);

	int (*flush)(fcom_file_obj *f, uint flags);

	int (*trunc)(fcom_file_obj *f, int64 size);

	/**
	flags: enum FCOM_FILE_BEH */
	int (*behaviour)(fcom_file_obj *f, uint flags);

	int (*info)(fcom_file_obj *f, fffileinfo *fi);

	/**
	flags: enum FCOM_FILE_FD */
	fffd (*fd)(fcom_file_obj *f, uint flags);

	/**
	mtime: Time since year 1. */
	int (*mtime_set)(fcom_file_obj *f, fftime mtime);

	int (*attr_set)(fcom_file_obj *f, uint attr);

	/** Create directory.
	By default, don't fail if the directory already exists.
	flags: enum FCOM_FILE_DIR_F */
	int (*dir_create)(const char *name, uint flags);

	/** Create hard link */
	int (*hlink)(const char *oldpath, const char *newpath, uint flags);

	/** Create symbolic link */
	int (*slink)(const char *target, const char *linkpath, uint flags);

	/** Move file
	flags: enum FCOM_FILE_MOVE_F */
	int (*move)(ffstr old, ffstr _new, uint flags);

	/** Delete file */
	int (*del)(const char *name, uint flags);
};

static inline fftime fffileinfo_mtime1(const fffileinfo *fi)
{
	fftime t = fffileinfo_mtime(fi);
	t.sec += FFTIME_1970_SECONDS;
	return t;
}


// SYNC

enum FCOM_SYNC_DIFF {
	FCOM_SYNC_DIFF_LEFT_PATH_STRIP = 1,
	FCOM_SYNC_DIFF_RIGHT_PATH_STRIP = 2,
	FCOM_SYNC_DIFF_NO_ATTR = 8,
	FCOM_SYNC_DIFF_NO_TIME = 0x10,
	FCOM_SYNC_DIFF_TIME_2SEC = 0x20,
	FCOM_SYNC_DIFF_MOVE_NO_NAME = 0x40,
};

enum FCOM_SYNC {
	FCOM_SYNC_LEFT = 1,
	FCOM_SYNC_RIGHT = 2,
	FCOM_SYNC_NEQ = 4,
	FCOM_SYNC_MOVE = 8,
	FCOM_SYNC_EQ = 0x10,
	FCOM_SYNC_MASK = 0xff,

	FCOM_SYNC_NEWER = 0x0100,
	FCOM_SYNC_OLDER = 0x0200,

	FCOM_SYNC_LARGER = 0x0400,
	FCOM_SYNC_SMALLER = 0x0800,

	FCOM_SYNC_ATTR = 0x1000,

	FCOM_SYNC_DIR = 0x2000,
	FCOM_SYNC_SWAP = 0x4000,
	_FCOM_SYNC_SKIP = 0x8000, // moved-double

	// user:
	FCOM_SYNC_SYNCING = 0x010000,
	FCOM_SYNC_ERROR = 0x020000,
	FCOM_SYNC_DONE = 0x040000,
};

enum FCOM_SYNC_SYNC {
	FCOM_SYNC_REPLACE_DATE = 1,
	FCOM_SYNC_VERIFY = 2,
	// FCOM_SYNC_SWAP
};

enum FCOM_SYNC_SORT {
	FCOM_SYNC_SORT_FILESIZE = 1,
	FCOM_SYNC_SORT_MTIME = 2,
};

struct fcom_sync_diff_stats {
	uint eq, left, right, neq, moved;
	uint ltotal, rtotal, entries;
};

struct fcom_sync_props {
	ffslice include, exclude; // ffstr[]
	fftime since_time;

	struct fcom_sync_diff_stats stats;
};

struct fcom_sync_entry {
	uint64	size;
	uint	unix_attr, win_attr;
	uint	uid, gid;
	fftime	mtime; // UTC
	uint	crc32;
};

struct fcom_sync_diff_entry {
	uint status; // enum FCOM_SYNC
	ffstr lname, rname; // NULL-terminated
	struct fcom_sync_entry *left, *right;
	void *id;
};

static inline void fcom_sync_diff_entry_destroy(struct fcom_sync_diff_entry *de) {
	ffstr_free(&de->lname);
	ffstr_free(&de->rname);
}

typedef struct snapshot fcom_sync_snapshot;
typedef struct diff fcom_sync_diff;
typedef struct fcom_sync_if fcom_sync_if;
struct fcom_sync_if {
	/** Read shapshot from file. */
	fcom_sync_snapshot* (*open)(const char *snapshot_path, uint flags);

	/** Create snapshot from directory. */
	fcom_sync_snapshot* (*scan)(ffstr path, uint flags);

	/** Create snapshot from the directories found by wildcard. */
	fcom_sync_snapshot* (*scan_wc)(ffstr wc, uint flags);

	void (*snapshot_free)(fcom_sync_snapshot *ss);

	/** Compare two snapshots.
	flags: enum FCOM_SYNC_DIFF */
	fcom_sync_diff* (*diff)(fcom_sync_snapshot *left, fcom_sync_snapshot *right, struct fcom_sync_props *props, uint flags);

	fcom_sync_diff* (*find_dups)(fcom_sync_snapshot *left, struct fcom_sync_props *props, uint flags);

	void (*diff_free)(fcom_sync_diff *sd);

	/** Filter diff entries.
	flags: enum FCOM_SYNC */
	uint (*view)(fcom_sync_diff *sd, struct fcom_sync_props *props, uint flags);

	/**
	flags: enum FCOM_SYNC_SORT */
	uint (*sort)(fcom_sync_diff *sd, uint flags);

	/** Get diff entry.
	flags: FCOM_SYNC_SWAP */
	int (*info)(fcom_sync_diff *sd, uint i, uint flags, struct fcom_sync_diff_entry *dst);
	int (*info_id)(fcom_sync_diff *sd, void *id, uint flags, struct fcom_sync_diff_entry *dst);

	/** Update diff entry's status.
	Return the resulting status. */
	uint (*status)(fcom_sync_diff *sd, void *id, uint mask, uint val);

	/** Synchronize files.
	flags: enum FCOM_SYNC_SYNC
	Return
		0: success
		1: async; on_complete() will be called
		-1: error */
	int (*sync)(fcom_sync_diff *sd, void *diff_entry_id, uint flags, void(*on_complete)(void*, int), void *param);
};


// UNPACK

typedef void fcom_unpack_obj;
typedef struct fcom_unpack_if fcom_unpack_if;
struct fcom_unpack_if {
	fcom_unpack_obj* (*open_file)(fffd f);
	void (*close)(fcom_unpack_obj *o);
	ffstr (*next)(fcom_unpack_obj *o, void *unused);
};


// HASH

typedef void fcom_hash_obj;
typedef struct fcom_hash {
	fcom_hash_obj* (*create)();
	void (*update)(fcom_hash_obj *obj, const void *data, ffsize size);
	void (*fin)(fcom_hash_obj *obj, byte *result, ffsize result_cap);
	void (*close)(fcom_hash_obj *obj);
} fcom_hash;


// AES

enum FCOM_AES_MODE {
	FCOM_AES_CBC,
	FCOM_AES_CFB,
	FCOM_AES_OFB,
};

typedef void fcom_aes_obj;
typedef struct fcom_aes {
	/**
	flags: enum FCOM_AES_MODE */
	fcom_aes_obj* (*create)(const byte *key, ffsize key_len, uint flags);
	int (*process)(fcom_aes_obj *obj, const void *in, void *out, ffsize len, byte *iv);
	void (*close)(fcom_aes_obj *obj);
} fcom_aes;
