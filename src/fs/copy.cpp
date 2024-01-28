/** fcom: copy data (input -> output)
2022, Simon Zolin */

static const char* copy_help()
{
	return "\
Copy files and directories, plus encryption & verification.\n\
Uses large `--buffer` by default.\n\
File properties are preserved.\n\
Usage:\n\
  `fcom copy` INPUT... [-o OUTPUT_FILE] [-C OUTPUT_DIR] [OPTIONS]\n\
\n\
OPTIONS:\n\
\n\
    `-e`, `--encrypt` PASSWORD\n\
                        Encrypt data (AES-256-CFB key=SHA256(password))\n\
    `-d`, `--decrypt` PASSWORD\n\
                        Decrypt data\n\
\n\
    `-5`, `--md5`           Print MD5 checksum of input file\n\
    `-y`, `--verify`        Verify data consistency with MD5.\n\
                        Implies `--directio` on output file.\n\
\n\
        `--rename-source` Rename source file to *.deleted after successful operation\n\
    `-u`, `--update`        Overwrite only older files\n\
        `--write-into`\n\
                        Overwrite file data instead of deleting the old target\n\
";
}

#include <fcom.h>
#include <ffsys/path.h>
#include <util/util.hpp>

static const fcom_core *core;

#define ffmem_free0(p)  ffmem_free(p), (p) = NULL

#define BUF_LARGE  (8*1024*1024)

struct copy {
	fcom_cominfo cominfo;

	uint st;
	ffstr data;
	fcom_cominfo *cmd;
	uint stop;

	fcom_filexx	input;
	ffstr		name;
	char*		iname;
	xxfileinfo	fi;
	uint64		in_off;
	uint		nfiles;
	ffstr		basename;

	struct {
		fcom_aes_obj *aes_obj;
		const fcom_aes *aes;
		byte aes_iv[16];
		ffstr aes_in;
		ffvec aes_buf;
		const fcom_hash *sha256;
		uint aes_iv_out :1;
		uint aes_iv_in :1;
	} cr;

	struct o_s {
		o_s() : f(core) {}
		uint		state;
		fcom_filexx	f;
		uint64		total, off;
		char*		name;
		char*		name_tmp;
		xxfileinfo	fi;
		uint		del_on_close :1;
	} o;

	struct {
		const fcom_hash *md5;
		fcom_hash_obj *md5_obj;
		byte md5_result_r[16];
	} vf;

	byte verify;
	byte print_md5;
	ffstr encrypt, decrypt;
	byte preserve_date;
	byte rename_source;
	byte update;
	byte write_into;

	copy() : input(core) {}
};

#define O(member)  (void*)FF_OFF(struct copy, member)

static int args_parse(struct copy *c, fcom_cominfo *cmd)
{
	c->preserve_date = 1;
	if (cmd->buffer_size == 0)
		cmd->buffer_size = BUF_LARGE;

	static const struct ffarg args[] = {
		{ "--decrypt",		'S',	O(decrypt) },
		{ "--encrypt",		'S',	O(encrypt) },
		{ "--md5",			'1',	O(print_md5) },
		{ "--rename-source",'1',	O(rename_source) },
		{ "--update",		'1',	O(update) },
		{ "--verify",		'1',	O(verify) },
		{ "--write-into",	'1',	O(write_into) },
		{ "-5",				'1',	O(print_md5) },
		{ "-d",				'S',	O(decrypt) },
		{ "-e",				'S',	O(encrypt) },
		{ "-u",				'1',	O(update) },
		{ "-y",				'1',	O(verify) },
		{}
	};
	if (0 != core->com->args_parse(cmd, args, c, FCOM_COM_AP_INOUT))
		return -1;

	if (cmd->stdout)
		c->update = 0;

	if (c->update)
		cmd->overwrite = 1;

	cmd->recursive = 1;
	return 0;
}

#undef O

static void copy_run(fcom_op *op);
#include <fs/copy-crypt.h>
#include <fs/copy-output.h>
#include <fs/copy-verify.h>

static void copy_close(fcom_op *op)
{
	struct copy *c = (struct copy*)op;
	crypt_close(c);
	verify_reset(c);
	output_close(c);
	c->~copy();
	ffmem_free(c);
}

static fcom_op* copy_create(fcom_cominfo *cmd)
{
	struct copy *c = new(ffmem_new(struct copy)) struct copy;

	if (0 != args_parse(c, cmd))
		goto end;
	c->cmd = cmd;

	{
	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	fc.n_buffers = 1;
	c->input.create(&fc);
	}

	if (0 != output_init(c)) goto end;
	if (0 != crypt_init(c)) goto end;
	if (0 != verify_init(c)) goto end;

	return c;

end:
	copy_close(c);
	return NULL;
}

static void copy_reset(struct copy *c)
{
	c->in_off = 0;
	crypt_reset(c);
	verify_reset(c);
	output_reset(c);
	ffmem_free0(c->iname);
}

