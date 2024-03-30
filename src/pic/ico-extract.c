/** fcom: extract files from .ico
2023, Simon Zolin */

static const char* icoex_help()
{
	return "\
Extract files from .ico.\n\
Usage:\n\
  `fcom ico-extract` INPUT... [-C OUTPUT_DIR]\n\
";
}

#include <fcom.h>
#include <util/ico-read.h>
#include <util/pixel-conv.h>
#include <util/stream.h>
#include <avpack/bmp-write.h>
#include <ffsys/path.h>

static const fcom_core *core;

struct icoex {
	fcom_cominfo cominfo;

	uint state;
	fcom_cominfo *cmd;
	icoread rico;
	ffstr iname;
	ffstr icodata, picdata;
	fcom_file_obj *in, *out;
	uint64 in_off;
	char *oname;
	ffstream stm;
	ffvec buf;
	uint idx, num;
	uint data_size, skip_bytes;
	uint stop;
	uint del_on_close :1;
	uint bmp :1;
	uint conv :1;
};

static int icoex_args_parse(struct icoex *c, fcom_cominfo *cmd)
{
	static const struct ffarg args[] = {
		{}
	};
	int r = core->com->args_parse(cmd, args, c, FCOM_COM_AP_INOUT);
	if (r != 0)
		return r;

	if (cmd->chdir.len == 0)
		ffstr_dupz(&cmd->chdir, ".");

	return 0;
}

static void icoex_close(fcom_op *op)
{
	struct icoex *c = op;
	icoread_close(&c->rico);
	core->file->destroy(c->in);
	if (c->del_on_close)
		core->file->del(c->oname, 0);
	core->file->destroy(c->out);
	ffmem_free(c->oname);
	ffvec_free(&c->buf);
	ffstream_free(&c->stm);
	ffmem_free(c);
}

