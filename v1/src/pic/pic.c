/** fcom: Convert images
2023, Simon Zolin */

#include <fcom.h>
#include <avpack/bmp-read.h>
#include <avpack/bmp-write.h>

static const fcom_core *core;

struct pic {
	uint st;
	fcom_cominfo *cmd;
	ffstr name, base;
	ffstr idata, tdata, odata;
	bmpread bmpr;
	struct bmp_info bi;
	bmpwrite bmpw;
	fcom_file_obj *in, *out;
	int64 in_off, out_off;
	ffvec buf;
	uint stop;
	uint out_opened :1;
	uint r_done :1;
	uint bmpw_opened :1;
};

static const char* pic_help()
{
	return "\
Convert images.\n\
Usage:\n\
  fcom pic INPUT -o OUTPUT\n\
";
}

static int args_parse(struct pic *p, fcom_cominfo *cmd)
{
	static const ffcmdarg_arg args[] = {
		{}
	};
	return core->com->args_parse(cmd, args, p);
}

static void pic_reset(struct pic *p)
{
	bmpread_close(&p->bmpr);
	bmpwrite_close(&p->bmpw);
	p->bmpw_opened = 0;
	p->out_opened = 0;
}

static void pic_close(fcom_op *op)
{
	struct pic *p = op;
	core->file->destroy(p->in);
	core->file->destroy(p->out);
	pic_reset(p);
	ffvec_free(&p->buf);
	ffmem_free(p);
}

