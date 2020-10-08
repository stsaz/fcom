/** Encrypt/decrypt files.
Copyright (c) 2019 Simon Zolin
*/

#include <fcom.h>
#include <FF/path.h>
#include <ffbase/atomic.h>
#include <aes/aes-ff.h>
#include <sha1/sha1.h>
#include <sha/sha256.h>


#define dbglog(dbglev, fmt, ...)  fcom_dbglog(dbglev, FILT_NAME, fmt, __VA_ARGS__)
#define errlog(fmt, ...)  fcom_errlog(FILT_NAME, fmt, __VA_ARGS__)

static const fcom_core *core;
static const fcom_command *com;

// MODULE
static int crypto_sig(uint signo);
static const void* crypto_iface(const char *name);
static int crypto_conf(const char *name, ffpars_ctx *ctx);
static const fcom_mod crypto_mod = {
	.sig = &crypto_sig, .iface = &crypto_iface, .conf = &crypto_conf,
};

// ENCRYPT/DECRYPT
static void* crypt_open(fcom_cmd *cmd);
static void crypt_close(void *p, fcom_cmd *cmd);
static int crypt_process(void *p, fcom_cmd *cmd);
static const fcom_filter crypt_filt = { &crypt_open, &crypt_close, &crypt_process };

// ENCRYPT1
static void* encrypt1_open(fcom_cmd *cmd);
static void encrypt1_close(void *p, fcom_cmd *cmd);
static int encrypt1_process(void *p, fcom_cmd *cmd);
static const fcom_filter encrypt1_filt = { &encrypt1_open, &encrypt1_close, &encrypt1_process };

// DECRYPT1
static void* decrypt1_open(fcom_cmd *cmd);
static void decrypt1_close(void *p, fcom_cmd *cmd);
static int decrypt1_process(void *p, fcom_cmd *cmd);
static const fcom_filter decrypt1_filt = { &decrypt1_open, &decrypt1_close, &decrypt1_process };


FF_EXP const fcom_mod* fcom_getmod(const fcom_core *_core)
{
	core = _core;
	return &crypto_mod;
}

struct oper {
	const char *name;
	const char *mod;
	const void *iface;
};

static const struct oper cmds[] = {
	{ "encrypt", "crypto.crypt", &crypt_filt },
	{ "decrypt", "crypto.crypt", &crypt_filt },
	{ NULL, "crypto.encrypt1", &encrypt1_filt },
	{ NULL, "crypto.decrypt1", &decrypt1_filt },
};

static const void* crypto_iface(const char *name)
{
	const struct oper *op;
	FF_FOREACH(cmds, op) {
		if (ffsz_eq(name, op->mod + FFS_LEN("crypto.")))
			return op->iface;
	}
	return NULL;
}

static int crypto_conf(const char *name, ffpars_ctx *ctx)
{
	return 1;
}

static int crypto_sig(uint signo)
{
	switch (signo) {
	case FCOM_SIGINIT: {
		com = core->iface("core.com");

		const struct oper *op;
		FF_FOREACH(cmds, op) {
			if (op->name != NULL
				&& 0 != com->reg(op->name, op->mod))
				return -1;
		}
		break;
	}
	case FCOM_SIGSTART:
		break;
	case FCOM_SIGFREE:
		break;
	}
	return 0;
}

static void set_key_and_iv(const char *passwd, byte iv[16], byte key[32])
{
	sha256_hash(passwd, ffsz_len(passwd), key);

	byte sha1sum[SHA1_LENGTH];
	sha1_ctx sha1;
	sha1_init(&sha1);
	sha1_update(&sha1, key, 32);
	sha1_fin(&sha1, sha1sum);
	ffmem_copy(iv, sha1sum, 16);
}

#define FILT_NAME "encrypt"

#define BUFSIZE  (64 * 1024)
#define AES_MODE  AES_CFB

struct encrypt {
	uint state;
	ffstr inbuf;
	ffstr outbuf;
	ffstr in;

	aes_ctx aes;
	byte iv[16];
};

