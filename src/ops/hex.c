/** fcom: print file contents in hexadecimal format
2022, Simon Zolin */

static const char* hex_help()
{
	return "\
Print file contents in hexadecimal format.\n\
Usage:\n\
  `fcom hex` INPUT... [OPTIONS]\n\
";
}

#include <fcom.h>
#include <ffbase/mem-print.h>

static const fcom_core *core;

struct hex {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	uint stop;
	fcom_file_obj *in, *out;
	ffstr data;
	ffvec buf;
	uint64 off;
};

static int args_parse(struct hex *h, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{}
	};
	if (0 != core->com->args_parse(cmd, args, h, FCOM_COM_AP_INOUT))
		return -1;

	if (h->cmd->output.len == 0)
		h->cmd->stdout = 1;

	return 0;
}

static void hex_close(fcom_op *op)
{
	struct hex *h = op;
	core->file->destroy(h->in);
	core->file->destroy(h->out);
	ffvec_free(&h->buf);
	ffmem_free(h);
}

static fcom_op* hex_create(fcom_cominfo *cmd)
{
	struct hex *h = ffmem_new(struct hex);
	h->cmd = cmd;

	if (0 != args_parse(h, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	h->in = core->file->create(&fc);
	h->out = core->file->create(&fc);

	return h;

end:
	hex_close(h);
	return NULL;
}

static void hex_run(fcom_op *op)
{
	struct hex *h = op;
	int r, rc = 1;
	enum { I_OUT_OPEN, I_IN, I_READ, };

	while (!FFINT_READONCE(h->stop)) {
		switch (h->st) {

		case I_OUT_OPEN: {
			uint oflags = FCOM_FILE_WRITE;
			oflags |= fcom_file_cominfo_flags_o(h->cmd);
			r = core->file->open(h->out, h->cmd->output.ptr, oflags);
			if (r == FCOM_FILE_ERR) goto end;

			h->st = I_IN;
			continue;
		}

		case I_IN: {
			ffstr in;
			if (0 > (r = core->com->input_next(h->cmd, &in, NULL, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE)
					rc = 0;
				goto end;
			}

			if (0 != core->com->input_allowed(h->cmd, in, FCOM_COM_IA_AUTO))
				continue;

			uint iflags = fcom_file_cominfo_flags_i(h->cmd);
			r = core->file->open(h->in, in.ptr, iflags);
			if (r == FCOM_FILE_ERR) goto end;

			r = core->file->write_fmt(h->out, "%s:\n", in.ptr);
			if (r == FCOM_FILE_ERR) goto end;

			h->st = I_READ;
			continue;
		}

		case I_READ: {
			r = core->file->read(h->in, &h->data, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) {
				h->off = 0;

				r = core->file->write_fmt(h->out, "\n");
				if (r == FCOM_FILE_ERR) goto end;

				h->st = I_IN;
				continue;
			}

			uint flags = 16 | FFMEM_PRINT_ZEROSPACE;
			ffsize n = ffmem_print(NULL, 0, h->data.ptr, h->data.len, h->off, flags);
			ffvec_grow(&h->buf, n, 1);
			h->buf.len = ffmem_print(h->buf.ptr, h->buf.cap, h->data.ptr, h->data.len, h->off, flags);
			r = core->file->write(h->out, *(ffstr*)&h->buf, -1);
			h->buf.len = 0;
			if (r == FCOM_FILE_ERR) goto end;

			h->off += h->data.len;
			continue;
		}
		}
	}

end:
	{
	fcom_cominfo *cmd = h->cmd;
	hex_close(h);
	core->com->complete(cmd, rc);
	}
}

static void hex_signal(fcom_op *op, uint signal)
{
	struct hex *h = op;
	FFINT_WRITEONCE(h->stop, 1);
}

static const fcom_operation fcom_op_hex = {
	hex_create, hex_close,
	hex_run, hex_signal,
	hex_help,
};

FCOM_MOD_DEFINE(hex, fcom_op_hex, core)
