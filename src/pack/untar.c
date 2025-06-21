/** fcom: unpack files from .tar
2023, Simon Zolin */

#include <ffsys/std.h>

static const char* untar_help()
{
	return "\
Unpack files from .tar.\n\
Usage:\n\
  `fcom untar` INPUT... [-C OUTPUT_DIR]\n\
\n\
OPTIONS:\n\
    `-m`, `--members-from-file` FILE\n\
                    Read archive member names from file\n\
    `-l`, `--list`      Just show the file list\n\
        `--plain`     Plain file names\n\
        `--autodir`   Add to OUTPUT_DIR a directory with name = input archive name.\n\
                     Same as manual 'untar arc.tar -C odir/arc'.\n\
";
}

#include <fcom.h>
#include <ffpack/tar-read.h>
#include <ffsys/path.h>
#include <ffbase/map.h>

extern const fcom_core *core;

struct untar {
	fcom_cominfo cominfo;

	uint state;
	fcom_cominfo *cmd;
	fftarread rtar;
	ffstr iname, base;
	ffstr tardata, plain;
	fcom_file_obj *in, *out;
	char *oname;
	ffvec buf;
	uint stop;
	uint del_on_close :1;

	// conf:
	byte list, list_plain;
	byte autodir;
	ffvec members_data;
	ffmap members; // char*[]
};

static int members_keyeq(void *opaque, const void *key, ffsize keylen, void *val)
{
	char *v = val;
	if (!ffmem_cmp(key, v, keylen) && v[keylen] == '\n')
		return 1;
	return 0;
}

/** Return !=0 if name matches the list of archive members for inclusion. */
static int members_find(ffmap *members, ffstr name)
{
	void *v = ffmap_find(members, name.ptr, name.len, NULL);
	return (v == NULL) ? 0 : 1;
}

static int untar_args_members_from_file(void *obj, char *fn)
{
	struct untar *t = obj;

	if (0 != fffile_readwhole(fn, &t->members_data, 100*1024*1024))
		return 1;
	ffvec_addchar(&t->members_data, '\n');

	uint n = 0;
	ffstr d = FFSTR_INITSTR(&t->members_data);
	while (d.len != 0) {
		ffstr ln;
		ffstr_splitby(&d, '\n', &ln, &d);
		if (ln.len == 0)
			continue;

		n++;
	}

	ffmap_init(&t->members, members_keyeq);
	ffmap_alloc(&t->members, n);

	ffstr_setstr(&d, &t->members_data);
	while (d.len != 0) {
		ffstr ln;
		ffstr_splitby(&d, '\n', &ln, &d);
		if (ln.len == 0)
			continue;

		ffmap_add(&t->members, ln.ptr, ln.len, ln.ptr);
	}
	return 0;
}

#define O(member)  (void*)FF_OFF(struct untar, member)

static int untar_args_parse(struct untar *t, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--autodir",				'1',	O(autodir) },
		{ "--list",					'1',	O(list) },
		{ "--members-from-file",	's',	untar_args_members_from_file },
		{ "--plain",				'1',	O(list_plain) },
		{ "-l",						'1',	O(list) },
		{ "-m",						's',	untar_args_members_from_file },
		{}
	};
	int r = core->com->args_parse(cmd, args, t, FCOM_COM_AP_INOUT);
	if (r != 0)
		return r;

	if (!t->list && cmd->output.len != 0) {
		fcom_fatlog("Use -C to set output directory");
		return -1;
	}

	if (cmd->chdir.len == 0)
		ffstr_dupz(&cmd->chdir, ".");

	return 0;
}

#undef O

static void untar_close(fcom_op *op)
{
	struct untar *t = op;
	fftarread_close(&t->rtar);
	core->file->destroy(t->in);
	if (t->del_on_close)
		core->file->del(t->oname, 0);
	core->file->destroy(t->out);
	ffvec_free(&t->buf);
	ffmem_free(t->oname);
	ffvec_free(&t->members_data);
	ffmap_free(&t->members);
	ffmem_free(t);
}

