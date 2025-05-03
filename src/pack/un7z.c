/** fcom: unpack files from .7z
2023, Simon Zolin */

static const char* un7z_help()
{
	return "\
Unpack files from .7z.\n\
Usage:\n\
  `fcom un7z` INPUT... [-C OUTPUT_DIR]\n\
\n\
OPTIONS:\n\
    `-m`, `--members-from-file` FILE\n\
                    Read archive member names from file\n\
    `-l`, `--list`      Just show the file list\n\
        `--autodir`   Add to OUTPUT_DIR a directory with name = input archive name.\n\
                     Same as manual 'un7z arc.7z -C odir/arc'.\n\
";
}

#include <fcom.h>
#include <ffpack/7z-read.h>
#include <ffsys/path.h>
#include <ffsys/globals.h>
#include <ffbase/map.h>

const fcom_core *core;

struct un7z {
	fcom_cominfo cominfo;

	uint state;
	fcom_cominfo *cmd;
	ff7zread z7;
	ffstr iname, base;
	ffstr zdata, plain;
	fcom_file_obj *in, *out;
	const ff7zread_fileinfo *curfile;
	char *oname;
	ffvec buf;
	uint stop;
	uint del_on_close :1;
	uint64 total_uncomp;
	int64 roff;

	// conf:
	byte list;
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

static int un7z_args_members_from_file(void *obj, char *fn)
{
	struct un7z *z = obj;

	if (0 != fffile_readwhole(fn, &z->members_data, 100*1024*1024))
		return 1;
	ffvec_addchar(&z->members_data, '\n');

	uint n = 0;
	ffstr d = FFSTR_INITSTR(&z->members_data);
	while (d.len != 0) {
		ffstr ln;
		ffstr_splitby(&d, '\n', &ln, &d);
		if (ln.len == 0)
			continue;

		n++;
	}

	ffmap_init(&z->members, members_keyeq);
	ffmap_alloc(&z->members, n);

	ffstr_setstr(&d, &z->members_data);
	while (d.len != 0) {
		ffstr ln;
		ffstr_splitby(&d, '\n', &ln, &d);
		if (ln.len == 0)
			continue;

		ffmap_add(&z->members, ln.ptr, ln.len, ln.ptr);
	}
	return 0;
}

#define O(member)  (void*)FF_OFF(struct un7z, member)

static int un7z_args_parse(struct un7z *z, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--autodir",				'1',	O(autodir) },
		{ "--list",					'1',	O(list) },
		{ "--members-from-file",	's',	un7z_args_members_from_file },
		{ "-l",						'1',	O(list) },
		{ "-m",						's',	un7z_args_members_from_file },
		{}
	};
	int r = core->com->args_parse(cmd, args, z, FCOM_COM_AP_INOUT);
	if (r != 0)
		return r;

	if (!z->list && cmd->output.len != 0) {
		fcom_fatlog("Use -C to set output directory");
		return -1;
	}

	if (cmd->chdir.len == 0)
		ffstr_dupz(&cmd->chdir, ".");

	return 0;
}

#undef O

static void un7z_log(void *udata, uint level, ffstr msg)
{
	fcom_dbglog("%S", &msg);
}

static void un7z_close(fcom_op *op)
{
	struct un7z *z = op;
	ff7zread_close(&z->z7);
	core->file->destroy(z->in);
	if (z->del_on_close)
		core->file->del(z->oname, 0);
	core->file->destroy(z->out);
	ffvec_free(&z->buf);
	ffmem_free(z->oname);
	ffvec_free(&z->members_data);
	ffmap_free(&z->members);
	ffmem_free(z);
}

