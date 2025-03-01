/** fcom: Compute MD5 hash
2023, Simon Zolin */

static const char* md5_help()
{
	return "\
Compute MD5 hash.\n\
Usage:\n\
  `fcom md5` INPUT... [OPTIONS] [-o OUTPUT.md5]\n\
\n\
OPTIONS:\n\
    `-c`, `--check`     Read checksums from INPUT files and verify\n\
    `-u`, `--update` FILE.md5\n\
	                    Compute hashes for the missing files only\n\
";
}

#include <fcom.h>
#include <util/md5sum.h>
#include <ops/md5-hash.h>
#include <ffsys/std.h>
#include <ffsys/path.h>
#include <ffsys/globals.h>
#include <ffbase/map.h>

static const fcom_core *core;

struct md5 {
	fcom_cominfo cominfo;

	uint state;
	fcom_cominfo *cmd;
	uint stop;
	ffstr iname;
	ffstr data;
	fcom_file_obj *in, *out;
	fcom_hash_obj *hash;
	uint opened :1;

	ffvec chksum_file_data;
	u_char chksum[16];
	ffstr chksum_current_data, chksum_name;
	uint64 n_processed, n_exist, n_err_format, n_err_io, n_err_verify;

	ffmap map; // hash(filename) => struct map_item

	u_char	verify;
	char*	update_fn;
};

struct map_item {
	u_char sum[16];
	const char *name;
};

#define O(member)  (void*)FF_OFF(struct md5, member)

static int args_parse(struct md5 *m, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{ "--check",		'1',	O(verify) },
		{ "--update",		's',	O(update_fn) },
		{ "-c",				'1',	O(verify) },
		{ "-u",				's',	O(update_fn) },
		{}
	};
	if (core->com->args_parse(cmd, args, m, FCOM_COM_AP_INOUT))
		return -1;

	if (!cmd->output.len)
		cmd->stdout = 1;

	return 0;
}

#undef O

static int map_keyeq(void *opaque, const void *key, ffsize keylen, void *val)
{
	const struct map_item *it = val;
	return !ffs_cmpz(key, keylen, it->name);
}

