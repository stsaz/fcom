/** fcom: list files
2022, Simon Zolin */

static const char* list_help()
{
	return "\
List directory contents.\n\
Usage:\n\
  `fcom list` INPUT... [OPTIONS]\n\
\n\
OPTIONS:\n\
    `-l`, `--long`          Use long format\n\
          `--oneline`       Display all file names in a single line\n\
";
}

#include <fcom.h>
#include <ffsys/path.h>
#include <ffsys/std.h>

static const fcom_core *core;

#ifdef FF_WIN
	#define NEWLINE  "\r\n"
#else
	#define NEWLINE  "\n"
#endif

struct list {
	fcom_cominfo cominfo;

	uint state;
	fcom_cominfo *cmd;
	ffstr			name, base;
	fffileinfo		fi;
	ffvec			buf;
	uint stop;
	uint skip_prefix;

	u_char	long_fmt;
	u_char	one_line;
};

static int list_read_input(struct list *l)
{
	int r;
	if (0 > (r = core->com->input_next(l->cmd, &l->name, &l->base, 0))) {
		if (r == FCOM_COM_RINPUT_NOMORE) {
			return 'done';
		} else {
			return 'next';
		}
	}

	if (fffile_info_path(l->name.ptr, &l->fi))
		return 'next';

	unsigned dir = fffile_isdir(fffileinfo_attr(&l->fi));
	if (core->com->input_allowed(l->cmd, l->name, dir))
		return 'next';

	if ((!l->base.len || l->cmd->recursive)
		&& dir) {
		core->com->input_dir(l->cmd, FFFILE_NULL);

		if (!l->base.len)
			return 'next'; // skip directory itself (e.g. skip "." for "fcom list .")
	}

	return 0;
}

static void list_process(struct list *l)
{
	if (l->skip_prefix)
		ffstr_shift(&l->name, 2);

	if (!l->long_fmt && l->one_line) {
		if (ffstr_findchar(&l->name, '"') >= 0)
			fcom_warnlog("file name '%S' contains double-quote character", &l->name);
		ffvec_addfmt(&l->buf, "\"%S\" ", &l->name);

	} else if (!l->long_fmt) {
		ffvec_addfmt(&l->buf, "%S" NEWLINE, &l->name);

	} else {
		ffdatetime dt;
		fftime t = fffileinfo_mtime1(&l->fi);
		fftime_split1(&dt, &t);
		char date[128];
		int r = fftime_tostr1(&dt, date, sizeof(date), FFTIME_DATE_YMD | FFTIME_HMS_USEC);
		date[r] = '\0';

		ffvec_addfmt(&l->buf, "%12U %s %S" NEWLINE
			, fffileinfo_size(&l->fi), date, &l->name);
	}
}

static void list_display(struct list *l, int force)
{
	if (force || l->buf.len >= 4096) {
		ffstdout_write(l->buf.ptr, l->buf.len);
		l->buf.len = 0;
	}
}

static int args_parse(struct list *l, fcom_cominfo *cmd)
{
	#define O(member)  (void*)FF_OFF(struct list, member)
	static const struct ffarg args[] = {
		{ "--long",		'1',	O(long_fmt) },
		{ "--oneline",	'1',	O(one_line) },
		{ "-l",			'1',	O(long_fmt) },
		{}
	};
	#undef O
	int r = core->com->args_parse(cmd, args, l, FCOM_COM_AP_INOUT);
	if (r) return r;

	if (!cmd->input.len) {
		ffstr *s = ffvec_pushT(&cmd->input, ffstr);
		s->ptr = ffsz_dup(".");
		s->len = 1;
		l->skip_prefix = 1;
	}
	return 0;
}

static void list_close(fcom_op *op)
{
	struct list *l = (struct list*)op;
	ffvec_free(&l->buf);
	ffmem_free(l);
}

static fcom_op* list_create(fcom_cominfo *cmd)
{
	struct list *l = ffmem_new(struct list);
	l->cmd = cmd;
	ffsize cap;

	if (args_parse(l, cmd))
		goto end;

	cap = (cmd->buffer_size) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&l->buf, cap, 1);
	return l;

end:
	list_close(l);
	return NULL;
}

static void list_run(fcom_op *op)
{
	struct list *l = (struct list*)op;
	int rc = 1;
	enum { I_IN, I_PRINT, };

	while (!FFINT_READONCE(l->stop)) {
		switch (l->state) {

		case I_IN:
			switch (list_read_input(l)) {
			case 'next':
				continue;

			case 'done':
				list_display(l, 1);
				rc = 0;
				goto end;

			case 'erro':
				goto end;
			}

			l->state = I_PRINT;
			// fallthrough

		case I_PRINT:
			list_process(l);
			list_display(l, 0);
			l->state = I_IN;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = l->cmd;
	list_close(l);
	core->com->complete(cmd, rc);
	}
}

static void list_signal(fcom_op *op, uint signal)
{
	struct list *l = (struct list*)op;
	FFINT_WRITEONCE(l->stop, 1);
}

static const fcom_operation fcom_op_list = {
	list_create, list_close,
	list_run, list_signal,
	list_help,
};

FCOM_MOD_DEFINE(list, fcom_op_list, core)
