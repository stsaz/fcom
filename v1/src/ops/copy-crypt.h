/** fcom: copy: encrypt/decrypt data
2022, Simon Zolin */

#include <FFOS/random.h>

static int crypt_init(struct copy *c)
{
	if (c->encrypt.len == 0 && c->decrypt.len == 0) return 0;

	if (c->encrypt.len != 0 && c->decrypt.len != 0) {
		fcom_errlog("both --encrypt and --decrypt can't be together");
		return -1;
	}

	if (NULL == (c->cr.sha256 = core->com->provide("crypto.sha256", 0)))
		return -1;

	const char *opname = (c->encrypt.len != 0) ? "crypto.fcom_aes_encrypt" : "crypto.fcom_aes_decrypt";
	if (NULL == (c->cr.aes = core->com->provide(opname, 0)))
		return -1;

	ffvec_alloc(&c->cr.aes_buf, 64*1024, 1);
	return 0;
}

static void crypt_close(struct copy *c)
{
	ffmem_zero(c->encrypt.ptr, c->encrypt.len);
	ffmem_zero(c->decrypt.ptr, c->decrypt.len);
	ffstr_free(&c->encrypt);
	ffstr_free(&c->decrypt);
	ffvec_free(&c->cr.aes_buf);
}

static void sha256_hash(struct copy *c, const void *data, ffsize size, byte result[32])
{
	fcom_hash_obj *h = c->cr.sha256->create();
	c->cr.sha256->update(h, data, size);
	c->cr.sha256->fin(h, result, 32);
	c->cr.sha256->close(h);
}

static void iv_get(byte *iv)
{
	fftime now;
	fftime_now(&now);
	ffrand_seed(now.sec);

	for (uint i = 0;  i < 16;  i += 4) {
		int n = ffrand_get();
		*(int*)(iv + i) = n;
	}
}

static int crypt_open(struct copy *c)
{
	if (c->cr.aes == NULL) return 0;

	ffstr pw = (c->encrypt.len != 0) ? c->encrypt : c->decrypt;
	byte key[32];
	sha256_hash(c, pw.ptr, pw.len, key);

	if (c->encrypt.len != 0) {
		c->cr.aes_iv_out = 1;
		iv_get(c->cr.aes_iv);
	} else {
		c->cr.aes_iv_in = 1;
	}

	if (NULL == (c->cr.aes_obj = c->cr.aes->create(key, sizeof(key), FCOM_AES_CFB)))
		return -1;
	ffmem_zero_obj(key);
	return 0;
}

static void crypt_reset(struct copy *c)
{
	if (c->cr.aes_obj == NULL) return;

	c->cr.aes->close(c->cr.aes_obj),  c->cr.aes_obj = NULL;
}

static int crypt_process(struct copy *c, ffstr *out)
{
	if (c->cr.aes_iv_out) {
		c->cr.aes_iv_out = 0;
		ffstr_set(out, c->cr.aes_iv, 16);
		return 0;
	} else if (c->cr.aes_iv_in) {
		c->cr.aes_iv_in = 0;
		ffmem_copy(c->cr.aes_iv, c->cr.aes_in.ptr, 16);
		ffstr_shift(&c->cr.aes_in, 16);
	}

	uint n = ffmin(c->cr.aes_in.len, c->cr.aes_buf.cap);
	if (0 != c->cr.aes->process(c->cr.aes_obj, (byte*)c->cr.aes_in.ptr, c->cr.aes_buf.ptr, n, c->cr.aes_iv))
		return -1;
	ffstr_shift(&c->cr.aes_in, n);
	ffstr_set(out, c->cr.aes_buf.ptr, n);
	return 0;
}
