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
#include <util/util.hpp>
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

	uint st;
	fcom_cominfo *cmd;
	xxstr			name, base;
	fcom_filexx		input;
	xxfileinfo		fi;
	xxvec			buf;
	uint stop;
	uint skip_prefix;

	u_char	long_fmt;
	u_char	one_line;

	list() : input(core) {}

	int read_input()
	{
		int r;
		if (0 > (r = core->com->input_next(this->cmd, &this->name, &this->base, 0))) {
			if (r == FCOM_COM_RINPUT_NOMORE) {
				return 'done';
			}
			return 'erro';
		}

		r = this->input.open(this->name.ptr, FCOM_FILE_READ | fcom_file_cominfo_flags_i(this->cmd));
		if (r == FCOM_FILE_ERR) return 'next';

		r = this->input.info(&this->fi);
		if (r == FCOM_FILE_ERR) return 'next';

		if (core->com->input_allowed(this->cmd, this->name, this->fi.dir()))
			return 'next';

		if ((this->base.len == 0 || this->cmd->recursive)
			&& this->fi.dir()) {
			core->com->input_dir(this->cmd, this->input.acquire_fd());

			if (!this->base.len)
				return 'next'; // skip directory itself (e.g. skip "." for "fcom list .")
		}

		this->input.close();
		return 0;
	}

	void process()
	{
		if (this->skip_prefix)
			ffstr_shift(&this->name, 2);

		if (!this->long_fmt && this->one_line) {
			if (this->name.find_char('"') >= 0)
				fcom_warnlog("file name '%S' contains double-quote character", &this->name);
			this->buf.add_f("\"%S\" ", &this->name);

		} else if (!this->long_fmt) {
			this->buf.add_f("%S" NEWLINE, &this->name);

		} else {
			ffdatetime dt;
			fftime_split1(&dt, &xxrval(this->fi.mtime1()));
			char date[128];
			int r = fftime_tostr1(&dt, date, sizeof(date), FFTIME_DATE_YMD | FFTIME_HMS_USEC);
			date[r] = '\0';

			this->buf.add_f("%12U %s %S" NEWLINE
				, this->fi.size(), date, &this->name);
		}
	}

	void display(int force)
	{
		if (force || this->buf.len >= 4096) {
			ffstdout_write(this->buf.ptr, this->buf.len);
			this->buf.len = 0;
		}
	}
};

#define O(member)  (void*)FF_OFF(struct list, member)

static int args_parse(struct list *l, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--long",		'1',	O(long_fmt) },
		{ "--oneline",	'1',	O(one_line) },
		{ "-l",			'1',	O(long_fmt) },
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

static void list_run(fcom_op *op)
{
	struct list *l = (struct list*)op;
	int rc = 1;
	enum { I_IN, I_PRINT, };

	while (!FFINT_READONCE(l->stop)) {
		switch (l->st) {

		case I_IN:
			switch (l->read_input()) {
			case 'next':
				continue;

			case 'done':
				l->display(1);
				rc = 0;
				goto end;

			case 'erro':
				goto end;
			}

			l->st = I_PRINT;
			// fallthrough

		case I_PRINT:
			l->process();
			l->display(0);
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

FCOM_MOD_DEFINE(list, fcom_op_list, core)
