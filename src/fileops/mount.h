/** fcom: mount disk to directory
2021, Simon Zolin
*/

#define FILT_NAME  "mount"

static void* mount_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void mount_close(void *p, fcom_cmd *cmd)
{
}

static int mount_process(void *p, fcom_cmd *cmd)
{
#ifdef FF_WIN
	ffbool ok = 0;
	const char *disk = com->arg_next(cmd, 0);
	const char *mount = cmd->output.fn;
	if (disk == NULL || mount == NULL)
		return FCOM_ERR;

	if (disk[0] == '\0') {
		if (0 != fffile_mount(NULL, mount)) {
			syserrlog("fffile_mount", 0);
			goto end;
		}
		verblog("removed mount point %s", mount);

	} else {
		if (0 != fffile_mount(disk, mount)) {
			syserrlog("fffile_mount", 0);
			goto end;
		}
		verblog("%s -> %s", disk, mount);
	}
	ok = 1;

end:
	return (ok) ? FCOM_DONE : FCOM_ERR;
#endif
	return FCOM_DONE;
}

#undef FILT_NAME

static const fcom_filter mount_filt = { &mount_open, &mount_close, &mount_process };
