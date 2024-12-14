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
        `--rename-source`   Rename source file to *.deleted after successful operation\n\
    `-u`, `--update`          Overwrite only older files\n\
          `--replace-date`  Just copy file date (don't overwrite content).\n\
                          Use with `--update`.\n\
        `--write-into`\n\
                        Overwrite file data instead of deleting the old target\n\
";
}

#include <fcom.h>
#include <ffsys/path.h>
#include <ffsys/globals.h>

static const fcom_core *core;

#define ffmem_free0(p)  ffmem_free(p), (p) = NULL

#define BUF_LARGE  (8*1024*1024)

struct copy {
	fcom_cominfo cominfo;

	uint st;
	ffstr data;
	fcom_cominfo *cmd;
	uint stop;

	fcom_file_obj*	input;
	ffstr		name;
	char*		iname;
	fffileinfo	fi;
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
		uint		state;
		fcom_file_obj*	f;
		uint64		total, off;
		char*		name;
		char*		name_tmp;
		fffileinfo	fi;
		uint		del_on_close :1;
	} o;

	struct {
		const fcom_hash *md5;
		fcom_hash_obj *md5_obj;
		byte md5_result_r[16];
	} vf;

	ffstr encrypt, decrypt;
	u_char verify;
	u_char print_md5;
	u_char preserve_date;
	u_char rename_source;
	u_char replace_date;
	u_char update;
	u_char write_into;
};

static int copy_input_next(struct copy *c)
{
	int r;
	if (0 > core->com->input_next(c->cmd, &c->name, &c->basename, 0)) {
		if (!c->nfiles) {
			fcom_errlog("no input files");
			return 'erro';
		}
		return 'done';
	}
	c->nfiles++;
	c->iname = ffsz_dupstr(&c->name);

	uint flags = FCOM_FILE_READ | FCOM_FILE_READAHEAD;
	flags |= fcom_file_cominfo_flags_i(c->cmd);
	r = core->file->open(c->input, c->iname, flags);
	if (r == FCOM_FILE_ERR) return 'erro';

	r = core->file->info(c->input, &c->fi);
	if (r == FCOM_FILE_ERR) return 'erro';

	unsigned dir = fffile_isdir(fffileinfo_attr(&c->fi));
	if (core->com->input_allowed(c->cmd, c->name, dir)) {
		return 'next';
	}

	if (dir && c->cmd->recursive)
		core->com->input_dir(c->cmd, core->file->fd(c->input, FCOM_FILE_ACQUIRE));

	return 0;
}

static void copy_complete(struct copy *c)
{
	if (c->rename_source) {
		char *fn = ffsz_allocfmt("%s.deleted", c->iname);
		if (fffile_rename(c->iname, fn)) {
			fcom_syserrlog("file rename: '%s' -> '%s'", c->iname, fn);
		}
		ffmem_free(fn);
	}

	fcom_verblog("'%s' -> '%s'  [%,U]"
		, c->iname, c->o.name, c->o.total);
}

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
		{ "--replace-date",	'1',	O(replace_date) },
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
	if (core->com->args_parse(cmd, args, c, FCOM_COM_AP_INOUT))
		return -1;

	if (!(cmd->chdir.len || cmd->output.len))
		fcom_errlog("please use --output or --chdir to set destination");

	if (cmd->stdout)
		c->update = 0;

	if (c->update)
		cmd->overwrite = 1;

	cmd->recursive = (cmd->recursive != 0xff) ? 1 : 0;
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
	core->file->destroy(c->input);
	ffmem_free(c);
}

static fcom_op* copy_create(fcom_cominfo *cmd)
{
	struct copy *c = ffmem_new(struct copy);

	if (args_parse(c, cmd))
		goto end;
	c->cmd = cmd;

	{
	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	fc.n_buffers = 1;
	c->input = core->file->create(&fc);
	}

	if (output_init(c)) goto end;
	if (crypt_init(c)) goto end;
	if (verify_init(c)) goto end;

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
		I_SRC, I_OPEN_OUT,
		I_READ, I_CRYPT, I_WRITE, I_RD_DONE, I_VERIFY, I_DONE,
	};
	while (!FFINT_READONCE(c->stop)) {
		switch (c->st) {

		case I_SRC:
			copy_reset(c);
			switch (copy_input_next(c)) {
			case 'next':
				c->st = I_SRC;
				continue;

			case 'done':
				k = 1;
				goto end;

			case 'erro':
				goto end;
			}

			c->st = I_OPEN_OUT;
			continue;

		case I_OPEN_OUT:
			r = output_open(c);
			if (r == 'skip') {
				c->st = I_SRC;
				continue;
			}
			if (r != 0) goto end;

			core->file->behaviour(c->input, FCOM_FBEH_SEQ);

			if (crypt_open(c)) goto end;
			if (verify_open(c)) goto end;

			c->st = I_READ;
			// fallthrough

		case I_READ:
			r = core->file->read(c->input, &c->data, c->in_off);
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

			if (crypt_process(c, &c->cr.aes_in, &c->data)) goto end;
			verify_process(c, c->data);

			c->st = I_WRITE;
			// fallthrough

		case I_WRITE:
			if (output_write(c, c->data)) goto end;
			c->st = I_READ;
			if (c->cr.aes_obj != NULL)
				c->st = I_CRYPT;
			continue;

		case I_RD_DONE:
			core->file->behaviour(c->o.f, FCOM_FBEH_TRUNC_PREALLOC);
			if (c->preserve_date) {
				core->file->mtime_set(c->o.f, fffileinfo_mtime1(&c->fi));
			}

			if (verify_read_fin(c)) {
				c->st = I_VERIFY;
				continue;
			}

			c->st = I_DONE;
			continue;

		case I_VERIFY:
			r = core->file->read(c->o.f, &c->data, c->o.off);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) {
				if (verify_result(c))
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

			copy_complete(c);

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

FCOM_MOD_DEFINE(copy, fcom_op_copy, core)