static void* encrypt1_open(fcom_cmd *cmd)
{
	if (cmd->passwd == NULL || cmd->passwd[0] == '\0') {
		errlog("password isn't specified", 0);
		return FCOM_OPEN_ERR;
	}

	struct encrypt *e = ffmem_new(struct encrypt);
	if (e == NULL
		|| NULL == ffstr_alloc(&e->inbuf, BUFSIZE)
		|| NULL == ffstr_alloc(&e->outbuf, BUFSIZE))
		return FCOM_OPEN_SYSERR;

	byte key[32];
	set_key_and_iv(cmd->passwd, e->iv, key);
	if (0 != aes_encrypt_init(&e->aes, key, sizeof(key), AES_MODE)) {
		encrypt1_close(e, cmd);
		return FCOM_OPEN_ERR;
	}
	e->state = 1;
	return e;
}

static void encrypt1_close(void *p, fcom_cmd *cmd)
{
	struct encrypt *e = p;
	aes_encrypt_free(&e->aes);
	ffslice_free(&e->inbuf);
	ffslice_free(&e->outbuf);
	ffmem_free(e);
}

static int encrypt1_process(void *p, fcom_cmd *cmd)
{
	struct encrypt *e = p;

	if (cmd->flags & FCOM_CMD_FWD) {
		e->in = cmd->in;
	}

	ffstr chunk;
	ffsize cap = BUFSIZE;
	ffsize n = ffstr_gather(&e->inbuf, &cap, e->in.ptr, e->in.len, BUFSIZE, &chunk);
	ffstr_shift(&e->in, n);

	if (chunk.len < BUFSIZE) {
		if (!cmd->in_last)
			return FCOM_MORE;

		ffstr_set2(&chunk, &e->inbuf);
		e->state = 2;
	}
	e->inbuf.len = 0;

	int r = aes_encrypt_chunk(&e->aes, (byte*)chunk.ptr, (byte*)e->outbuf.ptr, chunk.len, e->iv);
	if (r != 0) {
		errlog("aes_encrypt()", 0);
		return FCOM_ERR;
	}

	ffstr_set(&cmd->out, e->outbuf.ptr, chunk.len);
	return (cmd->in_last) ? FCOM_OUTPUTDONE : FCOM_DATA;
}


struct decrypt {
	ffstr inbuf;
	ffstr outbuf;
	ffstr in;
	uint64 inoff;

	aes_ctx aes;
	byte iv[16];
};

static void* decrypt1_open(fcom_cmd *cmd)
{
	if (cmd->passwd == NULL || cmd->passwd[0] == '\0') {
		errlog("password isn't specified", 0);
		return FCOM_OPEN_ERR;
	}

	struct decrypt *d = ffmem_new(struct decrypt);
	if (d == NULL
		|| NULL == ffstr_alloc(&d->inbuf, BUFSIZE)
		|| NULL == ffstr_alloc(&d->outbuf, BUFSIZE))
		return FCOM_OPEN_SYSERR;

	byte key[32];
	set_key_and_iv(cmd->passwd, d->iv, key);
	if (0 != aes_decrypt_init(&d->aes, key, sizeof(key), AES_MODE)) {
		decrypt1_close(d, cmd);
		return FCOM_OPEN_ERR;
	}
	return d;
}

static void decrypt1_close(void *p, fcom_cmd *cmd)
{
	struct decrypt *d = p;
	aes_decrypt_free(&d->aes);
	ffslice_free(&d->inbuf);
	ffslice_free(&d->outbuf);
	ffmem_free(d);
}

static int decrypt1_process(void *p, fcom_cmd *cmd)
{
	struct decrypt *d = p;

	if (cmd->flags & FCOM_CMD_FWD) {
		d->in = cmd->in;
	}

	ffstr chunk;
	ffsize cap = BUFSIZE;
	ffsize n = ffstr_gather(&d->inbuf, &cap, d->in.ptr, d->in.len, BUFSIZE, &chunk);
	ffstr_shift(&d->in, n);

	if (chunk.len < BUFSIZE) {
		if (!(cmd->flags & FCOM_CMD_FIRST))
			return FCOM_MORE;
		ffstr_set2(&chunk, &d->inbuf);
	}
	d->inbuf.len = 0;

	int r = aes_decrypt_chunk(&d->aes, (byte*)chunk.ptr, (byte*)d->outbuf.ptr, chunk.len, d->iv);
	if (r != 0) {
		errlog("aes_decrypt()", 0);
		return FCOM_ERR;
	}

	ffstr_set(&cmd->out, d->outbuf.ptr, chunk.len);
	return (cmd->flags & FCOM_CMD_FIRST) ? FCOM_DONE : FCOM_DATA;
}

