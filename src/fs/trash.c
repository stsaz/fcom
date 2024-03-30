/** fcom: move files to user's trash directory
2022, Simon Zolin */

static const char* trash_help()
{
	return "\
Move files to user's trash directory, plus obfuscation.\n\
Usage:\n\
  `fcom trash` INPUT...\n\
\n\
OPTIONS:\n\
    `-w`, `--wipe`          Overwrite data to hide file content (files-only).\n\
                        Additionally, set file modification time to 2000-01-01.\n\
    `-n`, `--rename`        Rename to \"00000000.0000\" before deleting (files-only)\n\
    `-f`                  Delete from disk if moving to Trash has failed (files-only)\n\
";
}

#ifdef _WIN32
#include <util/windows-shell.h>
#endif
#include <fcom.h>
#include <util/unix-shell.h>
#include <ffsys/path.h>

static const fcom_core *core;

struct trash {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	fcom_file_obj *in;
	ffvec buf;
	uint stop;
	ffvec names; //char*[]
	uint n_trashed, n_deleted;

	byte wipe;
	byte rename;
};

#define O(member)  (void*)FF_OFF(struct trash, member)
static int args_parse(struct trash *t, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--rename",	'1',	O(rename) },
		{ "--wipe",		'1',	O(wipe) },
		{ "-n",			'1',	O(rename) },
		{ "-w",			'1',	O(wipe) },
		{}
	};
	return core->com->args_parse(cmd, args, t, FCOM_COM_AP_INOUT);
}

static void trash_close(fcom_op *op);