static fcom_op* pic_create(fcom_cominfo *cmd)
{
	struct pic *p = ffmem_new(struct pic);
	p->cmd = cmd;

	if (0 != args_parse(p, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	p->in = core->file->create(&fc);
	p->out = core->file->create(&fc);

	ffsize cap = (cmd->buffer_size != 0) ? cmd->buffer_size : 64*1024;
	ffvec_alloc(&p->buf, cap, 1);
	return p;

end:
	pic_close(p);
	return NULL;
}

static int bmp_read(struct pic *p, ffstr *input, ffstr *output)
{
	// bmpread_open(&p->bmpr);
	for (;;) {
		int r = bmpread_process(&p->bmpr, input, output);
		switch (r) {
		case BMPREAD_SEEK:
			p->in_off = bmpread_seek_offset(&p->bmpr);
			return r;

		case BMPREAD_HEADER: {
			const struct bmp_info *bi = bmpread_info(&p->bmpr);
			fcom_dbglog("%u/%u %u"
				, bi->width, bi->height, bi->bpp);
			p->bi = *bi;
			continue;
		}

		case BMPREAD_LINE:
			fcom_dbglog("read line %u", (int)bmpread_line(&p->bmpr));
			return r;

		case BMPREAD_DONE:
			p->r_done = 1;
			return r;
		case BMPREAD_MORE:
			return r;

		case BMPREAD_ERROR:
			fcom_errlog("bmpread_process(): %s", bmpread_error(&p->bmpr));
			return r;
		}
	}
}

static int bmp_write(struct pic *p, ffstr *input, ffstr *output)
{
	if (!p->bmpw_opened) {
		p->bmpw_opened = 1;
		bmpwrite_info bi = {};
		bi.width = p->bi.width;
		bi.height = p->bi.height;
		bi.bpp = p->bi.bpp;
		bmpwrite_create(&p->bmpw, &bi, 0);
	}

	for (;;) {
		int r = bmpwrite_process(&p->bmpw, input, output);
		switch (r) {
		case BMPWRITE_SEEK:
			p->out_off = bmpwrite_seek_offset(&p->bmpw);
			continue;

		case BMPWRITE_DATA:
			fcom_dbglog("write line %u", (int)bmpwrite_line(&p->bmpw));
			return r;

		case BMPWRITE_MORE:
		case BMPWRITE_DONE:
			return r;

		case BMPWRITE_ERROR:
			fcom_errlog("bmpwrite_process(): %s", bmpwrite_error(&p->bmpw));
			return r;
		}
	}
}

static void pic_run(fcom_op *op)
{
	struct pic *p = op;
	int r, rc = 1;
	enum { I_IN, I_READ, I_INPUT, I_OUTPUT, I_OUT_OPEN, I_WRITE, };

	while (!FFINT_READONCE(p->stop)) {
		switch (p->st) {

		case I_IN: {
			if (0 > (r = core->com->input_next(p->cmd, &p->name, &p->base, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE) {
					rc = 0;
				}
				goto end;
			}

			uint flags = fcom_file_cominfo_flags_i(p->cmd);
			flags |= FCOM_FILE_READ;
			r = core->file->open(p->in, p->name.ptr, flags);
			if (r == FCOM_FILE_ERR) goto end;

			fffileinfo fi = {};
			r = core->file->info(p->in, &fi);
			if (r == FCOM_FILE_ERR) goto end;

			if ((p->base.len == 0 || p->cmd->recursive)
				&& fffile_isdir(fffileinfo_attr(&fi))) {
				fffd fd = core->file->fd(p->in, FCOM_FILE_ACQUIRE);
				core->com->input_dir(p->cmd, fd);
			}

			if (0 != core->com->input_allowed(p->cmd, p->name))
				continue;

			p->st = I_INPUT;
			continue;
		}

		case I_READ:
			r = core->file->read(p->in, &p->idata, p->in_off);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) goto end;
			p->in_off += p->idata.len;

			p->st = I_INPUT;
			// fallthrough

		case I_INPUT:
			r = bmp_read(p, &p->idata, &p->tdata);
			switch (r) {
			case BMPREAD_LINE:
			case BMPREAD_DONE:
				p->st = I_OUTPUT;
				continue;

			case BMPREAD_SEEK:
			case BMPREAD_MORE:
				break;

			case BMPREAD_ERROR:
				goto end;
			}
			p->st = I_READ;
			continue;

		case I_OUTPUT:
			r = bmp_write(p, &p->tdata, &p->odata);
			switch (r) {
			case BMPWRITE_DATA:
				p->st = I_WRITE;
				if (!p->out_opened) {
					p->out_opened = 1;
					p->st = I_OUT_OPEN;
				}
				break;

			case BMPWRITE_MORE:
				p->st = I_INPUT;
				break;

			case BMPWRITE_DONE:
				core->file->close(p->out);
				pic_reset(p);
				p->st = I_IN;
				break;

			case BMPWRITE_ERROR:
				goto end;
			}
			continue;

		case I_OUT_OPEN: {
			uint oflags = FCOM_FILE_WRITE;
			oflags |= fcom_file_cominfo_flags_o(p->cmd);
			r = core->file->open(p->out, p->cmd->output.ptr, oflags);
			if (r == FCOM_FILE_ERR) goto end;
			p->st = I_WRITE;
		}
			// fallthrough

		case I_WRITE:
			r = core->file->write(p->out, p->odata, p->out_off);
			if (r == FCOM_FILE_ERR) goto end;
			p->out_off += p->odata.len;

			p->st = I_OUTPUT;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = p->cmd;
	pic_close(p);
	core->com->complete(cmd, rc);
	}
}

static void pic_signal(fcom_op *op, uint signal)
{
	struct pic *p = op;
	FFINT_WRITEONCE(p->stop, 1);
}

static const fcom_operation fcom_op_pic = {
	pic_create, pic_close,
	pic_run, pic_signal,
	pic_help,
};


static void pic_init(const fcom_core *_core) { core = _core; }
static void pic_destroy() {}
static const fcom_operation* pic_provide_op(const char *name)
{
	if (ffsz_eq(name, "pic"))
		return &fcom_op_pic;
	return NULL;
}
FF_EXP const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	pic_init, pic_destroy, pic_provide_op,
};