static void copy_signal(fcom_op *op, uint signal)
{
	struct copy *c = (struct copy*)op;
	FFINT_WRITEONCE(c->stop, 1);
}

static void copy_run(fcom_op *op)
{
	struct copy *c = (struct copy*)op;
	int r, k = 0;
	enum {
		I_SRC, I_OPEN_IN, I_MKDIR, I_OPEN_OUT,
		I_READ, I_CRYPT, I_WRITE, I_RD_DONE, I_VERIFY, I_DONE,
	};
	while (!FFINT_READONCE(c->stop)) {
		switch (c->st) {

		case I_SRC:
			copy_reset(c);
			if (0 > core->com->input_next(c->cmd, &c->name, &c->basename, 0)) {
				if (c->nfiles == 0) {
					fcom_errlog("no input files");
					goto end;
				}
				k = 1;
				goto end;
			}
			c->nfiles++;
			c->iname = ffsz_dupstr(&c->name);
			c->st = I_OPEN_IN;
			// fallthrough

		case I_OPEN_IN: {
			uint flags = FCOM_FILE_READ | FCOM_FILE_READAHEAD;
			flags |= fcom_file_cominfo_flags_i(c->cmd);
			r = c->input.open(c->iname, flags);
			if (r == FCOM_FILE_ERR) goto end;

			if (!c->cmd->stdout) {
				ffmem_free0(c->o.name);
				ffmem_free0(c->o.name_tmp);
				if (NULL == (c->o.name = out_name(c, c->name, c->basename)))
					goto end;
				c->o.name_tmp = ffsz_allocfmt("%s.fcomtmp", c->o.name);
			}

			r = c->input.info(&c->fi);
			if (r == FCOM_FILE_ERR) goto end;

			if (0 != core->com->input_allowed(c->cmd, c->name, c->fi.dir())) {
				c->st = I_SRC;
				continue;
			}

			if (c->fi.dir()) {
				c->st = I_MKDIR;
				continue;
			}

			c->st = I_OPEN_OUT;
			continue;
		}

		case I_MKDIR:
			r = core->file->dir_create(c->o.name, FCOM_FILE_DIR_RECURSIVE);
			if (r == FCOM_FILE_ERR) goto end;
			if (c->cmd->recursive == 1)
				core->com->input_dir(c->cmd, c->input.acquire_fd());
			c->st = I_SRC;
			continue;

		case I_OPEN_OUT:
			r = output_open(c);
			if (r == 'skip') {
				c->st = I_SRC;
				continue;
			}
			if (r != 0) goto end;

			c->input.behaviour(FCOM_FBEH_SEQ);

			if (0 != crypt_open(c)) goto end;
			if (0 != verify_open(c)) goto end;

			c->st = I_READ;
			// fallthrough

		case I_READ:
			r = c->input.read(&c->data, c->in_off);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) {
				c->st = I_RD_DONE;
				continue;
			}
			c->in_off += c->data.len;

			if (c->cr.aes_obj != NULL) {
				c->cr.aes_in = c->data;
				c->st = I_CRYPT;
				continue;
			}

			verify_process(c, c->data);

			c->st = I_WRITE;
			continue;

		case I_CRYPT:
			if (c->cr.aes_in.len == 0) {
				c->st = I_READ;
				continue;
			}

			if (0 != crypt_process(c, &c->cr.aes_in, &c->data)) goto end;
			verify_process(c, c->data);

			c->st = I_WRITE;
			// fallthrough

		case I_WRITE:
			if (0 != output_write(c, c->data)) goto end;
			c->st = I_READ;
			if (c->cr.aes_obj != NULL)
				c->st = I_CRYPT;
			continue;

		case I_RD_DONE:
			c->o.f.behaviour(FCOM_FBEH_TRUNC_PREALLOC);
			if (c->preserve_date) {
				c->o.f.mtime(c->fi.mtime1());
			}

			if (verify_read_fin(c)) {
				c->st = I_VERIFY;
				continue;
			}

			c->st = I_DONE;
			continue;

		case I_VERIFY:
			r = c->o.f.read(&c->data, c->o.off);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) {
				if (0 != verify_result(c))
					goto end;
				c->st = I_DONE;
				continue;
			}

			verify_process(c, c->data);
			c->o.off += c->data.len;
			continue;

		case I_DONE:
			r = output_fin(c);
			if (r == 'asyn') return;
			if (r != 0) goto end;

			if (c->rename_source) {
				ffstrxx_buf<4096> fn;
				if (0 != fffile_rename(c->iname, fn.zfmt("%s.deleted", c->iname))) {
					fcom_syserrlog("file rename: '%s' -> '%s'", c->iname, fn.ptr);
				}
			}

			fcom_verblog("'%s' -> '%s'  [%,U]"
				, c->iname, c->o.name, c->o.total);

			c->st = I_SRC;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = c->cmd;
	copy_close(c);
	core->com->complete(cmd, k ? 0 : 1);
	}
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
FCOM_EXPORT const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	copy_init, copy_destroy, copy_provide_op,
};
