/** fcom: Mount logical volumes (Windows)
2023, Simon Zolin */

#include <fcom.h>
#include <ffsys/std.h>
#include <ffsys/volume.h>

static const fcom_core *core;

struct mount {
	fcom_cominfo *cmd;
	uint stop;
};

static const char* mount_help()
{
	return "\
Mount logical volumes.\n\
Usage:\n\
  fcom mount DISK -o PATH (Mount)\n\
  fcom mount \"\" -o PATH (Unmount)\n\
";
}

static int args_parse(struct mount *m, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{}
	};
	int r = core->com->args_parse(cmd, args, m);
	if (r != 0)
		return r;

	if (cmd->input.len == 0) {
		fcom_fatlog("Please specify volume UID");
		return 1;
	}

	if (cmd->output.len == 0) {
		fcom_fatlog("Please use -o to set mount path");
		return 1;
	}

	return 0;
}

static void mount_close(fcom_op *op)
{
	struct mount *m = op;
	ffmem_free(m);
}

static fcom_op* mount_create(fcom_cominfo *cmd)
{
	struct mount *m = ffmem_new(struct mount);
	m->cmd = cmd;

	if (0 != args_parse(m, cmd))
		goto end;

	return m;

end:
	mount_close(m);
	return NULL;
}

static void mount_run(fcom_op *op)
{
	struct mount *m = op;
	int rc = 1;

	ffstr disk = *ffslice_itemT(&m->cmd->input, 0, ffstr);
	const char *mpath = m->cmd->output.ptr;

	if (disk.ptr[0] == '\0') {
		if (0 != ffvol_mount(NULL, mpath)) {
			fcom_syserrlog("ffvol_mount", 0);
			goto end;
		}
		fcom_verblog("removed mount point %s", mpath);

	} else {
		if (0 != ffvol_mount(disk.ptr, mpath)) {
			fcom_syserrlog("ffvol_mount", 0);
			goto end;
		}
		fcom_verblog("%s -> %s", disk.ptr, mpath);
	}

	rc = 0;
	goto end;

end:
	{
	fcom_cominfo *cmd = m->cmd;
	mount_close(m);
	core->com->complete(cmd, rc);
	}
}

static void mount_signal(fcom_op *op, uint signal)
{
	struct mount *m = op;
	FFINT_WRITEONCE(m->stop, 1);
}

static const fcom_operation fcom_op_mount = {
	mount_create, mount_close,
	mount_run, mount_signal,
	mount_help,
};


static void mount_init(const fcom_core *_core) { core = _core; }
static void mount_destroy() {}
static const fcom_operation* mount_provide_op(const char *name)
{
	if (ffsz_eq(name, "mount"))
		return &fcom_op_mount;
	return NULL;
}
FF_EXP const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	mount_init, mount_destroy, mount_provide_op,
};
