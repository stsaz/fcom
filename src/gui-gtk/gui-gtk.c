/** fcom: GUI (GTK+)
2021, Simon Zolin
*/

#include <fcom.h>
#include <util/gui-gtk/gtk.h>
#include <util/gui-gtk/loader.h>
#include <FFOS/thread.h>

const fcom_core *core;
const fcom_command *com;

void* gui_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

void gui_close(void *p, fcom_cmd *cmd)
{
	ffui_uninit();
}

void wsync_init();
void wsync_destroy();

fftask tsk;

/** GUI thread has exitted. */
static void gui_exit(void *param)
{
	core->cmd(FCOM_STOP, 0);
}

int gui_loop(void *param)
{
	wsync_init();
	ffui_run();
	wsync_destroy();
	tsk.handler = gui_exit;
	core->task(FCOM_TASK_ADD, &tsk);
	return 0;
}

int gui_process(void *p, fcom_cmd *cmd)
{
	ffui_init();
	if (ffsz_eq(cmd->name, "gsync"))
	{}
	else
		return FCOM_ERR;

	ffthread th = ffthread_create(gui_loop, NULL, 0);
	ffthread_detach(th);
	return FCOM_ASYNC;
}

const fcom_filter gui_filter = { gui_open, gui_close, gui_process };

int gui_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT:
		com = core->iface("core.com");
		if (0 != com->reg("gsync", "gui.gui"))
			return -1;
		break;

	case FCOM_SIGFREE:
		break;
	}
	return 0;
}

const void* gui_iface(const char *name)
{
	if (ffsz_eq(name, "gui"))
		return &gui_filter;
	else if (ffsz_eq(name, "dcbmp"))
		return (void*)1;
	return NULL;
}

const fcom_mod gui_mod = {
	.sig = gui_sig, .iface = gui_iface,
	.ver = FCOM_VER,
	.name = "GUI", .desc = "GUI",
};

FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &gui_mod;
}
