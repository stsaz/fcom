/** fcom: rename files
2021, Simon Zolin
*/

#define FILT_NAME  "f-rename"
static void* f_rename_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

static void f_rename_close(void *p, fcom_cmd *cmd)
{
}

/** For each input filename replace text within. */
static int f_rename_process(void *p, fcom_cmd *cmd)
{
	if (cmd->search.len == 0) {
		errlog("Use --replace argument to specify search and replace text", 0);
		return FCOM_ERR;
	}

	int r;
	ffvec newfn = {};
	const char *fn;
	ffstr sfn;

	for (;;) {
		if (NULL == (fn = com->arg_next(cmd, 0))) {
			r = FCOM_DONE;
			goto done;
		}

		ffstr_setz(&sfn, fn);
		newfn.len = 0;
		if (0 == ffstr_growadd_replace((ffstr*)&newfn, &newfn.cap, &sfn, &cmd->search, &cmd->replace, FFSTR_REPLACE_ICASE | FFSTR_REPLACE_ALL))
			continue;

		if (0 == ffvec_addchar(&newfn, '\0'))
			return FCOM_SYSERR;

		if (!cmd->read_only
			&& 0 != fffile_rename(fn, newfn.ptr)) {
			syserrlog("%s -> %s", fn, newfn.ptr);
			if (cmd->skip_err)
				continue;
			r = FCOM_ERR;
			goto done;
		}
		verblog("%s -> %s", fn, newfn.ptr);
	}

done:
	ffvec_free(&newfn);
	return r;
}
#undef FILT_NAME

static const fcom_filter f_rename_filt = { f_rename_open, f_rename_close, f_rename_process };
