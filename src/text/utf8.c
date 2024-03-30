/** fcom: Convert files to UTF-8
2023, Simon Zolin */

static const char* utf8_help()
{
	return "\
Convert UTF-8/16 (with BOM) files to UTF-8 (without BOM).\n\
Usage:\n\
  fcom utf8 INPUT... -o OUTPUT\n\
";
}

#include <fcom.h>
#include <ffbase/unicode.h>

static const fcom_core *core;

struct utf8 {
	fcom_cominfo cominfo;

	uint st;
	uint stop;
	fcom_cominfo *cmd;

	fcom_file_obj *out;
	ffstr iname;
	ffstr data;
	ffvec fdata, buf;
	uint64 off;
};

static int args_parse(struct utf8 *u, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{}
	};
	if (0 != core->com->args_parse(cmd, args, u, FCOM_COM_AP_INOUT))
		return -1;

	if (u->cmd->output.len == 0)
		u->cmd->stdout = 1;

	return 0;
}

static void utf8_close(fcom_op *op)
{
	struct utf8 *u = op;
	core->file->destroy(u->out);
	ffvec_free(&u->fdata);
	ffvec_free(&u->buf);
	ffmem_free(u);
}

static fcom_op* utf8_create(fcom_cominfo *cmd)
{
	struct utf8 *u = ffmem_new(struct utf8);
	u->cmd = cmd;

	if (0 != args_parse(u, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	u->out = core->file->create(&fc);

	return u;

end:
	utf8_close(u);
	return NULL;
}

static ffsize ffutf8_encodedata(char *dst, ffsize cap, const char *src, ffsize *plen, uint flags)
{
	ffssize r;
	ffsize len = *plen;

	switch (flags) {
	case FFUNICODE_UTF8:
		if (dst == NULL)
			return len;
		len = ffmin(cap, len);
		ffmem_copy(dst, src, len);
		*plen = len;
		return len;

	case FFUNICODE_UTF16LE:
		r = ffutf8_from_utf16(dst, cap, src, *plen, FFUNICODE_UTF16LE);
		break;

	case FFUNICODE_UTF16BE:
		r = ffutf8_from_utf16(dst, cap, src, *plen, FFUNICODE_UTF16BE);
		break;

	default:
		r = ffutf8_from_cp(dst, cap, src, *plen, flags);
		break;
	}

	if (r < 0)
		return 0;
	return r;
}

static ffsize ffutf8_encodewhole(char *dst, ffsize cap, const char *src, ffsize len, uint flags)
{
	ffsize ln = len;
	ffsize r = ffutf8_encodedata(dst, cap, src, &ln, flags);
	if (ln != len)
		return 0; //not enough output space
	return r;
}

static ffsize ffutf8_strencode(ffvec *dst, const char *src, ffsize len, uint flags)
{
	ffsize r = ffutf8_encodewhole(NULL, 0, src, len, flags);
	if (NULL == ffvec_realloc(dst, r, 1))
		return 0;
	r = ffutf8_encodewhole(dst->ptr, dst->cap, src, len, flags);
	dst->len = r;
	return r;
}

static void utf8_run(fcom_op *op)
{
	struct utf8 *u = op;
	int r, rc = 1;
	enum { I_IN, I_READ, I_PROC, I_OUT_OPEN, I_WRITE };

	while (!FFINT_READONCE(u->stop)) {
		switch (u->st) {

		case I_IN:
			if (0 > (r = core->com->input_next(u->cmd, &u->iname, NULL, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE)
					rc = 0;
				goto end;
			}

			if (0 != core->com->input_allowed(u->cmd, u->iname, FCOM_COM_IA_AUTO))
				continue;

			u->st = I_READ;
			// fallthrough

		case I_READ:
			if (0 != fffile_readwhole(u->iname.ptr, &u->fdata, (uint64)-1))
				goto end;
			u->st = I_PROC;
			// fallthrough

		case I_PROC: {
			ffstr data = *(ffstr*)&u->fdata;
			ffsize n = data.len;
			int coding = ffutf_bom(data.ptr, &n);
			if (coding == -1) {
				fcom_infolog("%S: no BOM, skipping file", &u->iname);
				u->st = I_IN;
				continue;
			}
			ffstr_shift(&data, n);

			u->buf.len = 0;
			if (0 == ffutf8_strencode(&u->buf, data.ptr, data.len, coding)) {
				fcom_errlog("ffutf8_strencode");
				goto end;
			}
			u->st = I_OUT_OPEN;
		}
			// fallthrough

		case I_OUT_OPEN: {
			uint oflags = FCOM_FILE_WRITE;
			oflags |= fcom_file_cominfo_flags_o(u->cmd);
			r = core->file->open(u->out, u->cmd->output.ptr, oflags);
			if (r == FCOM_FILE_ERR) goto end;
			u->st = I_WRITE;
		}
			// fallthrough

		case I_WRITE:
			r = core->file->write(u->out, *(ffstr*)&u->buf, -1);
			if (r == FCOM_FILE_ERR) goto end;

			core->file->close(u->out);

			u->st = I_IN;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = u->cmd;
	utf8_close(u);
	core->com->complete(cmd, rc);
	}
}

static void utf8_signal(fcom_op *op, uint signal)
{
	struct utf8 *u = op;
	FFINT_WRITEONCE(u->stop, 1);
}

static const fcom_operation fcom_op_utf8 = {
	utf8_create, utf8_close,
	utf8_run, utf8_signal,
	utf8_help,
};

FCOM_MOD_DEFINE(utf8, fcom_op_utf8, core)
