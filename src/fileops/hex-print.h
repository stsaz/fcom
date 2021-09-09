/** fcom: hex data printing
2020, Simon Zolin
*/

#include <ffbase/mem-print.h>

#define FILT_NAME  "hexprint"

struct hexprint {
	ffarr fdata;
	ffarr out;
};

static void* hexprint_open(fcom_cmd *cmd)
{
	struct hexprint *c = ffmem_new(struct hexprint);
	return c;
}

static void hexprint_close(void *p, fcom_cmd *cmd)
{
	struct hexprint *c = p;
	ffarr_free(&c->fdata);
	ffarr_free(&c->out);
	ffmem_free(c);
}

// Note: uses much memory (reads the whole file)
static int hexprint_process(void *p, fcom_cmd *cmd)
{
	struct hexprint *c = p;
	const char *fn;

	for (;;) {
		if (NULL == (fn = com->arg_next(cmd, FCOM_CMD_ARG_FILE)))
			return FCOM_DONE;
		if (0 != fffile_readall(&c->fdata, fn, (uint64)-1))
			return FCOM_SYSERR;

		ffstdout_fmt("%s:\n", fn);

		ffstr hex = ffmem_print(c->fdata.ptr, c->fdata.len, 0);
		ffstdout_write(hex.ptr, hex.len);
	}
	return FCOM_DONE;
}

const fcom_filter hexprint_filt = { hexprint_open, hexprint_close, hexprint_process, };

#undef FILT_NAME