static int md5_update_read(struct md5 *m)
{
	int rc = 'erro';
	if (fffile_readwhole(m->update_fn, &m->chksum_file_data, 100*1024*1024)) {
		fcom_errlog("file read: %s", m->update_fn);
		goto end;
	}

	ffmap_init(&m->map, map_keyeq);
	ffmap_alloc(&m->map, 1024);

	ffstr s = *(ffstr*)&m->chksum_file_data, name;
	while (s.len) {
		u_char sum[16];
		if (md5sum_read(&s, sum, &name))
			continue;
		name.len = ffpath_normalize(name.ptr, name.len, name.ptr, name.len, 0);
		name.ptr[name.len] = '\0';
		struct map_item *it = ffmem_new(struct map_item);
		it->name = name.ptr;
		ffmem_copy(it->sum, sum, 16);
		ffmap_add(&m->map, name.ptr, name.len, it);
	}

	rc = 0;

end:
	return rc;
}

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

	if (!m->opened) {
		m->opened = 1;
		uint f = fcom_file_cominfo_flags_o(m->cmd);
		f |= FCOM_FILE_WRITE | FCOM_FILE_NOCACHE;
		r = core->file->open(m->out, m->cmd->output.ptr, f);
		if (r == FCOM_FILE_ERR) return 'erro';
	}

	if (m->update_fn) {
		const struct map_item *it = ffmap_find(&m->map, m->iname.ptr, m->iname.len, NULL);
		if (it) {
			r = core->file->write_fmt(m->out, "%*xb *%S\n", (ffsize)16, it->sum, &m->iname);
			if (r == FCOM_FILE_ERR) return 'erro';
			m->n_exist++;
			return 'skip';
		}
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

static void md5_close(fcom_op *op)
{
	struct md5 *m = op;
	fcom_md5.close(m->hash);
	core->file->destroy(m->in);
	if (m->out)
		core->file->close(m->out);
	core->file->destroy(m->out);
	ffvec_free(&m->chksum_file_data);

	struct _ffmap_item *it;
	FFMAP_WALK(&m->map, it) {
		if (_ffmap_item_occupied(it))
			ffmem_free(it->val);
	}
	ffmap_free(&m->map);

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

static int md5_chksum_file_read(struct md5 *m)
{
	int r;
	ffstr base;
	if (0 > (r = core->com->input_next(m->cmd, &m->iname, &base, 0))) {
		if (r == FCOM_COM_RINPUT_NOMORE) {
			return 'done';
		}
		return 'erro';
	}

	fcom_dbglog("opening %s", m->iname.ptr);
	m->chksum_file_data.len = 0;
	if (fffile_readwhole(m->iname.ptr, &m->chksum_file_data, 100*1024*1024)) {
		fcom_errlog("file read: %s", m->iname.ptr);
		return 'skip';
	}

	m->chksum_current_data = *(ffstr*)&m->chksum_file_data;
	return 0;
}

static int md5_chksum_next(struct md5 *m)
{
	if (!m->chksum_current_data.len)
		return 'done';
	if (md5sum_read(&m->chksum_current_data, m->chksum, &m->chksum_name)) {
		fcom_warnlog("%s: bad .md5 line format", m->iname.ptr);
		return 'erro';
	}
	m->chksum_name.ptr[m->chksum_name.len] = '\0';
	return 0;
}

static int md5_file_open(struct md5 *m)
{
	uint flags = fcom_file_cominfo_flags_i(m->cmd);
	flags |= FCOM_FILE_READ;
	int r = core->file->open(m->in, m->chksum_name.ptr, flags);
	if (r == FCOM_FILE_ERR) return 'erro';

	fffileinfo fi = {};
	if (fffile_isdir(fffileinfo_attr(&fi))) {
		fcom_warnlog("%s: file is a directory", m->chksum_name.ptr);
		return 'erro';
	}

	m->hash = fcom_md5.create();
	return 0;
}

static int md5_file_process(struct md5 *m)
{
	int r = core->file->read(m->in, &m->data, -1);
	if (r == FCOM_FILE_ERR) return 'erro';
	if (r == FCOM_FILE_EOF) return 'done';

	fcom_md5.update(m->hash, m->data.ptr, m->data.len);
	return 0;
}

static int md5_file_verify(struct md5 *m)
{
	u_char result[16];
	fcom_md5.fin(m->hash, result, 16);
	int r = 1;
	if (!ffmem_cmp(result, m->chksum, 16)) {
		r = 0;
		if (core->verbose)
			fcom_infolog("%s: OK", m->chksum_name.ptr);
	} else {
		fcom_warnlog("%s: FAIL", m->chksum_name.ptr);
	}
	fcom_md5.close(m->hash);  m->hash = NULL;
	return r;
}

static void md5_run(fcom_op *op)
{
	struct md5 *m = op;
	int rc = 1, err = 0;
	enum {
		I_IN, I_MD5OPEN, I_READ,
		I_VERIFY_OPEN, I_VERIFY_NEXT, I_VERIFY_FILE_OPEN, I_VERIFY_FILE_READ,
	};

	if (m->verify)
		m->state = I_VERIFY_OPEN;
	else if (m->update_fn)
		m->state = I_MD5OPEN;

	while (!FFINT_READONCE(m->stop)) {
		switch (m->state) {
		case I_MD5OPEN:
			switch (md5_update_read(m)) {
			case 'erro':
				goto end;
			}
			m->state = I_IN;
			// fallthrough

		case I_IN:
			switch (md5_open(m)) {
			case 'done':
				rc = err;
				if (m->update_fn) {
					ffstderr_fmt("md5: files processed:%U  existing:%U"
						, m->n_processed, m->n_exist);
				}
				goto end;
			case 'skip':
				continue;
			case 'erro':
				err = 1;
				continue;
			}
			m->state = I_READ;
			// fallthrough

		case I_READ:
			switch (md5_process(m)) {
			case 'done':
				if (md5_result(m)) goto end;
				m->n_processed++;
				m->state = I_IN;
				continue;
			case 'erro':
				err = 1;
				m->state = I_IN;
				continue;
			}
			continue;


		case I_VERIFY_OPEN:
			switch (md5_chksum_file_read(m)) {
			case 'skip':
				m->n_err_io++;
				continue;
			case 'done':
				if (!(m->n_err_format | m->n_err_io | m->n_err_verify))
					rc = 0;
				fcom_infolog("md5: files processed:%U  failed:%U  error:%U"
					, m->n_processed, m->n_err_verify, m->n_err_format + m->n_err_io);
				goto end;
			case 'erro':
				goto end;
			}
			m->state = I_VERIFY_NEXT;
			// fallthrough

		case I_VERIFY_NEXT:
			switch (md5_chksum_next(m)) {
			case 'done':
				m->state = I_VERIFY_OPEN;
				continue;
			case 'erro':
				m->n_err_format++;
				continue;
			}
			m->state = I_VERIFY_FILE_OPEN;
			// fallthrough

		case I_VERIFY_FILE_OPEN:
			switch (md5_file_open(m)) {
			case 'erro':
				m->n_err_io++;
				m->state = I_VERIFY_NEXT;
				continue;
			}
			m->state = I_VERIFY_FILE_READ;
			// fallthrough

		case I_VERIFY_FILE_READ:
			switch (md5_file_process(m)) {
			case 0:
				continue;
			case 'done':
				m->n_processed++;
				if (md5_file_verify(m))
					m->n_err_verify++;
				break;
			case 'erro':
				m->n_err_io++;
				break;
			}
			m->state = I_VERIFY_NEXT;
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
