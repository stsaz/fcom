/** fcom: pack files into .iso
2023, Simon Zolin */

static const char* iso_help()
{
	return "\
Pack files into .iso.\n\
Usage:\n\
  `fcom iso` INPUT... [OPTIONS] -o OUTPUT.iso\n\
";
}

#include <fcom.h>
#include <ffpack/isowrite.h>
#include <ffpack/isoread.h>
#include <ffsys/path.h>
#include <ffsys/globals.h>

const fcom_core *core;

struct iso {
	fcom_cominfo cominfo;

	uint st;
	uint stop;
	fcom_cominfo *cmd;

	//input:
	ffstr			iname, base;
	ffstr			plain;
	fcom_file_obj*	in;
	fffileinfo		fi;
	uint			in_file_notfound :1;

	//output:
	ffisowrite		iso;
	ffstr			isodata;
	fcom_file_obj*	out;
	int64			woff;
	uint			del_on_close :1;

	ffvec fnames; // char*[]
	ffsize fnames_i;

	char *volname;
};

#define MIN_COMPRESS_SIZE 32

#define O(member)  FF_OFF(struct iso, member)

static int args_parse(struct iso *c, fcom_cominfo *cmd)
{
	cmd->recursive = 1;

	static const struct ffarg args[] = {
		{}
	};
	int r = core->com->args_parse(cmd, args, c, FCOM_COM_AP_INOUT);
	if (r != 0)
		return r;

	if (cmd->output.len == 0) {
		fcom_fatlog("Use --out to set output file name");
		return -1;
	}

	return 0;
}

#undef O

static void iso_close(fcom_op *op)
{
	struct iso *c = op;
	ffisowrite_close(&c->iso);
	core->file->destroy(c->in);
	if (c->del_on_close)
		core->file->del(c->cmd->output.ptr, 0);
	core->file->destroy(c->out);

	char **it;
	FFSLICE_WALK(&c->fnames, it) {
		ffmem_free(*it);
	}
	ffvec_free(&c->fnames);

	ffmem_free(c->volname);
	ffmem_free(c);
}

static fcom_op* iso_create(fcom_cominfo *cmd)
{
	struct iso *c = ffmem_new(struct iso);
	c->cmd = cmd;

	if (0 != args_parse(c, cmd))
		goto end;

	if (c->volname == NULL) {
		ffstr name;
		ffpath_splitpath_str(cmd->output, NULL, &name);
		ffpath_splitname_str(name, &name, NULL);
		c->volname = ffsz_dupstr(&name);
	}
	if (0 != ffisowrite_create(&c->iso, c->volname, 0))
		goto end;

	struct fcom_file_conf fc = {};
	fcom_cmd_file_conf(&fc, cmd);
	c->in = core->file->create(&fc);
	c->out = core->file->create(&fc);
	return c;

end:
	iso_close(c);
	return NULL;
}


static int iso_file_add(struct iso *c, ffstr name, const fffileinfo *fi)
{
	ffisowrite_fileinfo_t f = {};
	f.name = name;
	f.size = fffileinfo_size(fi);
	f.mtime = fffileinfo_mtime(fi);
	uint a = fffileinfo_attr(fi);
	if (fffile_isdir(a))
		f.attr = ISO_FILE_DIR;
	int r;
	if (0 != (r = ffisowrite_fileadd(&c->iso, &f)))
		fcom_errlog("ffisowrite_fileadd: %s", ffisowrite_error(&c->iso));
	return r;
}

static int iso_write(struct iso *c, ffstr *in, ffstr *out)
{
	for (;;) {
		int r = ffisowrite_process(&c->iso, in, out);
		switch (r) {
		case FFISOWRITE_SEEK:
			c->woff = ffisowrite_offset(&c->iso);
			continue;

		case FFISOWRITE_ERROR:
			fcom_errlog("ffisowrite_process: %s", ffisowrite_error(&c->iso));
		}
		return r;
	}
}

