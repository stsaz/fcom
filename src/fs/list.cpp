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
";
}

#include <fcom.h>
#include <util/util.hpp>
#include <ffsys/path.h>
#include <ffsys/std.h>

static const fcom_core *core;

struct list {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	ffstr name, base;
	fcom_filexx		input;
	xxfileinfo		fi;
	ffvecxx			buf;
	uint stop;
	uint skip_prefix;

	byte long_fmt;

	list() : input(core) {}
};

#define O(member)  (void*)FF_OFF(struct list, member)

static int args_parse(struct list *l, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--long",	'1',	O(long_fmt) },
		{ "-l",		'1',	O(long_fmt) },
		{}
	};
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
	l->~list();
	ffmem_free(l);
}

static fcom_op* list_create(fcom_cominfo *cmd)
{
	struct list *l = new(ffmem_new(struct list)) struct list;
	l->cmd = cmd;
	struct fcom_file_conf fc = {};
	ffsize cap;

	if (0 != args_parse(l, cmd))
		goto end;

	fc.buffer_size = cmd->buffer_size;
	l->input.create(&fc);

	cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	l->buf.alloc<char>(cap);
	return l;

end:
	list_close(l);
	return NULL;
}

#ifdef FF_WIN
	#define NEWLINE  "\r\n"
#else
	#define NEWLINE  "\n"
#endif

static void list_run(fcom_op *op)
{
	struct list *l = (struct list*)op;
	int r, rc = 1;
	enum { I_IN, I_INFO, I_PRINT, };

	while (!FFINT_READONCE(l->stop)) {
		switch (l->st) {

		case I_IN:
			if (0 > (r = core->com->input_next(l->cmd, &l->name, &l->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					ffstdout_write(l->buf.ptr, l->buf.len);
					rc = 0;
				}
				goto end;
			}

			l->st = I_INFO;
			// fallthrough

		case I_INFO:
			r = l->input.open(l->name.ptr, FCOM_FILE_READ | fcom_file_cominfo_flags_i(l->cmd));
			if (r == FCOM_FILE_ERR) goto next;

			r = l->input.info(&l->fi);
			if (r == FCOM_FILE_ERR) goto next;

			if (0 != core->com->input_allowed(l->cmd, l->name, l->fi.dir()))
				goto next;

			if ((l->base.len == 0 || l->cmd->recursive)
				&& l->fi.dir()) {
				core->com->input_dir(l->cmd, l->input.acquire_fd());

				if (!l->base.len)
					goto next; // skip directory itself (e.g. skip "." for "fcom list .")
			}

			l->input.close();
			l->st = I_PRINT;
			continue;
next:
			l->st = I_IN;
			continue;

		case I_PRINT:
			if (l->skip_prefix)
				ffstr_shift(&l->name, 2);

			if (!l->long_fmt) {
				l->buf.addf("%S" NEWLINE, &l->name);
			} else {
				ffdatetime dt;
				fftime_split1(&dt, &xxrval(l->fi.mtime1()));
				char date[128];
				r = fftime_tostr1(&dt, date, sizeof(date), FFTIME_DATE_YMD | FFTIME_HMS_USEC);
				date[r] = '\0';

				l->buf.addf("%12U %s %S" NEWLINE
					, l->fi.size(), date, &l->name);
			}

			if (!l->buf.len) {
				ffstdout_write(l->buf.ptr, l->buf.len);
				l->buf.len = 0;
				continue;
			}

			l->st = I_IN;
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


static void list_init(const fcom_core *_core) { core = _core; }
static void list_destroy() {}
static const fcom_operation* list_provide_op(const char *name)
{
	if (ffsz_eq(name, "list"))
		return &fcom_op_list;
	return NULL;
}
FCOM_EXPORT const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	list_init, list_destroy, list_provide_op,
};