static fcom_op* un7z_create(fcom_cominfo *cmd)
{
	struct un7z *z = ffmem_new(struct un7z);
	z->cmd = cmd;

	if (0 != un7z_args_parse(z, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fcom_cmd_file_conf(&fc, cmd);
	z->in = core->file->create(&fc);
	z->out = core->file->create(&fc);

	ffsize cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&z->buf, cap, 1);
	return z;

end:
	un7z_close(z);
	return NULL;
}

/* "size zsize(%) date name" */
static void un7z_showinfo(struct un7z *z, const ff7zread_fileinfo *zf)
{
	ffvec *b = &z->buf;
	b->len = 0;

	if (core->verbose) {
		if (zf->attr & FFFILE_WIN_DIR) {
			ffvec_addsz(b, "       <DIR>                  ");
		} else {
			ffvec_addfmt(b, "%12U", zf->size);
		}
		ffvec_addchar(b, ' ');

		ffdatetime dt;
		fftime m = zf->mtime;
		m.sec += FFTIME_1970_SECONDS + core->tz.real_offset;

		fftime_split1(&dt, &m);
		b->len += fftime_tostr1(&dt, ffslice_end(b, 1), ffvec_unused(b), FFTIME_DATE_YMD | FFTIME_HMS);
		ffvec_addchar(b, ' ');
	}

	if (ffutf8_valid(zf->name.ptr, zf->name.len)) {
		ffvec_addstr(b, &zf->name);

	} else {
		ffvec_grow(b, zf->name.len * 4, 1);
		ffssize r = ffutf8_from_cp(ffslice_end(b, 1), ffvec_unused(b), zf->name.ptr, zf->name.len, core->codepage);
		if (r < 0) {
			fcom_errlog("ffutf8_from_cp: %S", &zf->name);
			return;
		}
		b->len += r;
	}

	ffstdout_write(b.ptr, b.len);
}

static int un7z_f_info(struct un7z *z, const ff7zread_fileinfo *zf)
{
	fcom_dbglog("header for %S: %U  attr:%xu"
		, &zf->name, zf->size, zf->attr);

	if ((zf->attr & FFFILE_WIN_DIR) && zf->size != 0)
		fcom_infolog("directory %S has non-zero size", &zf->name);

	if (z->members.len != 0
		&& 0 == members_find(&z->members, zf->name))
		return 0;

	if (z->list) {
		un7z_showinfo(z, zf);
		z->total_uncomp += zf->size;
		return 0;
	}

	return 1;
}

/*
`d/f` -> `out/d/f`
`d/f` -> `out/iname/d/f` (--autodir)
*/
static char* un7z_outname(struct un7z *z, ffstr lname, ffstr rpath)
{
	if (z->autodir) {
		ffstr name;
		ffpath_splitpath_str(z->iname, NULL, &name);
		ffpath_splitname_str(name, &name, NULL);
		return ffsz_allocfmt("%S%c%S%c%S%Z", &rpath, FFPATH_SLASH, &name, FFPATH_SLASH, &lname);
	}
	return ffsz_allocfmt("%S%c%S%Z", &rpath, FFPATH_SLASH, &lname);
}

static int un7z_read(struct un7z *z, ffstr *input, ffstr *output)
{
	for (;;) {
		int r = ff7zread_process(&z->z7, input, output);
		switch ((enum FF7ZREAD_R)r) {

		case FF7ZREAD_FILEHEADER: {
			const ff7zread_fileinfo *zf;
			for (;;) {
				if (NULL == (zf = ff7zread_nextfile(&z->z7))) {
					ff7zread_close(&z->z7);
					return 'done';
				}
				if (un7z_f_info(z, zf))
					break;
			}

			z->curfile = zf;
			ffmem_free(z->oname);
			z->oname = un7z_outname(z, zf->name, z->cmd->chdir);
			return FF7ZREAD_FILEHEADER;
		}

		case FF7ZREAD_FILEDONE: {
			fcom_verblog("%S: %U", &z->curfile->name, z->curfile->size);
			return FF7ZREAD_FILEDONE;
		}

		case FF7ZREAD_DATA:
			return FF7ZREAD_DATA;

		case FF7ZREAD_MORE:
			return 'more';

		case FF7ZREAD_SEEK:
			z->roff = ff7zread_offset(&z->z7);
			return 'more';

		case FF7ZREAD_ERROR:
			fcom_errlog("%s", ff7zread_error(&z->z7));
			return FF7ZREAD_ERROR;
		}
	}
}

static void un7z_reset(struct un7z *z)
{
	z->total_uncomp = 0;
	z->roff = -1;
}

static void un7z_run(fcom_op *op)
{
	struct un7z *z = op;
	int r, rc = 1;
	enum { I_IN, I_INFO, I_PARSE, I_FILEREAD, I_OUT_OPEN, I_WRITE };

	while (!FFINT_READONCE(z->stop)) {
		switch (z->state) {

		case I_IN:
			if (0 > (r = core->com->input_next(z->cmd, &z->iname, &z->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					rc = 0;
				}
				goto end;
			}

			z->state = I_INFO;
			// fallthrough

		case I_INFO: {
			uint flags = fcom_file_cominfo_flags_i(z->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(z->in, z->iname.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;

			fffileinfo fi = {};
			r = core->file->info(z->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;

			if (0 != core->com->input_allowed(z->cmd, z->iname, fffile_isdir(fffileinfo_attr(&fi)))) {
				z->state = I_IN;
				continue;
			}

			if ((z->base.len == 0 || z->cmd->recursive)
					&& fffile_isdir(fffileinfo_attr(&fi))) {
				fffd fd = core->file->fd(z->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(z->cmd, fd);
			}

			un7z_reset(z);

			ff7zread_open(&z->z7);
			z->z7.log = un7z_log;
			z->z7.udata = z;

			z->state = I_PARSE;
			continue;
		}

		case I_FILEREAD:
			r = core->file->read(z->in, &z->zdata, z->roff);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(z->cmd);
				return;
			}
			if (r == FCOM_FILE_EOF) {
				fcom_errlog("incomplete archive");
				goto end;
			}
			z->roff = -1;
			z->state = I_PARSE;
			// fallthrough

		case I_PARSE:
			r = un7z_read(z, &z->zdata, &z->plain);
			switch (r) {

			case FF7ZREAD_FILEDONE: {
				fftime t = z->curfile->mtime;
				t.sec += FFTIME_1970_SECONDS;
				core->file->mtime_set(z->out, t);

				if (!(z->curfile->attr & FFFILE_WIN_DIR)) {
#ifdef FF_WIN
					if (z->curfile->attr != 0)
						core->file->attr_set(z->out, z->curfile->attr);
#endif

					core->file->close(z->out);
					z->del_on_close = 0;
				}
				break;
			}

			case 'done':
				z->state = I_IN; break;

			case 'more':
				z->state = I_FILEREAD; break;

			case FF7ZREAD_FILEHEADER:
				z->state = I_OUT_OPEN; break;

			case FF7ZREAD_DATA:
				z->state = I_WRITE; break;

			case FF7ZREAD_ERROR:
				goto end;
			}
			continue;

		case I_OUT_OPEN: {
			if (z->curfile->attr & FFFILE_WIN_DIR) {
				r = core->file->dir_create(z->oname, FCOM_FILE_DIR_RECURSIVE);
				if (r == FCOM_FILE_ERR) goto end;
				z->state = I_PARSE;
				continue;
			}

			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(z->cmd);
			r = core->file->open(z->out, z->oname, flags);
			if (r == FCOM_FILE_ERR) goto end;

			core->file->trunc(z->out, z->curfile->size);

			z->del_on_close = !z->cmd->stdout && !z->cmd->test;
			z->state = I_PARSE;
			continue;
		}

		case I_WRITE:
			r = core->file->write(z->out, z->plain, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(z->cmd);
				return;
			}

			z->total_uncomp += z->plain.len;
			z->state = I_PARSE;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = z->cmd;
	un7z_close(z);
	core->com->complete(cmd, rc);
	}
}

static void un7z_signal(fcom_op *op, uint signal)
{
	struct un7z *z = op;
	FFINT_WRITEONCE(z->stop, 1);
}

const fcom_operation fcom_op_un7z = {
	un7z_create, un7z_close,
	un7z_run, un7z_signal,
	un7z_help,
};


static void z7_init(const fcom_core *_core) { core = _core; }
static void z7_destroy() {}
static const fcom_operation* z7_provide_op(const char *name)
{
	if (ffsz_eq(name, "un7z"))
		return &fcom_op_un7z;
	return NULL;
}
FF_EXPORT const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	z7_init, z7_destroy, z7_provide_op,
};
