/** fcom: change file date/time
2022, Simon Zolin */

static const char* touch_help()
{
	return "\
Change file date/time.\n\
By default uses the current date/time.\n\
Usage:\n\
  `fcom touch` INPUT... [OPTIONS]\n\
\n\
OPTIONS:\n\
    `-d`, `--date` STR      Set local date/time\n\
                            Format:\n\
                              yyyy-MM-dd [hh:mm:ss]\n\
    `-r`, `--reference` FILE\n\
                        Set date/time from this file\n\
";
}

#include <fcom.h>

static const fcom_core *core;

struct touch {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	uint stop;
	fcom_file_obj *in;
	fftime mtime;

	const char *reference_fn;
};

static int arg_date(struct touch *t, ffstr val)
{
	ffdatetime dt = {};
	if (val.len == fftime_fromstr1(&dt, val.ptr, val.len, FFTIME_YMD))
	{}
	else if (val.len == fftime_fromstr1(&dt, val.ptr, val.len, FFTIME_DATE_YMD))
	{}
	else
		return 0xbad;

	fftime_join1(&t->mtime, &dt);
	t->mtime.sec -= FFTIME_1970_SECONDS + core->tz.real_offset;
	return 0;
}

#define O(member)  (void*)FF_OFF(struct touch, member)
static int args_parse(struct touch *t, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--date",		'S',	arg_date },
		{ "--reference",'s',	O(reference_fn) },
		{ "-d",			'S',	arg_date },
		{ "-r",			's',	O(reference_fn) },
		{}
	};
	return core->com->args_parse(cmd, args, t, FCOM_COM_AP_INOUT);
}

static void touch_close(fcom_op *op)
{
	struct touch *t = op;
	core->file->destroy(t->in);
	ffmem_free(t);
}

static fcom_op* touch_create(fcom_cominfo *cmd)
{
	struct touch *t = ffmem_new(struct touch);

	if (0 != args_parse(t, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	t->in = core->file->create(&fc);

	t->cmd = cmd;
	return t;

end:
	touch_close(t);
	return NULL;
}

static int touch_do(struct touch *t, ffstr iname)
{
	const char *fn = iname.ptr;
	uint oflags = FCOM_FILE_READ;

	if (!t->cmd->recursive) {
		if (0 == fffile_set_mtime_path(fn, &t->mtime)) {
			goto end;
		}

		if (!fferr_notexist(fferr_last())) {
			fcom_syserrlog("fffile_set_mtime_path: %s", fn);
			return 0xbad;
		}

		oflags = FCOM_FILE_CREATENEW | FCOM_FILE_WRITE;
	}

	int r = core->file->open(t->in, fn, oflags);
	if (r == FCOM_FILE_ERR) return 0xbad;

	fffileinfo fi;
	r = core->file->info(t->in, &fi);
	if (r == FCOM_FILE_ERR) return 0xbad;

	if (0 != core->com->input_allowed(t->cmd, iname, fffile_isdir(fffileinfo_attr(&fi))))
		return 0;

	if (fffile_isdir(fffileinfo_attr(&fi))) {
		fffd fd = core->file->fd(t->in, FCOM_FILE_ACQUIRE);
		core->com->input_dir(t->cmd, fd);
	}

	core->file->close(t->in);

	if (0 != fffile_set_mtime_path(fn, &t->mtime)) {
		fcom_syserrlog("fffile_set_mtime_path: %s", fn);
		return 0xbad;
	}

end:
	fcom_verblog("touch: %s", fn);
	return 0;
}

static void touch_run(fcom_op *op)
{
	struct touch *t = op;
	int r, rc = 1;
	enum { I_INIT, I_IN, };

	while (!FFINT_READONCE(t->stop)) {
		switch (t->st) {
		case I_INIT:
			if (t->reference_fn != NULL) {
				fffileinfo fi;
				if (0 != fffile_info_path(t->reference_fn, &fi)) {
					fcom_syserrlog("%s", t->reference_fn);
					goto end;
				}
				t->mtime = fffileinfo_mtime(&fi);

			} else if (t->mtime.sec == 0) {
				fftime_now(&t->mtime);
			}
			t->st++;
			// fallthrough

		case I_IN: {
			ffstr in;
			if (0 > (r = core->com->input_next(t->cmd, &in, NULL, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE)
					rc = 0;
				goto end;
			}
			if (0 != (r = touch_do(t, in))) goto end;
			continue;
		}
		}
	}

end:
	{
	fcom_cominfo *cmd = t->cmd;
	touch_close(t);
	core->com->complete(cmd, rc);
	}
}

static void touch_signal(fcom_op *op, uint signal)
{
	struct touch *t = op;
	FFINT_WRITEONCE(t->stop, 1);
}

static const fcom_operation fcom_op_touch = {
	touch_create, touch_close,
	touch_run, touch_signal,
	touch_help,
};

FCOM_MOD_DEFINE(touch, fcom_op_touch, core)
