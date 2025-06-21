/** fcom: unpack files from .iso
2023, Simon Zolin */

#include <ffsys/std.h>

static const char* uniso_help()
{
	return "\
Unpack files from .iso.\n\
Usage:\n\
  `fcom uniso` INPUT... [-C OUTPUT_DIR]\n\
\n\
OPTIONS:\n\
    `-m`, `--members-from-file` FILE\n\
                    Read archive member names from file\n\
    `-l`, `--list`      Just show the file list\n\
        `--autodir`   Add to OUTPUT_DIR a directory with name = input archive name.\n\
                     Same as manual 'uniso arc.iso -C odir/arc'.\n\
";
}

#include <fcom.h>
#include <ffpack/iso-read.h>
#include <ffsys/path.h>
#include <ffbase/map.h>

extern const fcom_core *core;

struct file {
	uint64 off;
	uint attr_unix, attr_win;
	uint64 zsize, size;
	fftime mtime;
};

struct uniso {
	fcom_cominfo cominfo;

	uint state;
	fcom_cominfo *cmd;
	ffisoread iso;
	ffstr iname, base;
	ffstr isodata, plain;
	fcom_file_obj *in, *out;
	char *oname;
	ffvec buf;
	const ffisoread_fileinfo_t *curfile;
	uint stop;
	uint del_on_close :1;
	uint64 total_comp, total_uncomp;
	uint64 roff;

	ffvec files; // struct file[]
	ffsize ifile;

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

static int uniso_args_members_from_file(void *obj, char *fn)
{
	struct uniso *c = obj;

	if (0 != fffile_readwhole(fn, &c->members_data, 100*1024*1024))
		return 1;
	ffvec_addchar(&c->members_data, '\n');

	uint n = 0;
	ffstr d = FFSTR_INITSTR(&c->members_data);
	while (d.len != 0) {
		ffstr ln;
		ffstr_splitby(&d, '\n', &ln, &d);
		if (ln.len == 0)
			continue;

		n++;
	}

	ffmap_init(&c->members, members_keyeq);
	ffmap_alloc(&c->members, n);

	ffstr_setstr(&d, &c->members_data);
	while (d.len != 0) {
		ffstr ln;
		ffstr_splitby(&d, '\n', &ln, &d);
		if (ln.len == 0)
			continue;

		ffmap_add(&c->members, ln.ptr, ln.len, ln.ptr);
	}
	return 0;
}

#define O(member)  (void*)FF_OFF(struct uniso, member)

static int uniso_args_parse(struct uniso *c, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--autodir",				'1',	O(autodir) },
		{ "--list",					'1',	O(list) },
		{ "--members-from-file",	's',	uniso_args_members_from_file },
		{ "-l", 					'1',	O(list) },
		{ "-m",						's',	uniso_args_members_from_file },
		{}
	};
	int r = core->com->args_parse(cmd, args, c, FCOM_COM_AP_INOUT);
	if (r != 0)
		return r;

	if (!c->list && cmd->output.len != 0) {
		fcom_fatlog("Use -C to set output directory");
		return -1;
	}

	if (cmd->chdir.len == 0)
		ffstr_dupz(&cmd->chdir, ".");

	return 0;
}

#undef O

static void uniso_log(void *udata, uint level, ffstr msg)
{
	fcom_dbglog("%S", &msg);
}

static void uniso_close(fcom_op *op)
{
	struct uniso *c = op;
	ffisoread_close(&c->iso);
	core->file->destroy(c->in);
	if (c->del_on_close)
		core->file->del(c->oname, 0);
	core->file->destroy(c->out);
	ffvec_free(&c->files);
	ffvec_free(&c->buf);
	ffmem_free(c->oname);
	ffvec_free(&c->members_data);
	ffmap_free(&c->members);
	ffmem_free(c);
}

