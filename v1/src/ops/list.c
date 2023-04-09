/** fcom: list files
2022, Simon Zolin */

#include <fcom.h>
#include <FFOS/path.h>
#include <FFOS/std.h>

static const fcom_core *core;

struct list {
	uint st;
	fcom_cominfo *cmd;
	ffstr name, base;
	fcom_file_obj *in;
	fffileinfo fi;
	ffvec buf;
	uint stop;

	byte long_fmt;
};

static const char* list_help()
{
	return "\
List directory contents.\n\
Usage:\n\
  fcom list INPUT... [OPTIONS]\n\
    OPTIONS:\n\
    -l, --long          Use long format\n\
";
}

static int args_parse(struct list *l, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{ 'l',	"long",	FFCMDARG_TSWITCH, FF_OFF(struct list, long_fmt) },
		{}
	};
	return core->com->args_parse(cmd, args, l);
}

static void list_close(fcom_op *op);

static fcom_op* list_create(fcom_cominfo *cmd)
{
	struct list *l = ffmem_new(struct list);
	l->cmd = cmd;

	if (0 != args_parse(l, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	l->in = core->file->create(&fc);

	ffsize cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&l->buf, cap, 1);
	return l;

end:
	list_close(l);
	return NULL;
}

static void list_close(fcom_op *op)
{
	struct list *l = op;
	core->file->destroy(l->in);
	ffvec_free(&l->buf);
	ffmem_free(l);
}

static void list_run(fcom_op *op)
{
	struct list *l = op;
	int r, rc = 1;
	enum { I_IN, I_INFO, I_PRINT, };

	while (!FFINT_READONCE(l->stop)) {
		switch (l->st) {

		case I_IN: {
			if (0 > (r = core->com->input_next(l->cmd, &l->name, &l->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					ffstdout_write(l->buf.ptr, l->buf.len);
					rc = 0;
				}
				goto end;
			}

			l->st = I_INFO;
		}
			// fallthrough

		case I_INFO: {
			uint flags = fcom_file_cominfo_flags_i(l->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(l->in, l->name.ptr, flags);
			if (r == FCOM_FILE_ERR) goto next;

			r = core->file->info(l->in, &l->fi);
			if (r == FCOM_FILE_ERR) goto next;

			if ((l->base.len == 0 || l->cmd->recursive)
				&& fffile_isdir(fffileinfo_attr(&l->fi))) {
				fffd fd = core->file->fd(l->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(l->cmd, fd);
			}

			if (0 != core->com->input_allowed(l->cmd, l->name))
				goto next;

			core->file->close(l->in);
			l->st = I_PRINT;
			continue;
next:
			l->st = I_IN;
			continue;
		}

		case I_PRINT:
			if (!l->long_fmt) {
				r = ffstr_addfmt((ffstr*)&l->buf, l->buf.cap, "%S\r\n", &l->name);
			} else {
				fftime mtime = fffileinfo_mtime1(&l->fi);
				ffdatetime dt;
				fftime_split1(&dt, &mtime);
				char date[128];
				r = fftime_tostr1(&dt, date, sizeof(date), FFTIME_DATE_YMD | FFTIME_HMS_USEC);
				date[r] = '\0';

				uint64 size = fffileinfo_size(&l->fi);
				r = ffstr_addfmt((ffstr*)&l->buf, l->buf.cap, "%12U %s %S\r\n"
					, size, date, &l->name);
			}

			if (r == 0) {
				if (l->buf.len == 0) {
					fcom_sysfatlog("too small buffer");
					goto end;
				}
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
	struct list *l = op;
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
FF_EXP const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	list_init, list_destroy, list_provide_op,
};
