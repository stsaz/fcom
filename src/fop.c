/** File operations.
Copyright (c) 2017 Simon Zolin
*/

#include <fcom.h>

#include <FFOS/file.h>


extern const fcom_core *core;
static const fcom_command *com;

// MODULE
static int f_sig(uint signo);
static const void* f_iface(const char *name);
static int f_conf(const char *name, ffpars_ctx *ctx);
const fcom_mod f_mod = {
	.sig = &f_sig, .iface = &f_iface, .conf = &f_conf,
};

// TOUCH
static void* f_touch_open(fcom_cmd *cmd);
static void f_touch_close(void *p, fcom_cmd *cmd);
static int f_touch_process(void *p, fcom_cmd *cmd);
static const fcom_filter f_touch_filt = {
	&f_touch_open, &f_touch_close, &f_touch_process,
};


static const void* f_iface(const char *name)
{
	if (ffsz_eq(name, "f-touch"))
		return &f_touch_filt;
	return NULL;
}

static int f_conf(const char *name, ffpars_ctx *ctx)
{
	return 1;
}

static int f_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT:
		com = core->iface("core.com");
		if (0 != com->reg("touch", "core.f-touch"))
			return 1;
		break;
	case FCOM_SIGSTART:
		break;
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}


static void* f_touch_open(fcom_cmd *cmd)
{
	return (void*)1;
}

static void f_touch_close(void *p, fcom_cmd *cmd)
{
}

static int f_touch_process(void *p, fcom_cmd *cmd)
{
	if (NULL == (cmd->output.fn = com->arg_next(cmd, 0)))
		return FCOM_DONE;

	if (0 != com->fcom_cmd_filtadd(cmd, "core.file-out"))
		return FCOM_ERR;

	if (cmd->mtime.s != 0 && cmd->date_as_fn != NULL)
		return FCOM_ERR;

	if (cmd->date_as_fn != NULL) {
		fffileinfo fi;
		if (0 != fffile_infofn(cmd->date_as_fn, &fi))
			return FCOM_SYSERR;
		cmd->output.mtime = fffile_infomtime(&fi);

	} else if (cmd->mtime.s != 0)
		cmd->output.mtime = cmd->mtime;
	else
		fftime_now(&cmd->output.mtime);

	// open or create, but don't modify file data
	cmd->out_overwrite = 1;
	cmd->out_notrunc = 1;
	cmd->output.size = 0;
	return FCOM_NEXTDONE;
}
