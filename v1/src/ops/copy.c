/** fcom: copy data (input -> output)
2022, Simon Zolin */

#include <fcom.h>
#include <FFOS/path.h>

static const fcom_core *core;

#define ffmem_free0(p)  ffmem_free(p), (p) = NULL

#define BUF_LARGE  (8*1024*1024)

struct copy {
	uint st;
	fcom_file_obj *in, *out;
	ffstr data;
	uint64 total, in_off, out_off;
	uint nfiles;
	char *iname, *oname;
	fcom_cominfo *cmd;
	byte preserve_date;
	byte rename_source;
	uint stop;

	ffstr encrypt, decrypt;
	fcom_aes_obj *aes_obj;
	const fcom_aes *aes;
	byte aes_iv[16];
	ffstr aes_in;
	ffvec aes_buf;
	const fcom_hash *sha256, *sha1;

	byte verify;
	const fcom_hash *md5;
	fcom_hash_obj *md5_obj;
	byte md5_result_r[16];
};

static int args_parse(struct copy *c, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{ 'e',	"encrypt",	FFCMDARG_TSTR | FFCMDARG_FNOTEMPTY, FF_OFF(struct copy, encrypt) },
		{ 'd',	"decrypt",	FFCMDARG_TSTR | FFCMDARG_FNOTEMPTY, FF_OFF(struct copy, decrypt) },
		{ 'y',	"verify",	FFCMDARG_TSWITCH, FF_OFF(struct copy, verify) },
		{ 0,	"rename-source",	FFCMDARG_TSWITCH, FF_OFF(struct copy, rename_source) },
		{}
	};
	return core->com->args_parse(cmd, args, c);
}

static const char* copy_help()
{
	return "\
Copy files from one place to another, plus encryption & verification.\n\
Implies '--recursive'.\n\
Uses large '--buffer' by default.\n\
Usage:\n\
  fcom copy INPUT... [-o OUTPUT_FILE] [-C OUTPUT_DIR] [OPTIONS]\n\
    OPTIONS:\n\
    -e, --encrypt=PASSWORD\n\
                        Encrypt data (AES256CFB key=SHA256(password) iv=SHA1(key))\n\
    -d, --decrypt=PASSWORD\n\
                        Decrypt data\n\
    -y, --verify        Verify data consistency with MD5.\n\
                        Implies '--directio' on output file.\n\
                        Prints hash sums with '--verbose'.\n\
        --rename-source\n\
                        Rename source file to *.deleted after successful operation\n\
";
}

static void copy_close(fcom_op *op);

static int crypt_init(struct copy *c)
{
	if (c->encrypt.len != 0 && c->decrypt.len != 0) {
		fcom_errlog("both --encrypt and --decrypt can't be together");
		return -1;
	}

	if (NULL == (c->sha256 = core->com->provide("crypto.sha256", 0)))
		return -1;
	if (NULL == (c->sha1 = core->com->provide("crypto.sha1", 0)))
		return -1;

	const char *opname = (c->encrypt.len != 0) ? "crypto.aes_encrypt" : "crypto.aes_decrypt";
	if (NULL == (c->aes = core->com->provide(opname, 0)))
		return -1;

	ffvec_alloc(&c->aes_buf, 64*1024, 1);
	return 0;
}

#define SHA1_LENGTH 20

static void sha1_hash(struct copy *c, const void *data, ffsize size, byte result[SHA1_LENGTH])
{
	fcom_hash_obj *h = c->sha1->create();
	c->sha1->update(h, data, size);
	c->sha1->fin(h, result, SHA1_LENGTH);
	c->sha1->close(h);
}

static void sha256_hash(struct copy *c, const void *data, ffsize size, byte result[32])
{
	fcom_hash_obj *h = c->sha256->create();
	c->sha256->update(h, data, size);
	c->sha256->fin(h, result, 32);
	c->sha256->close(h);
}

static int crypt_open(struct copy *c)
{
	ffstr pw = (c->encrypt.len != 0) ? c->encrypt : c->decrypt;
	byte key[32];
	sha256_hash(c, pw.ptr, pw.len, key);
	byte sha1[SHA1_LENGTH];
	sha1_hash(c, key, 16, sha1);
	ffmem_copy(c->aes_iv, sha1, 16);
	if (NULL == (c->aes_obj = c->aes->create(key, 32, FCOM_AES_CFB)))
		return -1;
	ffmem_zero_obj(key);
	return 0;
}

static fcom_op* copy_create(fcom_cominfo *cmd)
{
	struct copy *c = ffmem_new(struct copy);
	c->preserve_date = 1;
	if (cmd->buffer_size == 0)
		cmd->buffer_size = BUF_LARGE;

	if (0 != args_parse(c, cmd))
		goto end;
	if (cmd->recursive != 0xff)
		cmd->recursive = 1;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	fc.n_buffers = 1;
	c->in = core->file->create(&fc);
	c->out = core->file->create(&fc);

	if (c->encrypt.len != 0 || c->decrypt.len != 0) {
		if (0 != crypt_init(c))
			goto end;
	}

	if (c->verify) {
		if (cmd->stdout) {
			fcom_errlog("STDOUT output can't be used with --verify");
			goto end;
		}
		if (NULL == (c->md5 = core->com->provide("crypto.md5", 0)))
			goto end;
	}

	c->cmd = cmd;
	return c;

end:
	copy_close(c);
	return NULL;
}

