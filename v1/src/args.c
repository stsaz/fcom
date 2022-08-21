/** fcom: command-line arguments
2022, Simon Zolin */

#include <fcom.h>
#include <args.h>
#include <util/cmdarg-scheme.h>

void stdlog(uint flags, const char *fmt, ...);

static const ffcmdarg_arg args[] = {
	{ 'v',	"verbose",	FFCMDARG_TSWITCH, FF_OFF(struct args, verbose) },
	{ 'D',	"debug",	FFCMDARG_TSWITCH, FF_OFF(struct args, debug) },
	{}
};

int args_read(struct args *a, int argc, char **argv)
{
	ffstr errmsg = {};
	int r = ffcmdarg_parse_object(args, a, (const char**)argv, argc, FFCMDARG_SCF_SKIP_UNKNOWN, &errmsg);
	if (r < 0) {
		stdlog(FCOM_LOG_ERR, "command-line: %S", &errmsg);
		goto err;
	}

	a->argc = argc;
	a->argv = argv;
	return 0;

err:
	ffstr_free(&errmsg);
	return -1;
}

void args_destroy(struct args *a)
{
}