static void iso_run(fcom_op *op)
{
	struct iso *c = op;
	int r, rc = 1;
	enum { I_OUT_OPEN, I_IN, I_INFO, I_IN_NEXT_OPEN, I_FILEREAD, I_PROC, I_WRITE, I_DONE, };

	while (!FFINT_READONCE(c->stop)) {
		switch (c->st) {

		case I_OUT_OPEN: {
			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(c->cmd);
			r = core->file->open(c->out, c->cmd->output.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;
			c->del_on_close = !c->cmd->stdout && !c->cmd->test;
			c->woff = -1;
			c->st = I_IN;
		}
			// fallthrough

		case I_IN:
			if (0 > (r = core->com->input_next(c->cmd, &c->iname, &c->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					c->st = I_PROC;
					continue;
				}
				goto end;
			}

			c->st = I_INFO;
			// fallthrough

		case I_INFO: {
			uint flags = fcom_file_cominfo_flags_i(c->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(c->in, c->iname.ptr, flags);
			if (r == FCOM_FILE_ERR) {
				if (fferr_notexist(fferr_last())) {
					c->in_file_notfound = 1;
					c->st = I_IN;
					continue;
				}
				goto end;
			}

			r = core->file->info(c->in, &c->fi);
			if (r == FCOM_FILE_ERR) goto end;

			if (0 != core->com->input_allowed(c->cmd, c->iname, fffile_isdir(fffileinfo_attr(&c->fi)))) {
				c->st = I_IN;
				continue;
			}

			if ((c->base.len == 0 || c->cmd->recursive)
				&& fffile_isdir(fffileinfo_attr(&c->fi))) {
				fffd fd = core->file->fd(c->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(c->cmd, fd);
			}

			if (!fffile_isdir(fffileinfo_attr(&c->fi)))
				*ffvec_pushT(&c->fnames, char*) = ffsz_dupstr(&c->iname);

			if (0 != iso_file_add(c, c->iname, &c->fi)) goto end;
			c->st = I_IN;
			continue;
		}

		case I_IN_NEXT_OPEN: {
			if (c->fnames_i == c->fnames.len) {
				ffisowrite_finish(&c->iso);
				c->st = I_PROC;
				continue;
			}

			const char *fn = *ffslice_itemT(&c->fnames, c->fnames_i, char*);
			c->fnames_i++;

			uint flags = fcom_file_cominfo_flags_i(c->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(c->in, fn, flags);
			if (r == FCOM_FILE_ERR) goto end;

			ffisowrite_filenext(&c->iso);
			c->st = I_FILEREAD;
		}
			// fallthrough

		case I_FILEREAD:
			r = core->file->read(c->in, &c->plain, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(c->cmd);
				return;
			}
			if (r == FCOM_FILE_EOF) {
				c->st = I_IN_NEXT_OPEN;
				continue;
			}
			c->st = I_PROC;
			// fallthrough

		case I_PROC:
			r = iso_write(c, &c->plain, &c->isodata);
			switch (r) {
			case FFISOWRITE_NEXTFILE:
				c->st = I_IN_NEXT_OPEN; break;

			case FFISOWRITE_MORE:
				c->st = I_FILEREAD; break;

			case FFISOWRITE_DATA:
				c->st = I_WRITE; break;

			case FFISOWRITE_DONE:
				c->st = I_DONE;
				break;

			case FFISOWRITE_ERROR:
				goto end;
			}
			continue;

		case I_WRITE:
			r = core->file->write(c->out, c->isodata, c->woff);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(c->cmd);
				return;
			}
			c->woff = -1;
			c->st = I_PROC;
			continue;

		case I_DONE:
			core->file->close(c->out);
			c->del_on_close = 0;
			rc = c->in_file_notfound;
			goto end;
		}
	}

end:
	{
	fcom_cominfo *cmd = c->cmd;
	iso_close(c);
	core->com->complete(cmd, rc);
	}
}

static void iso_signal(fcom_op *op, uint signal)
{
	struct iso *c = op;
	FFINT_WRITEONCE(c->stop, 1);
}

static const fcom_operation fcom_op_iso = {
	iso_create, iso_close,
	iso_run, iso_signal,
	iso_help,
};


static void iso_init(const fcom_core *_core) { core = _core; }
static void iso_destroy() {}
extern const fcom_operation fcom_op_uniso;
static const fcom_operation* iso_provide_op(const char *name)
{
	if (ffsz_eq(name, "iso"))
		return &fcom_op_iso;
	else if (ffsz_eq(name, "uniso"))
		return &fcom_op_uniso;
	return NULL;
}
FF_EXPORT const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	iso_init, iso_destroy, iso_provide_op,
};
