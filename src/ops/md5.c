/** fcom: Compute MD5 hash
2023, Simon Zolin */

static const char* md5_help()
{
	return "\
Compute MD5 hash.\n\
Usage:\n\
  `fcom md5` INPUT... [OPTIONS] [-o OUTPUT]\n\
";
}

#include <fcom.h>
#include <ops/md5-hash.h>
#include <ffsys/globals.h>

static const fcom_core *core;

struct md5 {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	uint stop;
	ffstr iname;
	ffstr data;
	fcom_file_obj *in, *out;
	fcom_hash_obj *hash;

	uint opened :1;
};

static int md5_open(struct md5 *m)
{
	int r;
	ffstr base;
	if (0 > (r = core->com->input_next(m->cmd, &m->iname, &base, 0))) {
		if (r == FCOM_COM_RINPUT_NOMORE) {
			return 'done';
		}
		return 'erro';
	}

	uint flags = fcom_file_cominfo_flags_i(m->cmd);
	flags |= FCOM_FILE_READ;
	r = core->file->open(m->in, m->iname.ptr, flags);
	if (r == FCOM_FILE_ERR) return 'erro';

	fffileinfo fi;
	r = core->file->info(m->in, &fi);
	if (r == FCOM_FILE_ERR) return 'erro';

	if (core->com->input_allowed(m->cmd, m->iname, fffile_isdir(fffileinfo_attr(&fi))))
		return 'skip';

	if (fffile_isdir(fffileinfo_attr(&fi))) {
		fffd fd = core->file->fd(m->in, FCOM_FILE_ACQUIRE);
		core->com->input_dir(m->cmd, fd);
		return 'skip';
	}

	m->hash = fcom_md5.create();
	return 0;
}

static int md5_process(struct md5 *m)
{
	int r = core->file->read(m->in, &m->data, -1);
	if (r == FCOM_FILE_ERR) return -1;
	if (r == FCOM_FILE_EOF) {
		return 'done';
	}

	if (!m->opened) {
		m->opened = 1;
		uint f = fcom_file_cominfo_flags_o(m->cmd);
		f |= FCOM_FILE_WRITE;
		r = core->file->open(m->out, m->cmd->output.ptr, f);
		if (r == FCOM_FILE_ERR) return 'erro';
	}

	fcom_md5.update(m->hash, m->data.ptr, m->data.len);
	return 0;
}

static int md5_result(struct md5 *m)
{
	byte result[16];
	fcom_md5.fin(m->hash, result, sizeof(result));
	int r = core->file->write_fmt(m->out, "%*xb *%S\n", (ffsize)16, result, &m->iname);
	if (r == FCOM_FILE_ERR) return -1;

	fcom_md5.close(m->hash);  m->hash = NULL;
	return 0;
}

#define O(member)  FF_OFF(struct md5, member)

static int args_parse(struct md5 *m, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{}
	};
	if (0 != core->com->args_parse(cmd, args, m, FCOM_COM_AP_INOUT))
		return -1;

	if (!cmd->output.len)
		cmd->stdout = 1;

	return 0;
}

#undef O

static void md5_close(fcom_op *op)
{
	struct md5 *m = op;
	fcom_md5.close(m->hash);
	core->file->destroy(m->in);
	if (m->out)
		core->file->close(m->out);
	core->file->destroy(m->out);
	ffmem_free(m);
}

static fcom_op* md5_create(fcom_cominfo *cmd)
{
	struct md5 *m = ffmem_new(struct md5);

	if (0 != args_parse(m, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	m->in = core->file->create(&fc);
	m->out = core->file->create(&fc);

	m->cmd = cmd;
	return m;

end:
	md5_close(m);
	return NULL;
}

static void md5_run(fcom_op *op)
{
	struct md5 *m = op;
	int rc = 1, err = 0;
	enum { I_IN, I_READ, };

	while (!FFINT_READONCE(m->stop)) {
		switch (m->st) {
		case I_IN:
			switch (md5_open(m)) {
			case 'done':
				rc = err;
				goto end;
			case 'skip':
				continue;
			case 'erro':
				err = 1;
				continue;
			}
			m->st = I_READ;
			// fallthrough

		case I_READ:
			switch (md5_process(m)) {
			case 'done':
				if (md5_result(m)) goto end;
				m->st = I_IN;
				continue;
			case 'erro':
				err = 1;
				m->st = I_IN;
				continue;
			}
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = m->cmd;
	md5_close(m);
	core->com->complete(cmd, rc);
	}
}

static void md5_signal(fcom_op *op, uint signal)
{
	struct md5 *m = op;
	FFINT_WRITEONCE(m->stop, 1);
}

static const fcom_operation fcom_op_md5 = {
	md5_create, md5_close,
	md5_run, md5_signal,
	md5_help,
};


static void md5m_init(const fcom_core *_core) { core = _core; }
static void md5m_destroy(){}
static const fcom_operation* md5m_provide_op(const char *name)
{
	if (ffsz_eq(name, "md5"))
		return &fcom_op_md5;
	return NULL;
}
FF_EXPORT struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	md5m_init, md5m_destroy, md5m_provide_op,
};