static fcom_op* trash_create(fcom_cominfo *cmd)
{
	struct trash *t = ffmem_new(struct trash);
	t->cmd = cmd;

	if (0 != args_parse(t, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	t->in = core->file->create(&fc);
	return t;

end:
	trash_close(t);
	return NULL;
}

static void trash_close(fcom_op *op)
{
	struct trash *t = op;
	core->file->destroy(t->in);

	char **p;
	FFSLICE_WALK(&t->names, p) {
		ffmem_free(*p);
	}
	ffvec_free(&t->names);

	ffvec_free(&t->buf);
	ffmem_free(t);
}

/** Fill buffer with pseudo-random data */
static void data_rnd(ffvec *d, uint chunk)
{
	FF_ASSERT((chunk & 3) == 0);
	char *p = d->ptr;
	d->len = ffint_align_ceil(d->len, chunk);
	const char *end = d->ptr + d->len;

	while (p < end) {
		uint rnd = core->random();
		*(uint*)p = rnd;
		p += 4;
	}
}

static void f_wipe_mtime(struct trash *t, fcom_file_obj *f)
{
	fftime mtime;
	ffdatetime dt = {
		.year = 2000,
		.month = 1,
		.day = 1,
		0,
	};
	fftime_join1(&mtime, &dt);
	core->file->mtime_set(f, mtime);
}

static int f_wipe(struct trash *t, const char *fn)
{
	int rc = -1;
	ffvec d = {};

	struct fcom_file_conf conf = {};
	fcom_file_obj *f = core->file->create(&conf);

	uint flags = FCOM_FILE_WRITE | FCOM_FILE_NO_PREALLOC;
	if (t->cmd->directio)
		flags |= FCOM_FILE_DIRECTIO;

	int r = core->file->open(f, fn, flags);
	if (r == FCOM_FILE_ERR) goto end;

	fffileinfo fi = {};
	r = core->file->info(f, &fi);
	if (r == FCOM_FILE_ERR) goto end;
	uint64 fsize = fffileinfo_size(&fi);

	if (t->cmd->buffer_size == 0)
		t->cmd->buffer_size = 64*1024;
	t->cmd->buffer_size = ffmax(t->cmd->buffer_size, 4096);
	ffvec_alloc(&d, t->cmd->buffer_size, 1);

	while ((int64)fsize > 0) {
		d.len = ffmin(fsize, d.cap);
		data_rnd(&d, 4096);
		fsize -= d.len;

		if (FFINT_READONCE(t->stop))
			goto end;

		r = core->file->write(f, *(ffstr*)&d, -1);
		if (r == FCOM_FILE_ERR) goto end;
	}

	f_wipe_mtime(t, f);

	core->file->close(f);

	fcom_verblog("%s: wiped data", fn);
	rc = 0;

end:
	ffvec_free(&d);
	core->file->destroy(f);
	return rc;
}

/** Move files to Trash */
static int f_trash(struct trash *t, const char **names, ffsize names_n)
{
#ifdef FF_LINUX
	int e = 0;
	for (ffsize i = 0;  i != names_n;  i++) {
		const char *err;
		if (0 != ffui_glib_trash(names[i], &err)) {
			if (!t->cmd->overwrite) {
				fcom_fatlog("%s: can't move file to trash: %s"
					, names[i], err);
			}
			e = 1;
		}
	}
	if (e)
		return -1;

#else
	if (0 != ffui_file_del(names, names_n, FFUI_FILE_TRASH)) {
		if (!t->cmd->overwrite)
			fcom_sysfatlog("can't move files to trash");
		return -1;
	}
#endif

	return 0;
}

/** Delete files from disk
Return N of files deleted */
static int f_del(ffvec names)
{
	uint n = 0;
	const char **namez;
	FFSLICE_WALK(&names, namez) {
		if (0 != core->file->del(*namez, 0))
			n++;
	}

	if (n != 0) {
		fcom_fatlog("%u files were not deleted", n);
	}

	return names.len - n;
}

/** /path/old -> /path/new */
static ffstr f_new_name(struct trash *t, ffstr old, const char *_new)
{
	ffstr path;
	if (ffpath_splitpath_str(old, &path, NULL) >= 0)
		path.len++;

	t->buf.len = 0;
	ffvec_addfmt(&t->buf, "%S%s%Z", &path, _new);
	t->buf.len--;
	return *(ffstr*)&t->buf;
}

static int trash_all(struct trash *t)
{
	int r;
	if ((r = f_trash(t, t->names.ptr, t->names.len))) {
		if (t->cmd->overwrite && !t->cmd->test) {
			fcom_verblog("can't move files to trash: error code %d.  Trying to delete them.", r);
			if (0 == (t->n_deleted = f_del(t->names)))
				return -1;

		} else {
			return -1;
		}
	} else {
		t->n_trashed += t->names.len;
	}

	if (core->verbose) {
		const char **namez;
		FFSLICE_WALK(&t->names, namez) {
			fcom_verblog("trash: %s", *namez);
		}
	}
	return 0;
}

static int trash_process(struct trash *t)
{
	const char **namez;
	FFSLICE_WALK(&t->names, namez) {
		const char *fn = *namez;

		if (t->wipe) {
			if (0 != f_wipe(t, fn)) {
				return -1;
			}
		}

		if (t->rename) {
			ffstr old = FFSTR_INITZ(fn);
			ffstr _new = f_new_name(t, old, "00000000.0000");
			if (0 != core->file->move(old, _new, FCOM_FILE_MOVE_SAFE)) {
				return -1;
			}
			fn = _new.ptr;
		}

		if (0 != f_trash(t, &fn, 1)) {
			if (t->cmd->overwrite && !t->cmd->test) {
				fcom_verblog("%s: can't move to trash.  Deleting."
					, fn);
				if (0 == core->file->del(fn, 0)) {
					t->n_deleted++;
				}
			}
		} else {
			fcom_verblog("trash: %s", fn);
			t->n_trashed++;
		}
	}
	return 0;
}

static void trash_done(struct trash *t)
{
	if (core->verbose) {
		fcom_verblog("%u files were moved to Trash", t->n_trashed);
		if (t->n_deleted != 0) {
			fcom_verblog("%u files were deleted", t->n_deleted);
		}
	}

	uint n = t->names.len - (t->n_trashed + t->n_deleted);
	if (n != 0) {
		fcom_errlog("%u files were skipped", n);
	}
}

static void trash_run(fcom_op *op)
{
	enum { I_IN, I_ALL, I_OBO, I_DONE };
	struct trash *t = op;
	int r, rc = 1;
	ffstr name;
	while (!FFINT_READONCE(t->stop)) {
		switch (t->st) {
		case 0: {
			if (0 > (r = core->com->input_next(t->cmd, &name, NULL, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					t->st = I_ALL;
					if (t->wipe || t->rename)
						t->st = I_OBO;
					continue;
				}
				goto end;
			}

			char **p = ffvec_pushT(&t->names, char*);
			*p = ffsz_dupstr(&name);
			continue;
		}

		case I_ALL:
			if (trash_all(t)) goto end;
			t->st = I_DONE;
			continue;

		case I_OBO:
			if (trash_process(t)) goto end;
			t->st = I_DONE;
			continue;

		case I_DONE:
			trash_done(t);
			rc = 0;
			goto end;
		}
	}

end:
	{
	fcom_cominfo *cmd = t->cmd;
	trash_close(t);
	core->com->complete(cmd, rc);
	}
}

static void trash_signal(fcom_op *op, uint signal)
{
	struct trash *t = op;
	FFINT_WRITEONCE(t->stop, 1);
}

static const fcom_operation fcom_op_trash = {
	trash_create, trash_close,
	trash_run, trash_signal,
	trash_help,
};

FCOM_MOD_DEFINE(trash, fcom_op_trash, core)
