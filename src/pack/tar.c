/** fcom: pack files into .tar
2023, Simon Zolin */

static const char* tar_help()
{
	return "\
Pack files into .tar.\n\
Usage:\n\
  `fcom tar` INPUT... [OPTIONS] -o OUTPUT.tar\n\
";
}

#include <fcom.h>
#include <ffpack/tarwrite.h>
#include <ffsys/globals.h>

const fcom_core *core;

struct tar {
	fcom_cominfo cominfo;

	uint st;
	uint stop;
	fcom_cominfo *cmd;

	ffstr			iname, base;
	fcom_file_obj*	in;
	fffileinfo		fi;
	ffstr			plain;
	uint			link :1;

	fftarwrite		wtar;
	ffstr			tardata;
	fcom_file_obj*	out;
	uint			del_on_close :1;
};

#define O(member)  FF_OFF(struct tar, member)

static int args_parse(struct tar *t, fcom_cominfo *cmd)
{
	cmd->recursive = 1;

	static const struct ffarg args[] = {
		{}
	};
	int r = core->com->args_parse(cmd, args, t, FCOM_COM_AP_INOUT);
	if (r != 0)
		return r;

	if (cmd->output.len == 0 && !cmd->stdout) {
		fcom_fatlog("Use --out to set output file name");
		return -1;
	}

	return 0;
}

#undef O

static void tar_close(fcom_op *op)
{
	struct tar *t = op;
	fftarwrite_destroy(&t->wtar);
	core->file->destroy(t->in);
	if (t->del_on_close)
		core->file->del(t->cmd->output.ptr, 0);
	core->file->destroy(t->out);
	ffmem_free(t);
}