static fcom_op* icoex_create(fcom_cominfo *cmd)
{
	struct icoex *c = ffmem_new(struct icoex);
	c->cmd = cmd;

	if (0 != icoex_args_parse(c, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fcom_cmd_file_conf(&fc, cmd);
	c->in = core->file->create(&fc);
	c->out = core->file->create(&fc);
	return c;

end:
	icoex_close(c);
	return NULL;
}

static char* icoex_out_fn(struct icoex *c)
{
	if (c->cmd->output.len != 0)
		return ffsz_dupstr(&c->cmd->output);

	const char *ext = "bmp";
	switch (icoread_fileformat(&c->rico)) {
	case ICOREAD_BMP:
		ext = "bmp"; break;
	case ICOREAD_PNG:
		ext = "png"; break;
	default:
		fcom_errlog("unknown file #%u format", c->idx);
		return NULL;
	}

	ffstr name;
	ffpath_splitpath_str(c->iname, NULL, &name);
	ffpath_splitname_str(name, &name, NULL);

	return ffsz_allocfmt("%S%c%S-%u.%s"
		, &c->cmd->chdir, FFPATH_SLASH, &name, c->idx, ext);
}

static int icoex_read(struct icoex *c, ffstr *input, ffstr *output)
{
	for (;;) {
		int r = icoread_read(&c->rico, input, output);
		switch (r) {

		case ICOREAD_MORE:
		case ICOREAD_HEADER:
		case ICOREAD_FILEDONE:
			return r;

		case ICOREAD_FILEFORMAT:
			c->bmp = (icoread_fileformat(&c->rico) == ICOREAD_BMP);
			return r;

		case ICOREAD_FILEINFO: {
			c->num++;
			const struct icoread_file *f = icoread_fileinfo(&c->rico);
			fcom_verblog("icon #%u: dimensions:%ux%u  bpp:%u  size:%u  offset:%xu"
				, c->num, f->width, f->height, f->bpp, f->size, f->offset);
			continue;
		}

		case ICOREAD_DATA:
			if (c->skip_bytes != 0) {
				ffsize n = ffmin(c->skip_bytes, output->len);
				ffstr_shift(output, n);
				c->skip_bytes -= n;
				if (c->skip_bytes != 0)
					continue;
			}
			goto data;

		case ICOREAD_SEEK:
			c->in_off = icoread_offset(&c->rico);
			return ICOREAD_MORE;

		case ICOREAD_ERROR:
			fcom_errlog("icoread_read(): %s", icoread_errstr(&c->rico));
			return r;
		}
	}

data:
	if (c->data_size == 0)
		return ICOREAD_FILEDONE;

	ffsize n = ffmin(c->data_size, output->len);
	output->len = n;
	c->data_size -= n;
	return ICOREAD_DATA;
}

/** Make .bmp header */
static int icoex_bmp_hdr(struct icoex *c, ffstr *output)
{
	const struct icoread_file *f = icoread_fileinfo(&c->rico);
	bmpwrite_info bi = {
		.width = f->width,
		.height = f->height,
		.bpp = f->bpp,
	};

	bmpwrite bw = {};
	bmpwrite_create(&bw, &bi, 0);

	ffstr in = {};
	int r = bmpwrite_process(&bw, &in, output);
	if (r != BMPWRITE_DATA) return -1;

	c->skip_bytes = icoread_bmphdr_size(&c->rico);
	c->data_size = f->width * f->height * f->bpp / 8;
	return 0;
}

/** Convert BGRA -> ABGR */
static void icoex_conv(struct icoex *c, ffstr *input, ffstr *output)
{
	if (!c->conv) return;

	ffstr d;
	uint n = ffint_align_ceil2(input->len, 4);
	uint r = ffstream_gather(&c->stm, *input, n, &d);
	ffstr_shift(input, r);

	d = ffstream_view(&c->stm);
	if ((d.len % 4) != 0)
		d.len -= (d.len % 4);
	ffstream_consume(&c->stm, d.len);

	pic_convert(PIC_BGRA, d.ptr, PIC_ABGR, c->buf.ptr, d.len / 4);
	ffstr_set(output, c->buf.ptr, d.len);
}

static int icoex_next(struct icoex *c)
{
	int r;
	if (0 > (r = core->com->input_next(c->cmd, &c->iname, NULL, 0))) {
		if (r == FCOM_COM_RINPUT_NOMORE) {
			return 'done';
		}
		return 'erro';
	}

	uint flags = fcom_file_cominfo_flags_i(c->cmd);
	flags |= FCOM_FILE_READ;
	r = core->file->open(c->in, c->iname.ptr, flags);
	if (r == FCOM_FILE_ERR) return 'erro';

	fffileinfo fi = {};
	r = core->file->info(c->in, &fi);
	if (r == FCOM_FILE_ERR) return 'erro';

	if (0 != core->com->input_allowed(c->cmd, c->iname, fffile_isdir(fffileinfo_attr(&fi)))) {
		return 'skip';
	}

	if (fffile_isdir(fffileinfo_attr(&fi))) {
		fffd fd = core->file->fd(c->in, FCOM_FILE_ACQUIRE);
		core->com->input_dir(c->cmd, fd);
		return 'skip';
	}

	icoread_open(&c->rico);
	return 0;
}

static void icoex_run(fcom_op *op)
{
	struct icoex *c = op;
	int r, rc = 1;
	enum { I_IN, I_PROC, I_FILEREAD, I_NEXT, I_OUT_OPEN, I_WRITE };

	while (!FFINT_READONCE(c->stop)) {
		switch (c->state) {

		case I_IN:
			switch (icoex_next(c)) {
			case 'done':
				rc = 0; goto end;
			case 'erro':
				goto end;
			case 'skip':
				continue;
			}

			c->state = I_PROC;
			continue;

		case I_FILEREAD:
			r = core->file->read(c->in, &c->icodata, c->in_off);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(c->cmd);
				return;
			}
			if (r == FCOM_FILE_EOF) {
				fcom_errlog("incomplete archive");
				goto end;
			}
			c->in_off += c->icodata.len;
			c->state = I_PROC;
			// fallthrough

		case I_PROC:
			r = icoex_read(c, &c->icodata, &c->picdata);
			switch (r) {

			case ICOREAD_HEADER:
			case ICOREAD_FILEDONE:
				c->state = I_NEXT; continue;

			case ICOREAD_FILEFORMAT:
				c->state = I_OUT_OPEN; continue;

			case ICOREAD_DATA: break;

			case ICOREAD_MORE:
				c->state = I_FILEREAD; continue;

			case ICOREAD_ERROR: goto end;
			}

			icoex_conv(c, &c->picdata, &c->picdata);

			c->state = I_WRITE;
			continue;

		case I_NEXT:
			if (c->idx == c->num) {
				core->file->close(c->out);
				c->del_on_close = 0;
				c->state = I_IN;
				continue;
			}

			icoread_readfile(&c->rico, c->idx);
			c->idx++;

			c->state = I_PROC;
			continue;

		case I_OUT_OPEN: {
			ffmem_free(c->oname);
			if (NULL == (c->oname = icoex_out_fn(c))) {
				c->state = I_NEXT;
				continue;
			}

			uint flags = FCOM_FILE_WRITE;
			flags |= fcom_file_cominfo_flags_o(c->cmd);
			r = core->file->open(c->out, c->oname, flags);
			if (r == FCOM_FILE_ERR) goto end;

			c->del_on_close = !c->cmd->stdout && !c->cmd->test;
			c->state = I_PROC;

			c->data_size = (uint)-1;
			c->skip_bytes = 0;
			c->conv = 0;
			if (c->bmp) {
				if (0 != (r = icoex_bmp_hdr(c, &c->picdata)))
					goto end;

				const struct icoread_file *f = icoread_fileinfo(&c->rico);
				if (f->bpp == 32) {
					ffvec_realloc(&c->buf, 64*1024, 1);
					ffstream_realloc(&c->stm, 64*1024);
					c->conv = 1;
				}

				c->state = I_WRITE;
			}
			continue;
		}

		case I_WRITE:
			r = core->file->write(c->out, c->picdata, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_ASYNC) {
				core->com->async(c->cmd);
				return;
			}

			c->state = I_PROC;
			continue;
		}
	}

end:
	{
	fcom_cominfo *cmd = c->cmd;
	icoex_close(c);
	core->com->complete(cmd, rc);
	}
}

static void icoex_signal(fcom_op *op, uint signal)
{
	struct icoex *c = op;
	FFINT_WRITEONCE(c->stop, 1);
}

const fcom_operation fcom_op_icoex = {
	icoex_create, icoex_close,
	icoex_run, icoex_signal,
	icoex_help,
};

static void icoex_init(const fcom_core *_core) { core = _core; }
static void icoex_destroy() {}
extern const fcom_operation fcom_op_unicoex;
static const fcom_operation* icoex_provide_op(const char *name)
{
	if (ffsz_eq(name, "ico-extract"))
		return &fcom_op_icoex;
	return NULL;
}
FF_EXPORT const struct fcom_module fcom_module = {
	FCOM_VER, FCOM_CORE_VER,
	icoex_init, icoex_destroy, icoex_provide_op,
};
