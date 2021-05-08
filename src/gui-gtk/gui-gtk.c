/** fcom: GUI (GTK+)
2021, Simon Zolin
*/

#include <fcom.h>

const fcom_core *core;
const fcom_command *com;

void* gui_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

void gui_close(void *p, fcom_cmd *cmd)
{}

int gui_process(void *p, fcom_cmd *cmd)
{
	return FCOM_DONE;
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

int gui_conf(const char *name, ffpars_ctx *ctx)
{
	return 0;
}

const fcom_mod gui_mod = {
	.sig = gui_sig, .iface = gui_iface, .conf = gui_conf,
	.ver = FCOM_VER,
	.name = "GUI", .desc = "GUI",
};

FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &gui_mod;
}
