/** fcom: public interface
2022, Simon Zolin */

#pragma once
#include <FFOS/error.h>
#include <util/taskqueue.h>
#include <util/cmdarg-scheme.h>
#include <FFOS/file.h>
#include <FFOS/timerqueue.h>
#include <FFOS/time.h>

#define FCOM_VER "1.0beta1"

typedef ffbyte byte;
typedef ffuint uint;
typedef ffuint64 uint64;

// CORE

enum FCOM_LOG {
	FCOM_LOG_FATAL,
	FCOM_LOG_SYSERR,
	FCOM_LOG_ERR,
	FCOM_LOG_WARN,
	FCOM_LOG_INFO,
	FCOM_LOG_VERBOSE,
	FCOM_LOG_DBG,
};

/**
flags: enum FCOM_LOG */
typedef void (*fcom_log_func)(uint flags, const char *fmt, va_list args);

typedef struct fcom_core fcom_core;

struct fcom_core_conf {
	uint timer_resolution_msec;
	char *app_path;
	fcom_log_func log;
	uint debug :1;
	uint verbose :1;
};

/** Core initializer.
core.so exports this interface as "fcom_coreinit" */
struct fcom_coreinit {
	void (*init)();
	void (*destroy)();
	fcom_core* (*conf)(struct fcom_core_conf *conf);
	int (*run)();
};

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
	/** File submodule interface */
	const fcom_file *file;

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
	flags: enum FCOM_CORE_CLOCK */
	fftime (*clock)(ffdatetime *dt, uint flags);

	/** Print log message */
	void (*log)(uint flags, const char *fmt, ...);
	void (*logv)(uint flags, const char *fmt, va_list args);

	uint debug :1;
	uint verbose :1;
};

#define fcom_syserrlog(fmt, ...)  (core)->log(FCOM_LOG_SYSERR, fmt, ##__VA_ARGS__)
#define fcom_errlog(fmt, ...)  (core)->log(FCOM_LOG_ERR, fmt, ##__VA_ARGS__)
#define fcom_infolog(fmt, ...)  (core)->log(FCOM_LOG_INFO, fmt, ##__VA_ARGS__)
#define fcom_verblog(fmt, ...) \
	do { if (core->verbose) (core)->log(FCOM_LOG_VERBOSE, fmt, ##__VA_ARGS__); } while (0)
#define fcom_dbglog(fmt, ...) \
	do { if (core->debug) (core)->log(FCOM_LOG_DBG, fmt, ##__VA_ARGS__); } while (0)


// FILE

struct fcom_file_conf {
	uint buffer_size;
	uint n_buffers;
};

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
	FCOM_FBEH_RANDOM, // Rando access behaviour
	FCOM_FBEH_TRUNC_PREALLOC, // Truncate the currently preallocated space
};

enum FCOM_FILE_FD {
	FCOM_FILE_GET,
	FCOM_FILE_ACQUIRE, // Acquire descriptor.  Only open() can be called afterwards.
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
	name_ptr: static name pointer
	flags: enum FCOM_FILE_OPEN
	Return enum FCOM_FILE_RET */
	int (*open)(fcom_file_obj *f, const char *static_name_ptr, uint flags);
	void (*close)(fcom_file_obj *f);
	int (*read)(fcom_file_obj *f, ffstr *d, int64 off);
	int (*write)(fcom_file_obj *f, ffstr d, int64 off);
	/**
	flags: enum FCOM_FILE_BEH */
	int (*behaviour)(fcom_file_obj *f, uint flags);
	int (*info)(fcom_file_obj *f, fffileinfo *fi);

	/**
	flags: enum FCOM_FILE_FD */
	fffd (*fd)(fcom_file_obj *f, uint flags);

	int (*mtime)(fcom_file_obj *f, fftime *mtime);
	int (*mtime_set)(fcom_file_obj *f, fftime *mtime);

	/** Create directory.
	By default, don't fail if the directory already exists. */
	int (*dir_create)(const char *name, uint flags);
};
