/** fcom GUI.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <util/gui-winapi/loader.h>
#include <util/gui-winapi/winapi.h>
#include <FFOS/thread.h>


const fcom_core *core;
const fcom_command *com;

// MODULE
static int gui_sig(uint signo);
static const void* gui_iface(const char *name);
static const fcom_mod gui_mod = {
	.sig = &gui_sig, .iface = &gui_iface,
	.ver = FCOM_VER,
	.name = "GUI", .desc = "GUI",
};

// GUI
static void* gui_open(fcom_cmd *cmd);
static void gui_close(void *p, fcom_cmd *cmd);
static int gui_process(void *p, fcom_cmd *cmd);
static const fcom_filter gui_filt = { &gui_open, &gui_close, &gui_process };
static void gui_destroy(void);
static FFTHDCALL int gui_run_thd(void *param);
static void gui_exit(void *param);


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &gui_mod;
}

struct cmd {
	const char *name;
	const char *mod;
	const fcom_filter *iface;
};

/** This function is called after GUI initializes. */
typedef int (*gui_func)(void);
extern int scrshots_create(void);
extern int gsync_create(void);
extern const fcom_filter dcbmp_filt;
static const struct cmd cmds[] = {
	{ "gui", NULL, &gui_filt },
	{ "screenshots", "gui.gui", (void*)&scrshots_create },
	{ "gsync", "gui.gui", (void*)&gsync_create },
	{ "dcbmp", NULL, &dcbmp_filt },
};

static int gui_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		com = core->iface("core.com");
		const struct cmd *c;
		FFARRS_FOREACH(cmds, c) {
			if (c->mod != NULL && 0 != com->reg(c->name, c->mod))
				return -1;
		}
		break;
	}
	case FCOM_SIGFREE:
		gui_destroy();
		break;
	}
	return 0;
}

static const void* gui_iface(const char *name)
{
	const struct cmd *c;
	FFARRS_FOREACH(cmds, c) {
		if (ffsz_eq(name, c->name))
			return c->iface;
	}
	return NULL;
}


/** Global GUI context. */
struct ggui {
	fcom_cmd *cmd;
	fftask tsk;
	ffthd th;
	gui_func wnd_creator;
};
static struct ggui *gg;

static void* gui_open(fcom_cmd *cmd)
{
	gg = ffmem_new(struct ggui);
	gg->cmd = cmd;
	return gg;
}
static void gui_close(void *p, fcom_cmd *cmd)
{
}
static int gui_process(void *p, fcom_cmd *cmd)
{
	if (gg->th != FFTHREAD_NULL)
		return FCOM_DONE;

#ifndef _DEBUG
	if (!fcom_logchk(core->conf->loglev, FCOM_LOGDBG))
		ffterm_detach();
#endif

	const struct cmd *c;
	FFARRS_FOREACH(cmds, c) {
		if (ffsz_eq(cmd->name, c->name)) {
			gui_func f = (void*)c->iface;
			gg->wnd_creator = f;
			break;
		}
	}
	FF_ASSERT(gg->wnd_creator != NULL);

	if (FFTHD_INV == (gg->th = ffthd_create(&gui_run_thd, NULL, 0))) {
		fcom_syserrlog("gui", "%s", ffthd_create_S);
		return FCOM_ERR;
	}

	return FCOM_ASYNC;
}


static void gui_destroy(void)
{
	if (gg == NULL)
		return;
	core->task(FCOM_TASK_DEL, &gg->tsk);
	ffmem_safefree0(gg);
}

/** GUI thread. */
static FFTHDCALL int gui_run_thd(void *param)
{
	fcom_dbglog(0, "gui", "ui thread enter", 0);
	ffui_init();
	ffui_wnd_initstyle();

	if (0 != gg->wnd_creator())
		goto done;

	ffui_run();

done:
	ffui_uninit();

	gg->tsk.handler = &gui_exit;
	core->task(FCOM_TASK_ADD, &gg->tsk);
	fcom_dbglog(0, "gui", "ui thread leave", 0);
	return 0;
}

/** GUI thread has exitted. */
static void gui_exit(void *param)
{
	com->ctrl(gg->cmd, FCOM_CMD_RUNASYNC);
}
