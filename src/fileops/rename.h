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

/**
"dir/file" -> "dir - file"
"./dir/file" -> "./dir - file"
*/
static int unbranch_filename(ffvec *dst, ffstr in)
{
	if (ffstr_matchz(&in, "./")) {
		ffvec_addsz(dst, "./");
		ffstr_shift(&in, 2);
	}

	ffstr sch = FFSTR_INITZ("/"), repl = FFSTR_INITZ(" - ");
	return ffstr_growadd_replace((ffstr*)dst, &dst->cap, &in, &sch, &repl, FFSTR_REPLACE_ALL);
}

#if 0
#include <FFOS/test.h>
static void test_unbranch_filename()
{
	ffvec v = {};
	ffstr s;

	ffstr_setz(&s, "file");
	unbranch_filename(&v, s);
	x(ffstr_eqz(&v, "file"));
	v.len = 0;

	ffstr_setz(&s, "./file");
	unbranch_filename(&v, s);
	x(ffstr_eqz(&v, "./file"));
	v.len = 0;

	ffstr_setz(&s, "dir/file");
	unbranch_filename(&v, s);
	x(ffstr_eqz(&v, "dir - file"));
	v.len = 0;

	ffstr_setz(&s, "dir/d/file");
	unbranch_filename(&v, s);
	x(ffstr_eqz(&v, "dir - d - file"));
	v.len = 0;

	ffstr_setz(&s, "./dir/d/file");
	unbranch_filename(&v, s);
	x(ffstr_eqz(&v, "./dir - d - file"));
	v.len = 0;

	ffvec_free(&v);
}
#endif

/** For each input filename replace text within. */
static int f_rename_process(void *p, fcom_cmd *cmd)
{
	// test_unbranch_filename();

	if (!(cmd->search.len != 0 || cmd->unbranch)) {
		errlog("Use --replace or --unbranch argument", 0);
		return FCOM_ERR;
	}

	int r, arg_next_flags = 0;
	ffvec newfn = {}, tmpfn = {};
	const char *fn;
	ffstr sfn;

	if (cmd->unbranch)
		arg_next_flags = FCOM_CMD_ARG_FILE;

	for (;;) {
		if (NULL == (fn = com->arg_next(cmd, arg_next_flags))) {
			r = FCOM_DONE;
			goto done;
		}
		ffstr_setz(&sfn, fn);
		newfn.len = 0;

		if (cmd->unbranch) {
			if (cmd->search.len != 0) {
				tmpfn.len = 0;
				unbranch_filename(&tmpfn, sfn);
				ffstr_growadd_replace((ffstr*)&newfn, &newfn.cap, (ffstr*)&tmpfn, &cmd->search, &cmd->replace, FFSTR_REPLACE_ICASE | FFSTR_REPLACE_ALL);
			} else {
				unbranch_filename(&newfn, sfn);
			}

		} else if (cmd->search.len != 0) {
			if (0 == ffstr_growadd_replace((ffstr*)&newfn, &newfn.cap, &sfn, &cmd->search, &cmd->replace, FFSTR_REPLACE_ICASE | FFSTR_REPLACE_ALL))
				continue;
		}

		if (0 == ffvec_addchar(&newfn, '\0')) {
			r = FCOM_SYSERR;
			goto done;
		}
		newfn.len--;

		if (ffstr_eq2(&newfn, &sfn))
			continue;

		if (!cmd->read_only
			&& 0 != fffile_rename(fn, newfn.ptr)) {

			syserrlog("%s -> %s", fn, newfn.ptr);
			if (cmd->skip_err)
				continue;
			r = FCOM_ERR;
			goto done;
		}
		verblog("\"%s\" -> \"%s\"", fn, newfn.ptr);
	}

done:
	ffvec_free(&newfn);
	ffvec_free(&tmpfn);
	return r;
}
#undef FILT_NAME

static const fcom_filter f_rename_filt = { f_rename_open, f_rename_close, f_rename_process };