static fcom_op* tar_create(fcom_cominfo *cmd)
{
	struct tar *t = ffmem_new(struct tar);
	t->cmd = cmd;

	if (0 != args_parse(t, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fcom_cmd_file_conf(&fc, cmd);
	t->in = core->file->create(&fc);
	t->out = core->file->create(&fc);
	return t;

end:
	tar_close(t);
	return NULL;
}

static int tar_file_add(struct tar *t)
{
	fftarwrite_conf conf = {};

	conf.type = TAR_FILE;
	if (t->link)
		conf.type = TAR_SLINK;

	conf.name = t->iname;
	conf.mtime = fffileinfo_mtime(&t->fi);
	conf.size = fffileinfo_size(&t->fi);

#ifdef FF_UNIX
	conf.attr_unix = fffileinfo_attr(&t->fi);
	conf.uid = t->fi.st_uid;
	conf.gid = t->fi.st_gid;
#endif

#ifdef FF_UNIX
	char link_tgt[4096];
	if (t->link) {
		int r = readlink(t->iname.ptr, link_tgt, sizeof(link_tgt));
		if (r == sizeof(link_tgt)) {
			fcom_errlog("readlink: %s: truncation", t->iname.ptr);
			return -1;
		} else if (r < 0) {
			fcom_syserrlog("readlink: %s", t->iname.ptr);
			return -1;
		}
		ffstr_set(&conf.link_to, link_tgt, r);

		conf.size = 0;
	}
#endif

	int r;
	if (0 != (r = fftarwrite_fileadd(&t->wtar, &conf))) {
		if (r == -2)
			return -2;
		fcom_errlog("fftarwrite_fileadd: %s", fftarwrite_error(&t->wtar));
		return -1;
	}

	return 0;
}

static int tar_write(struct tar *t, ffstr *in, ffstr *out)
{
	for (;;) {
		int r = fftarwrite_process(&t->wtar, in, out);
		switch (r) {

		case FFTARWRITE_FILEDONE:
			fcom_verblog("%s", t->iname.ptr);
			break;

		case FFTARWRITE_ERROR:
			fcom_errlog("fftarwrite_process: %s", fftarwrite_error(&t->wtar));
		}
		return r;
	}
}

static void tar_run(fcom_op *op)
{
	struct tar *t = op;
	int r, rc = 1;
	enum { I_OUT_OPEN, I_IN, I_INFO, I_ADD, I_FILEREAD, I_PROC, I_WRITE, I_DONE, };

	while (!FFINT_READONCE(t->stop)) {
		switch (t->st) {

		case I_OUT_OPEN: {
			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(t->cmd);
			r = core->file->open(t->out, t->cmd->output.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;
			t->del_on_close = !t->cmd->stdout && !t->cmd->test;
			t->st = I_IN;
		}
			// fallthrough

		case I_IN:
			if (0 > (r = core->com->input_next(t->cmd, &t->iname, &t->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					fftarwrite_finish(&t->wtar);
					t->st = I_PROC;
					continue;
				}
				goto end;
			}

			t->st = I_INFO;
			// fallthrough

		case I_INFO: {
			uint flags = fcom_file_cominfo_flags_i(t->cmd);
			flags |= FCOM_FILE_READ;
			flags |= FCOM_FILE_INFO_NOFOLLOW;
			r = core->file->open(t->in, t->iname.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;

			r = core->file->info(t->in, &t->fi);
			if (r == FCOM_FILE_ERR) goto end;

			if (0 != core->com->input_allowed(t->cmd, t->iname, fffile_isdir(fffileinfo_attr(&t->fi)))) {
				t->st = I_IN;
				continue;
			}

			if ((t->base.len == 0 || t->cmd->recursive)
				&& fffile_isdir(fffileinfo_attr(&t->fi))) {
				fffd fd = core->file->fd(t->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(t->cmd, fd);
			}

			t->link = 0;
#ifdef FF_UNIX
			t->link = (S_IFLNK == (fffileinfo_attr(&t->fi) & S_IFMT));
#endif

			t->st = I_ADD;
			continue;
		}

		case I_ADD:
			if (0 != (r = tar_file_add(t))) {
				if (r == -2) {
					t->st = I_IN;
					continue;
				}
				goto end;
			}

			if (fffile_isdir(fffileinfo_attr(&t->fi)) || t->link) {
				fftarwrite_filefinish(&t->wtar);
				t->st = I_PROC;
				continue;
			}

			t->st = I_FILEREAD;
			// fallthrough

		case I_FILEREAD:
			r = core->file->read(t->in, &t->plain, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(t->cmd);
				return;
			}
			if (r == FCOM_FILE_EOF)
				fftarwrite_filefinish(&t->wtar);
			t->st = I_PROC;
			// fallthrough

		case I_PROC:
			r = tar_write(t, &t->plain, &t->tardata);
			switch (r) {
			case FFTARWRITE_MORE:
				t->st = I_FILEREAD; break;

			case FFTARWRITE_DATA:
				t->st = I_WRITE; break;

			case FFTARWRITE_FILEDONE:
				t->st = I_IN; break;

			case FFTARWRITE_DONE:
				t->st = I_DONE;
				break;

			case FFTARWRITE_ERROR:
				goto end;
			}
			continue;

		case I_WRITE:
			r = core->file->write(t->out, t->tardata, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(t->cmd);
				return;
			}
			t->st = I_PROC;
			continue;

		case I_DONE:
			core->file->close(t->out);
			t->del_on_close = 0;
			rc = 0;
			goto end;
		}
	}

end:
	{
	fcom_cominfo *cmd = t->cmd;
	tar_close(t);
	core->com->complete(cmd, rc);
	}
}

static void tar_signal(fcom_op *op, uint signal)
{
	struct tar *t = op;
	FFINT_WRITEONCE(t->stop, 1);
}

static const fcom_operation fcom_op_tar = {
	tar_create, tar_close,
	tar_run, tar_signal,
	tar_help,
};

static void tar_init(const fcom_core *_core) { core = _core; }
static void tar_destroy() {}
extern const fcom_operation fcom_op_untar;
static const fcom_operation* tar_provide_op(const char *name)
{
	if (ffsz_eq(name, "tar"))
		return &fcom_op_tar;
	else if (ffsz_eq(name, "untar"))
		return &fcom_op_untar;
	return NULL;
}
FF_EXPORT const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	tar_init, tar_destroy, tar_provide_op,
};