static void copy_reset(struct copy *c)
{
	c->total = 0;
	c->in_off = c->out_off = 0;

	ffstr_free(&c->encrypt);
	ffstr_free(&c->decrypt);
	ffvec_free(&c->aes_buf);
	if (c->aes_obj != NULL) {
		c->aes->close(c->aes_obj),  c->aes_obj = NULL;
	}

	if (c->md5_obj != NULL) {
		c->md5->close(c->md5_obj),  c->md5_obj = NULL;
	}

	ffmem_free0(c->iname);
	ffmem_free0(c->oname);
}

static void copy_close(fcom_op *op)
{
	struct copy *c = op;
	core->file->destroy(c->in);
	core->file->destroy(c->out);
	ffmem_zero(c->encrypt.ptr, c->encrypt.len);
	ffmem_zero(c->decrypt.ptr, c->decrypt.len);
	copy_reset(c);
	ffmem_free(c);
}

static void copy_signal(fcom_op *op, uint signal)
{
	struct copy *c = op;
	FFINT_WRITEONCE(c->stop, 1);
}

static int verify_result(struct copy *c)
{
	byte result_w[16];
	c->md5->fin(c->md5_obj, result_w, 16);

	const char *iname = c->iname;
	if (c->aes_obj == NULL) {
		fcom_verblog("%*xb *%s", (ffsize)16, c->md5_result_r, c->iname);
		iname = "should be";
	}
	fcom_verblog("%*xb *%s", (ffsize)16, result_w, c->oname);

	if (0 != ffmem_cmp(c->md5_result_r, result_w, 16)) {
		fcom_errlog("MD5 verification failed.  '%s': %*xb  '%s': %*xb"
			, iname, (ffsize)16, c->md5_result_r
			, c->oname, (ffsize)16, result_w);
		return 1;
	}
	return 0;
}

static char* out_name(struct copy *c, ffstr in, ffstr base)
{
	char *s;
	if (c->cmd->output.len != 0 && c->cmd->chdir.len == 0) {
		s = ffsz_dupstr(&c->cmd->output);

	} else if (c->cmd->output.len != 0 && c->cmd->chdir.len != 0) {
		s = ffsz_allocfmt("%S%c%S"
			, &c->cmd->chdir, FFPATH_SLASH, &c->cmd->output);

	} else if (c->cmd->output.len == 0 && c->cmd->chdir.len != 0) {
		/*
		`in -C out`: "in" -> "out/in"
		`d -R -C out`: "d/f" -> "out/d/f"
		`/tmp/d -R -C out`: "/tmp/d/f" -> "out/d/f"
		*/
		ffstr name;
		if (base.len == 0)
			base = in;
		ffpath_splitpath(base.ptr, base.len, NULL, &name);
		if (name.len != 0)
			ffstr_shift(&in, name.ptr - base.ptr);
		s = ffsz_allocfmt("%S%c%S"
			, &c->cmd->chdir, FFPATH_SLASH, &in);

	} else {
		fcom_errlog("please use --output or --chdir to set destination");
		return NULL;
	}

	fcom_dbglog("output file name: %s", s);
	return s;
}