static fcom_op* untar_create(fcom_cominfo *cmd)
{
	struct untar *t = ffmem_new(struct untar);
	t->cmd = cmd;

	if (0 != untar_args_parse(t, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fcom_cmd_file_conf(&fc, cmd);
	t->in = core->file->create(&fc);
	t->out = core->file->create(&fc);

	ffsize cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&t->buf, cap, 1);
	return t;

end:
	untar_close(t);
	return NULL;
}

/* "size date name" */
static void untar_showinfo(struct untar *t, const fftarread_fileinfo_t *tf)
{
	ffvec *b = &t->buf;
	b->len = 0;

	if (!t->list_plain) {
		if (tf->type == TAR_DIR) {
			ffvec_addsz(b, "       <DIR>");
		} else {
			ffvec_addfmt(b, "%12U", tf->size);
		}
		ffvec_addchar(b, ' ');

		ffdatetime dt;
		fftime m = tf->mtime;
		m.sec += FFTIME_1970_SECONDS + core->tz.real_offset;

		fftime_split1(&dt, &m);
		b->len += fftime_tostr1(&dt, ffslice_end(b, 1), ffvec_unused(b), FFTIME_DATE_YMD | FFTIME_HMS);
		ffvec_addchar(b, ' ');
	}

	ffvec_addstr(b, &tf->name);
	ffvec_addchar(b, '\n');
	ffstdout_write(b->ptr, b->len);
}

/*
`d/f` -> `out/d/f`
`d/f` -> `out/iname/d/f` (--autodir)
*/
static char* untar_outname(struct untar *t, ffstr lname, ffstr rpath)
{
	if (t->autodir) {
		ffstr name;
		ffpath_splitpath_str(t->iname, NULL, &name);
		ffpath_splitname_str(name, &name, NULL);
		return ffsz_allocfmt("%S%c%S%c%S%Z", &rpath, FFPATH_SLASH, &name, FFPATH_SLASH, &lname);
	}
	return ffsz_allocfmt("%S%c%S%Z", &rpath, FFPATH_SLASH, &lname);
}

static int untar_read(struct untar *t, ffstr *input, ffstr *output)
{
	for (;;) {
		int r = fftarread_process(&t->rtar, input, output);
		switch ((enum FFTARREAD_R)r) {

		case FFTARREAD_FILEHEADER: {
			const fftarread_fileinfo_t *tf = fftarread_fileinfo(&t->rtar);
			fcom_dbglog("file header for %S", &tf->name);

			if (t->members.len != 0
				&& 0 == members_find(&t->members, tf->name))
				continue;

			if (t->list) {
				untar_showinfo(t, tf);
				continue;
			}

			switch (tf->type) {
			case TAR_FILE: case TAR_FILE0:
			case TAR_DIR:
			case TAR_HLINK: case TAR_SLINK:
				break;
			default:
				fcom_warnlog("%S: unsupported file type '%c'"
					, &tf->name, tf->type);
				continue;
			}

			ffmem_free(t->oname);
			t->oname = untar_outname(t, tf->name, t->cmd->chdir);
			return FFTARREAD_FILEHEADER;
		}

		case FFTARREAD_FILEDONE: {
			if (t->oname == NULL)
				continue;
			const fftarread_fileinfo_t *tf = fftarread_fileinfo(&t->rtar);
			fcom_dbglog("%S: %U", &tf->name, tf->size);
			return FFTARREAD_FILEDONE;
		}

		case FFTARREAD_DONE:
			fftarread_close(&t->rtar);
			return FFTARREAD_DONE;

		case FFTARREAD_DATA:
			if (t->oname == NULL)
				continue;
			return FFTARREAD_DATA;

		case FFTARREAD_MORE:
			return FFTARREAD_MORE;

		case FFTARREAD_WARNING:
			fcom_warnlog("fftarread_process: %s", fftarread_error(&t->rtar));
			continue;

		case FFTARREAD_ERROR:
			fcom_errlog("fftarread_process: %s", fftarread_error(&t->rtar));
			return FFTARREAD_ERROR;
		}
	}
}

static void untar_reset(struct untar *t)
{
}

static void untar_run(fcom_op *op)
{
	struct untar *t = op;
	int r, rc = 1;
	enum { I_IN, I_INFO, I_PARSE, I_FILEREAD, I_OUT_OPEN, I_WRITE };

	while (!FFINT_READONCE(t->stop)) {
		switch (t->state) {

		case I_IN:
			if (0 > (r = core->com->input_next(t->cmd, &t->iname, &t->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					rc = 0;
				}
				goto end;
			}

			t->state = I_INFO;
			// fallthrough

		case I_INFO: {
			uint flags = fcom_file_cominfo_flags_i(t->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(t->in, t->iname.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;

			fffileinfo fi = {};
			r = core->file->info(t->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;

			if (0 != core->com->input_allowed(t->cmd, t->iname, fffile_isdir(fffileinfo_attr(&fi)))) {
				t->state = I_IN;
				continue;
			}

			if ((t->base.len == 0 || t->cmd->recursive)
					&& fffile_isdir(fffileinfo_attr(&fi))) {
				fffd fd = core->file->fd(t->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(t->cmd, fd);
			}

			untar_reset(t);

			fftarread_open(&t->rtar);

			t->state = I_PARSE;
			continue;
		}

		case I_FILEREAD:
			r = core->file->read(t->in, &t->tardata, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(t->cmd);
				return;
			}
			if (r == FCOM_FILE_EOF) {
				fcom_errlog("incomplete archive");
				goto end;
			}
			t->state = I_PARSE;
			// fallthrough

		case I_PARSE:
			r = untar_read(t, &t->tardata, &t->plain);
			switch (r) {

			case FFTARREAD_FILEDONE: {
				const fftarread_fileinfo_t *tf = fftarread_fileinfo(&t->rtar);

				fftime t1 = tf->mtime;
				t1.sec += FFTIME_1970_SECONDS;
				core->file->mtime_set(t->out, t1);

#ifdef FF_UNIX
				if (tf->type == TAR_FILE)
					core->file->attr_set(t->out, tf->attr_unix);
#endif

				if (tf->type != TAR_DIR) {
					core->file->close(t->out);
					t->del_on_close = 0;
					fcom_verblog("%s", t->oname);
					ffmem_free(t->oname);  t->oname = NULL;
				}
				continue;
			}

			case FFTARREAD_DONE:
				t->state = I_IN; continue;

			case FFTARREAD_MORE:
				t->state = I_FILEREAD; continue;

			case FFTARREAD_FILEHEADER:
				t->state = I_OUT_OPEN; continue;

			case FFTARREAD_DATA:
				t->state = I_WRITE; continue;

			case FFTARREAD_ERROR:
				goto end;
			}
			continue;

		case I_OUT_OPEN: {
			const fftarread_fileinfo_t *tf = fftarread_fileinfo(&t->rtar);

			switch (tf->type) {
			case TAR_FILE:
			case TAR_FILE0:
				break;

			case TAR_DIR:
				r = core->file->dir_create(t->oname, FCOM_FILE_DIR_RECURSIVE);
				if (r == FCOM_FILE_ERR) goto end;
				t->state = I_PARSE;
				continue;

			case TAR_HLINK:
			case TAR_SLINK: {
				char *old = ffsz_dupstr(&tf->link_to);

				uint flags = (t->cmd->overwrite) ? FCOM_FILE_CREATE : 0;

				if (tf->type == TAR_HLINK)
					r = core->file->hlink(old, t->oname, 0);
				else
					r = core->file->slink(old, t->oname, flags);

				if (r == FCOM_FILE_ERR) goto end;
				t->state = I_PARSE;
				continue;
			}
			}

			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(t->cmd);
			r = core->file->open(t->out, t->oname, flags);
			if (r == FCOM_FILE_ERR) goto end;

			core->file->trunc(t->out, tf->size);

			t->del_on_close = !t->cmd->stdout && !t->cmd->test;
			t->state = I_PARSE;
			continue;
		}

		case I_WRITE:
			r = core->file->write(t->out, t->plain, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(t->cmd);
				return;
			}

			t->state = I_PARSE;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = t->cmd;
	untar_close(t);
	core->com->complete(cmd, rc);
	}
}

static void untar_signal(fcom_op *op, uint signal)
{
	struct untar *t = op;
	FFINT_WRITEONCE(t->stop, 1);
}

const fcom_operation fcom_op_untar = {
	untar_create, untar_close,
	untar_run, untar_signal,
	untar_help,
};
