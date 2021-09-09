/** fcom: create files
2021, Simon Zolin
*/

#define FILT_NAME  "touch"

static void* f_touch_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void f_touch_close(void *p, fcom_cmd *cmd)
{
}

static int f_touch_process(void *p, fcom_cmd *cmd)
{
	fftime mtime;
	const char *fn;

	if (fftime_sec(&cmd->mtime) != 0 && cmd->date_as_fn != NULL)
		return FCOM_ERR;

	if (cmd->date_as_fn != NULL) {
		fffileinfo fi;
		if (0 != fffile_infofn(cmd->date_as_fn, &fi)) {
			syserrlog("%s", cmd->date_as_fn);
			return FCOM_ERR;
		}
		mtime = fffile_infomtime(&fi);

	} else if (fftime_sec(&cmd->mtime) != 0)
		mtime = cmd->mtime;
	else
		fftime_now(&mtime);

	for (;;) {
		if (NULL == (fn = com->arg_next(cmd, 0)))
			return FCOM_DONE;

		if (0 == fffile_settimefn(fn, &mtime)) {
			verblog("%s", fn);
			continue;
		}

		if (!fferr_nofile(fferr_last())) {
			syserrlog("%s: %s", fn, fffile_info_S);
			return FCOM_ERR;
		}

		// create a new file
		fffd fd;
		while (FF_BADFD == (fd = fffile_open(fn, FFO_CREATENEW | FFO_WRONLY | FFO_NOATIME))) {
			if (fferr_nofile(fferr_last())) {
				if (0 != ffdir_make_path((void*)fn, 0)) {
					syserrlog("%s: for filename %s", ffdir_make_S, fn);
					return FCOM_ERR;
				}
			}
		}
		if (0 != fffile_close(fd))
			syserrlog("%s: %s", fn, fffile_close_S);

		if (0 == fffile_settimefn(fn, &mtime)) {
			verblog("%s", fn);
			continue;
		}
		syserrlog("%s: %s", fn, fffile_settime_S);
		return FCOM_ERR;
	}

	return FCOM_ERR;
}
#undef FILT_NAME

static const fcom_filter f_touch_filt = { f_touch_open, f_touch_close, f_touch_process };