static fcom_op* uniso_create(fcom_cominfo *cmd)
{
	struct uniso *c = ffmem_new(struct uniso);
	c->cmd = cmd;

	if (0 != uniso_args_parse(c, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fcom_cmd_file_conf(&fc, cmd);
	c->in = core->file->create(&fc);
	c->out = core->file->create(&fc);

	ffsize cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&c->buf, cap, 1);
	return c;

end:
	uniso_close(c);
	return NULL;
}

/* "size zsize(%) date name" */
static void showinfo(struct uniso *c, const ffisoread_fileinfo_t *f)
{
	ffvec *b = &c->buf;
	b->len = 0;
	if (f->attr & ISO_FILE_DIR) {
		ffvec_addsz(b, "       <DIR>");
	} else {
		ffvec_addfmt(b, "%12U", f->size);
	}
	ffvec_addchar(b, ' ');

	ffdatetime dt;
	fftime m = f->mtime;
	m.sec += FFTIME_1970_SECONDS + core->tz.real_offset;

	fftime_split1(&dt, &m);
	b->len += fftime_tostr1(&dt, ffslice_end(b, 1), ffvec_unused(b), FFTIME_DATE_YMD | FFTIME_HMS);
	ffvec_addchar(b, ' ');

	ffvec_addstr(b, &f->name);
	ffvec_addchar(b, '\n');
	ffstdout_write(b->ptr, b->len);
}

/*
`d/f` -> `out/d/f`
`d/f` -> `out/iname/d/f` (--autodir)
*/
static char* outname(struct uniso *c, ffstr lname, ffstr rpath)
{
	if (c->autodir) {
		ffstr name;
		ffpath_splitpath_str(c->iname, NULL, &name);
		ffpath_splitname_str(name, &name, NULL);
		return ffsz_allocfmt("%S%c%S%c%S%Z", &rpath, FFPATH_SLASH, &name, FFPATH_SLASH, &lname);
	}
	return ffsz_allocfmt("%S%c%S%Z", &rpath, FFPATH_SLASH, &lname);
}

static int uniso_read(struct uniso *c, ffstr *input, ffstr *output)
{
	for (;;) {
		int r = ffisoread_process(&c->iso, input, output);
		switch ((enum FFISOREAD_R)r) {

		case FFISOREAD_HDR:
			continue;

		case FFISOREAD_FILEMETA: {
			ffisoread_fileinfo_t *f = ffisoread_fileinfo(&c->iso);

			if (c->members.len != 0
				&& 0 == members_find(&c->members, f->name))
				continue;

			if (0 != ffisoread_storefile(&c->iso))
				return FFISOREAD_ERROR;

			if (c->list) {
				showinfo(c, f);
			}
			continue;
		}

		case FFISOREAD_LISTEND:
		case FFISOREAD_DATA:
		case FFISOREAD_FILEDONE:
			return r;

		case FFISOREAD_SEEK:
			c->roff = ffisoread_offset(&c->iso);
			// fallthrough
		case FFISOREAD_MORE:
			return r;

		case FFISOREAD_ERROR:
			fcom_errlog("%s  offset:0x%xU", ffisoread_error(&c->iso), c->roff);
			return FFISOREAD_ERROR;
		}
	}
}

static void uniso_run(fcom_op *op)
{
	struct uniso *c = op;
	int r, rc = 1;
	enum { I_IN, I_INFO, I_PARSE, I_FILEREAD, I_OUT_OPEN, I_WRITE };

	while (!FFINT_READONCE(c->stop)) {
		switch (c->state) {

		case I_IN:
			if (0 > (r = core->com->input_next(c->cmd, &c->iname, &c->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					rc = 0;
				}
				goto end;
			}

			c->state = I_INFO;
			// fallthrough

		case I_INFO: {
			uint flags = fcom_file_cominfo_flags_i(c->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(c->in, c->iname.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;

			fffileinfo fi = {};
			r = core->file->info(c->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;

			if (0 != core->com->input_allowed(c->cmd, c->iname, fffile_isdir(fffileinfo_attr(&fi)))) {
				c->state = I_IN;
				continue;
			}

			if ((c->base.len == 0 || c->cmd->recursive)
					&& fffile_isdir(fffileinfo_attr(&fi))) {
				fffd fd = core->file->fd(c->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(c->cmd, fd);
			}

			ffmem_zero_obj(&c->iso);
			ffisoread_open(&c->iso);
			c->iso.log = uniso_log;
			c->iso.udata = c;

			c->state = I_PARSE;
			continue;
		}

		case I_FILEREAD:
			r = core->file->read(c->in, &c->isodata, c->roff);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(c->cmd);
				return;
			}
			if (r == FCOM_FILE_EOF) {
				fcom_errlog("incomplete archive");
				goto end;
			}
			c->roff += c->isodata.len;
			c->state = I_PARSE;
			// fallthrough

		case I_PARSE:
			r = uniso_read(c, &c->isodata, &c->plain);
			switch (r) {

			case FFISOREAD_LISTEND:
				if (c->list) {
					c->state = I_IN;
					continue;
				}
				c->state = I_OUT_OPEN; break;

			case FFISOREAD_FILEDONE: {
				fftime t = c->curfile->mtime;
				t.sec += FFTIME_1970_SECONDS;
				core->file->mtime_set(c->out, t);

				if (!(c->curfile->attr & ISO_FILE_DIR)) {
					core->file->close(c->out);
					c->del_on_close = 0;
					fcom_verblog("%s", c->oname);
				}
				c->state = I_OUT_OPEN;
				break;
			}

			case FFISOREAD_MORE:
			case FFISOREAD_SEEK:
				c->state = I_FILEREAD; break;

			case FFISOREAD_DATA:
				c->state = I_WRITE; break;

			case FFISOREAD_ERROR:
				goto end;
			}
			continue;

		case I_OUT_OPEN: {
			ffisoread_fileinfo_t *f;
			if (NULL == (f = ffisoread_nextfile(&c->iso))) {
				c->state = I_IN;
				continue;
			}
			c->curfile = f;
			ffisoread_readfile(&c->iso, f);

			ffmem_free(c->oname);
			c->oname = outname(c, f->name, c->cmd->chdir);

			if (f->attr & ISO_FILE_DIR) {
				r = core->file->dir_create(c->oname, FCOM_FILE_DIR_RECURSIVE);
				if (r == FCOM_FILE_ERR) goto end;
				c->state = I_PARSE;
				continue;
			}

			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(c->cmd);
			r = core->file->open(c->out, c->oname, flags);
			if (r == FCOM_FILE_ERR) goto end;

			core->file->trunc(c->out, f->size);

			c->del_on_close = !c->cmd->stdout && !c->cmd->test;
			c->state = I_PARSE;
			continue;
		}

		case I_WRITE:
			r = core->file->write(c->out, c->plain, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(c->cmd);
				return;
			}

			c->total_uncomp += c->plain.len;
			c->state = I_PARSE;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = c->cmd;
	uniso_close(c);
	core->com->complete(cmd, rc);
	}
}

static void uniso_signal(fcom_op *op, uint signal)
{
	struct uniso *c = op;
	FFINT_WRITEONCE(c->stop, 1);
}

const fcom_operation fcom_op_uniso = {
	uniso_create, uniso_close,
	uniso_run, uniso_signal,
	uniso_help,
};