static void copy_run(fcom_op *op)
{
	struct copy *c = op;
	int r, k = 0;
	enum {
		I_SRC, I_OPEN_IN, I_MKDIR, I_OPEN_OUT,
		I_READ, I_CRYPT, I_WRITE, I_RD_DONE, I_VERIFY, I_DONE,
	};
	while (!FFINT_READONCE(c->stop)) {
		switch (c->st) {

		case I_SRC: {
			ffstr name, base;
			if (0 > core->com->input_next(c->cmd, &name, &base, 0)) {
				if (c->nfiles == 0) {
					fcom_errlog("no input files");
					goto end;
				}
				k = 1;
				goto end;
			}
			c->nfiles++;
			c->iname = ffsz_dupstr(&name);

			if (!c->cmd->stdout) {
				if (NULL == (c->oname = out_name(c, name, base)))
					goto end;
			}

			c->st++;
		}
			// fallthrough

		case I_OPEN_IN: {
			uint flags = FCOM_FILE_READ;
			flags |= fcom_file_cominfo_flags_i(c->cmd);
			r = core->file->open(c->in, c->iname, flags);
			if (r == FCOM_FILE_ERR) goto end;

			fffileinfo fi = {};
			r = core->file->info(c->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;
			if (fffile_isdir(fffileinfo_attr(&fi))) {
				c->st = I_MKDIR;
				continue;
			}

			c->st = I_OPEN_OUT;
			continue;
		}

		case I_MKDIR: {
			r = core->file->dir_create(c->oname, 0);
			if (r == FCOM_FILE_ERR) goto end;
			// move our directory descriptor to Com, so it can use it for fdopendir()
			fffd fd = core->file->fd(c->in, FCOM_FILE_ACQUIRE);
			if (c->cmd->recursive == 1)
				core->com->input_dir(c->cmd, fd);
			c->st = I_SRC;
			continue;
		}

		case I_OPEN_OUT: {
			uint flags = FCOM_FILE_WRITE;
			if (c->verify)
				flags = FCOM_FILE_READWRITE | FCOM_FILE_DIRECTIO;
			flags |= fcom_file_cominfo_flags_o(c->cmd);

			r = core->file->open(c->out, c->oname, flags);
			if (r == FCOM_FILE_ERR) goto end;

			if (!c->cmd->stdout) {
				fffileinfo fi = {};
				r = core->file->info(c->out, &fi);
				if (r == FCOM_FILE_ERR) goto end;
				if (fffile_isdir(fffileinfo_attr(&fi))) {
					fcom_errlog("output file is an existing directory. Use '-C DIR' to copy files into this directory.");
					goto end;
				}
			}

			core->file->behaviour(c->in, FCOM_FBEH_SEQ);

			if (c->aes != NULL)
				crypt_open(c);
			if (c->verify)
				c->md5_obj = c->md5->create();

			c->st++;
		}
			// fallthrough

		case I_READ:
			r = core->file->read(c->in, &c->data, c->in_off);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) {
				c->st = I_RD_DONE;
				continue;
			}
			c->in_off += c->data.len;

			if (c->aes_obj != NULL) {
				c->aes_in = c->data;
				c->st = I_CRYPT;
				continue;
			}

			if (c->md5_obj != NULL)
				c->md5->update(c->md5_obj, c->data.ptr, c->data.len);

			c->st = I_WRITE;
			continue;

		case I_CRYPT: {
			if (c->aes_in.len == 0) {
				c->st = I_READ;
				continue;
			}

			uint n = ffmin(c->aes_in.len, c->aes_buf.cap);
			if (0 != c->aes->process(c->aes_obj, (byte*)c->aes_in.ptr, c->aes_buf.ptr, n, c->aes_iv))
				goto end;
			ffstr_shift(&c->aes_in, n);
			ffstr_set(&c->data, c->aes_buf.ptr, n);

			if (c->md5_obj != NULL)
				c->md5->update(c->md5_obj, c->data.ptr, c->data.len);

			c->st = I_WRITE;
		}
			// fallthrough

		case I_WRITE:
			r = core->file->write(c->out, c->data, c->out_off);
			if (r == FCOM_FILE_ERR) goto end;
			c->out_off += c->data.len;
			c->total += c->data.len;

			c->st = I_READ;
			if (c->aes_obj != NULL)
				c->st = I_CRYPT;
			continue;

		case I_RD_DONE:
			core->file->behaviour(c->out, FCOM_FBEH_TRUNC_PREALLOC);
			if (c->preserve_date) {
				fftime mtime;
				if (0 == core->file->mtime(c->in, &mtime))
					core->file->mtime_set(c->out, &mtime);
			}

			if (c->md5_obj != NULL) {
				c->md5->fin(c->md5_obj, c->md5_result_r, 16);
				c->md5->close(c->md5_obj);
				c->md5_obj = c->md5->create();
				c->out_off = 0;

				c->st = I_VERIFY;
				continue;
			}

			c->st = I_DONE;
			continue;

		case I_VERIFY:
			r = core->file->read(c->out, &c->data, c->out_off);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) {
				if (0 != verify_result(c))
					goto end;
				c->st = I_DONE;
				continue;
			}

			c->md5->update(c->md5_obj, c->data.ptr, c->data.len);
			c->out_off += c->data.len;
			continue;

		case I_DONE:
			if (c->rename_source) {
				char *fn = ffsz_allocfmt("%s.deleted", c->iname);
				if (0 != fffile_rename(c->iname, fn)) {
					fcom_syserrlog("file rename: '%s' -> '%s'", c->iname, fn);
				}
				ffmem_free(fn);
			}

			fcom_verblog("'%s' -> '%s', %,U"
				, c->iname, c->oname, c->total);

			copy_reset(c);
			c->st = I_SRC;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = c->cmd;
	copy_close(c);
	core->com->destroy(cmd);
	}
	if (!k)
		core->exit(1);
	else
		core->exit(0);
}

static const fcom_operation fcom_op_copy = {
	copy_create, copy_close,
	copy_run, copy_signal,
	copy_help,
};


static void copy_init(const fcom_core *_core) { core = _core; }
static void copy_destroy() {}
static const fcom_operation* copy_provide_op(const char *name)
{
	if (ffsz_eq(name, "copy"))
		return &fcom_op_copy;
	return NULL;
}
FF_EXP const struct fcom_module fcom_module = {
	FCOM_VER,
	copy_init, copy_destroy, copy_provide_op,
};
