/** fcom: file list
2021, Simon Zolin
*/

#include <FFOS/dirscan.h>

void* flist_open(fcom_cmd *cmd)
{
	return FCOM_OPEN_DUMMY;
}

void flist_close(void *p, fcom_cmd *cmd)
{
}

int flist_process(void *p, fcom_cmd *cmd)
{
	ffvec d = {};
	const char *fn;
	for (;;) {
		if (NULL == (fn = com->arg_next(cmd, 0)))
			break;
		ffvec_addfmt(&d, "%s\n", fn);

		if (!cmd->recurse) {
			ffdirscan dir = {};
			if (0 != ffdirscan_open(&dir, fn, 0))
				continue;
			const char *name;
			for (;;) {
				if (NULL == (name = ffdirscan_next(&dir)))
					break;
				ffvec_addfmt(&d, "%s/%s\n", fn, name);
			}
			ffdirscan_close(&dir);
		}
	}

	ffstdout_write(d.ptr, d.len);
	ffvec_free(&d);
	return FCOM_DONE;
}

const fcom_filter flist_filt = { flist_open, flist_close, flist_process };