struct crypto {
	ffatomic nsubtasks;
	fcom_cmd *cmd;
	ffvec fn;
	uint close;
};

static void* crypt_open(fcom_cmd *cmd)
{
	struct crypto *c = ffmem_new(struct crypto);
	c->cmd = cmd;
	return c;
}

static void crypt_close(void *p, fcom_cmd *cmd)
{
	struct crypto *c = p;

	if (0 != ffatomic_load(&c->nsubtasks)) {
		// wait until the last subtask is finished
		c->close = 1;
		return;
	}

	ffvec_free(&c->fn);
	ffmem_free(c);
}

static void task_onfinish(fcom_cmd *cmd, uint sig, void *param)
{
	struct crypto *c = param;
	if (1 == ffatomic_fetch_add(&c->nsubtasks, -1) && c->close) {
		crypt_close(c, NULL);
		return;
	}

	com->ctrl(c->cmd, FCOM_CMD_RUNASYNC);
}

static void fcom_cmd_set(fcom_cmd *dst, const fcom_cmd *src)
{
	ffmem_copy(dst, src, sizeof(*dst));
	ffstr_null(&dst->in);
	ffstr_null(&dst->out);
}

/** The same as ffpath_split3() except that for fullname="path/.ext" ".ext" is treated as extension. */
static void path_split3(const char *fullname, size_t len, ffstr *path, ffstr *name, ffstr *ext)
{
	ffpath_split2(fullname, len, path, name);
	char *dot = ffs_rfind(name->ptr, name->len, '.');
	ffs_split2(name->ptr, name->len, dot, name, ext);
}

/** Make output file name */
static const char* make_filename(struct crypto *c, const char *ifn, const char *ofn)
{
	if (ofn == NULL) {
		errlog("output file isn't set", 0);
		return NULL;
	}

	ffstr idir, iname;
	ffstr_setz(&iname, ifn);
	ffpath_split3(iname.ptr, iname.len, &idir, &iname, NULL);

	ffstr odir, oname, oext;
	ffstr_setz(&oname, ofn);
	path_split3(oname.ptr, oname.len, &odir, &oname, &oext);
	if (oname.len == 0) {
		if (0 != ffpath_makefn_out((ffarr*)&c->fn, &idir, &iname, &odir, &oext)) {
			errlog("ffpath_makefn_out()", 0);
			return NULL;
		}
		return c->fn.ptr;
	}
	return ofn;
}

static int crypt_process1(struct crypto *c, fcom_cmd *cmd, const char *fn)
{
	fcom_cmd ncmd = {};
	fcom_cmd_set(&ncmd, cmd);
	ncmd.name = "crypto.task";
	ncmd.flags = FCOM_CMD_EMPTY | FCOM_CMD_INTENSE;
	ncmd.input.fn = fn;

	ncmd.output.fn = make_filename(c, fn, cmd->output.fn);
	if (ncmd.output.fn == NULL)
		return FCOM_ERR;
	ncmd.out_fn_copy = 1;

	fcom_cmd *nc;
	if (NULL == (nc = com->create(&ncmd)))
		return FCOM_ERR;

	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_IN(nc));

	const char *name = "crypto.encrypt1";
	if (ffsz_eq(c->cmd->name, "decrypt"))
		name = "crypto.decrypt1";
	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, name);

	com->ctrl(nc, FCOM_CMD_FILTADD_LAST, FCOM_CMD_FILT_OUT(nc));

	com->fcom_cmd_monitor_func(nc, task_onfinish, c);
	ffatomic_fetch_add(&c->nsubtasks, 1);
	com->ctrl(nc, FCOM_CMD_RUNASYNC);
	return FCOM_MORE;
}

static int crypt_process(void *p, fcom_cmd *cmd)
{
	struct crypto *c = p;
	int r;
	const char *fn;
	for (;;) {
		if (NULL == (fn = com->arg_next(cmd, FCOM_CMD_ARG_FILE))) {
			if (0 != ffatomic_load(&c->nsubtasks))
				return FCOM_ASYNC;
			return FCOM_DONE;
		}

		if (FCOM_MORE != (r = crypt_process1(c, cmd, fn)))
			return r;

		if (0 == core->cmd(FCOM_WORKER_AVAIL))
			return FCOM_ASYNC;
	}
}

#undef FILT_NAME
