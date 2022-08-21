/** fcom: public interface
2022, Simon Zolin */

#pragma once
#include <FFOS/error.h>
#include <util/taskqueue.h>
#include <util/cmdarg-scheme.h>
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
