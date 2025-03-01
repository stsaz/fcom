/** fcom: command-line arguments
2022, Simon Zolin */

#include <fcom.h>
#include <exe/args.h>
#include <ffsys/std.h>

void exe_log(uint flags, const char *fmt, ...);

static void help_info_write(ffstr s)
{
	ffstr l, k;
	ffvec v = {};

	int use_color = !ffstd_attr(ffstdout, FFSTD_VTERM, FFSTD_VTERM);

	const char *clr = FFSTD_CLR_B(FFSTD_PURPLE);
	while (s.len) {
		ffstr_splitby(&s, '`', &l, &s);
		ffstr_splitby(&s, '`', &k, &s);
		if (use_color) {
			ffvec_addfmt(&v, "%S%s%S%s"
				, &l, clr, &k, FFSTD_CLR_RESET);
		} else {
			ffvec_addfmt(&v, "%S%S"
				, &l, &k);
		}
	}

	ffstdout_write(v.ptr, v.len);
	ffvec_free(&v);
}

#define HELP_TXT "help.txt"
extern char* path(const char *fn);

static int args_help(struct args *a)
{
	char *fn = path(HELP_TXT);
	ffvec d = {};
	if (0 != fffile_readwhole(fn, &d, 64*1024)) {
		exe_log(FCOM_LOG_SYSERR, "file read: %s", fn);
		return 1;
	}
	help_info_write(*(ffstr*)&d);
	ffvec_free(&d);
	ffmem_free(fn);
	return 1;
}

#define O(member)  (void*)FF_OFF(struct args, member)

static const struct ffarg exe_args[] = {
	{ "--Debug",	'1',	O(debug) },
	{ "--Verbose",	'1',	O(verbose) },

	{ "--help",		'0',	args_help },

	{ "-D",			'1',	O(debug) },
	{ "-V",			'1',	O(verbose) },

	{ "-h",			'0',	args_help },
	{}
};

int args_read(struct args *a, uint argc, char **argv, char *cmd_line)
{
#ifdef FF_WIN
	// Prepare argv[] array
	ffstr line = FFSTR_Z(cmd_line), arg;
	while (!_ffargs_next(&line, &arg)) {
		arg.ptr[arg.len] = '\0';
		*ffvec_pushT(&a->args, char*) = arg.ptr;
	}
	ffvec_zpushT(&a->args, char*);
	argv = a->args.ptr;
	argc = a->args.len - 1;
#endif

	struct ffargs as = {};
	uint f = FFARGS_O_PARTIAL | FFARGS_O_DUPLICATES | FFARGS_O_SKIP_FIRST;
	int r = ffargs_process_argv(&as, exe_args, a, f, argv, argc);
	if (r > 0)
		return 1;
	if (r == -FFARGS_E_ARG) {
		// we met the first operation-specific argument
		as.argi--;
	} else if (r < 0) {
		exe_log(FCOM_LOG_ERR, "command-line: %s", as.error);
		goto err;
	}

	a->argc = argc - as.argi;
	a->argv = argv + as.argi;
	return 0;

err:
	return -1;
}

void args_destroy(struct args *a)
{
	ffvec_free(&a->args);
}
