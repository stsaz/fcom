/** fcom: Convert images
2023, Simon Zolin */

static const char* pic_help()
{
	return "\
Convert images (.bmp/.jpg/.png).\n\
Usage:\n\
  `fcom pic` INPUT... -o OUTPUT\n\
\n\
OUTPUT\n\
      Output file name with extension (.bmp/.jpg/.png).\n\
      If only an extension is given, use source file name automatically.\n\
\n\
OPTIONS:\n\
    `-q`, `--jpeg-quality` INT\n\
                        Set JPEG quality: 1..100 (default: 85)\n\
        `--png-compression` INT\n\
                        Set PNG compression level: 0..9 (default: 9)\n\
    `-k`, `--skip-errors`\n\
                        Skip errors\n\
          `--delete-source`\n\
                        Delete source file after successful conversion\n\
";
}

#include <fcom.h>
#include <util/pixel-conv.h>
#include <avpack/bmp-read.h>
#include <avpack/bmp-write.h>
#include <../3pt-pic/jpeg-turbo/jpeg-ff.h>
#include <../3pt-pic/png/png-ff.h>
#include <ffsys/path.h>

static const fcom_core *core;
static void pic_run(fcom_op *op);

struct pic;
typedef int (*pic_io)(struct pic *p, ffstr *input, ffstr *output);

struct pic {
	fcom_cominfo cominfo;

	uint st;
	fcom_cominfo *cmd;
	ffstr name, basepath, inameonly;
	ffstr idata, tdata, odata;
	struct {
		uint format; // enum PIC_FMT
		uint width, height;
	} in_info, out_info;
	uint in_line_size, out_line_size;
	fcom_file_obj *in, *out;
	fffileinfo ifi;
	int64 in_off, out_off;
	ffvec buf;
	pic_io read, write;
	char *oname;
	ffstr autoname_ext;

	bmpread bmpr;
	bmpwrite bmpw;

	struct jpeg_reader *jpegr;
	struct jpeg_writer *jpegw;
	ffvec jpg_buf, jpg_wbuf;

	struct png_reader *pngr;
	struct png_writer *pngw;
	ffvec png_buf;

	uint stop;
	uint out_opened :1;
	uint r_done :1;
	uint writer_opened :1;
	uint reader_opened :1;
	uint conv :1;

	struct {
		u_char skip_errors;
		u_char delete_source;
		uint jpeg_quality;
		uint png_comp;
	} conf;
};

#include <pic/bmp.h>
#include <pic/jpg.h>
#include <pic/png.h>

static pic_io get_format_w(ffstr oext)
{
	pic_io io = bmp_write;
	if (ffstr_eqz(&oext, "jpg"))
		io = pic_jpg_write;
	else if (ffstr_eqz(&oext, "png"))
		io = pic_png_write;
	return io;
}

static pic_io get_format_r(ffstr iext)
{
	pic_io io = bmp_read;
	if (ffstr_eqz(&iext, "jpg"))
		io = pic_jpg_read;
	else if (ffstr_eqz(&iext, "png"))
		io = pic_png_read;
	return io;
}

static inline ffstr ffstr_sub(ffstr s, ffsize from, ffssize len)
{
	FF_ASSERT(from <= s.len);
	if (len == -1)
		len = s.len - from;
	FF_ASSERT(from + len <= s.len);
	ffstr r = FFSTR_INITN(s.ptr + from, len);
	return r;
}

#define O(member)  (void*)FF_OFF(struct pic, member)

static int args_parse(struct pic *p, fcom_cominfo *cmd)
{
	p->conf.jpeg_quality = 85;
	p->conf.png_comp = 9;

	static const struct ffarg args[] = {
		{ "--delete-source",	'1',	O(conf.delete_source) },
		{ "--jpeg-quality",		'u',	O(conf.jpeg_quality) },
		{ "--png-compression",	'u',	O(conf.png_comp) },
		{ "--skip-errors",		'1',	O(conf.skip_errors) },
		{ "-k",					'1',	O(conf.skip_errors) },
		{ "-q",					'u',	O(conf.jpeg_quality) },
		{}
	};
	int r = core->com->args_parse(cmd, args, p, FCOM_COM_AP_INOUT);
	if (r != 0)
		return r;

	if (!(p->conf.png_comp <= 9)) {
		fcom_fatlog("png-compression: must be 0..9");
		return -1;
	}

	if (!(p->conf.jpeg_quality > 0 && p->conf.jpeg_quality <= 100)) {
		fcom_fatlog("jpeg-quality: must be 1..100");
		return -1;
	}

	if (!cmd->output.len) {
		fcom_fatlog("Please set output file with `-o file.ext`");
		return -1;
	}

	ffstr oname, oext;
	ffpath_splitpath_str(cmd->output, NULL, &oname);
	if (0 == ffpath_splitname_str(oname, NULL, &oext)) {
		oext = ffstr_sub(oname, 1, -1);
		p->autoname_ext = oext;
		cmd->output.len = 0;
	}
	p->write = get_format_w(oext);

	return 0;
}

#undef O

static void pic_reset(struct pic *p)
{
	bmp_free(p);
	pic_jpeg_free(p);
	png_free(p);
	p->writer_opened = 0;
	p->reader_opened = 0;
	p->conv = 0;
	p->idata.len = 0;
	ffmem_zero_obj(&p->in_info);
	ffmem_zero_obj(&p->out_info);

	if (p->out != NULL) {
		core->file->close(p->out);
		fcom_verblog("%s", p->oname);
	}
	p->out_opened = 0;
	p->in_off = 0;
	p->out_off = 0;
}

static void pic_close(fcom_op *op)
{
	struct pic *p = op;
	pic_reset(p);
	core->file->destroy(p->in);
	core->file->destroy(p->out);
	ffvec_free(&p->buf);
	ffvec_free(&p->jpg_wbuf);
	ffvec_free(&p->jpg_buf);
	ffvec_free(&p->png_buf);
	ffmem_free(p->oname);
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

static int pic_conv(struct pic *p, ffstr *input, ffstr *output)
{
	if (p->in_info.format == p->out_info.format) {
		*output = *input;
		return 0;
	}

	if (0 != pic_convert(p->in_info.format, input->ptr, p->out_info.format, p->buf.ptr, p->out_info.width)) {
		fcom_errlog("unsupported pixel conversion");
		return -1;
	}
	ffstr_set(output, p->buf.ptr, p->out_line_size);
	return 0;
}

/** Prepare output file name */
static char* pic_oname(struct pic *p, ffstr in, ffstr base)
{
	ffstr dir = p->cmd->chdir, name = p->cmd->output, ext = p->autoname_ext;
	if (!p->cmd->output.len) {
		// [-C "dir"] -o ".ext"
		if (!base.len)
			base = in;
		ffstr nm;
		ffpath_splitpath_str(base, NULL, &nm);
		if (nm.len)
			ffstr_shift(&in, nm.ptr - base.ptr);
		name = in;
		ffpath_splitname_str(name, &name, NULL);

	} else {
		// -o "name.ext"
		// -C "dir" -o "name.ext"
	}

	ffsize path_len = (dir.len) ? 1 : 0;
	ffsize ext_len = (ext.len) ? 1 : 0;
	char *s = ffsz_allocfmt("%S%*c%S%*c%S"
		, &dir, path_len, FFPATH_SLASH
		, &name
		, ext_len, '.', &ext);

	fcom_dbglog("output file name: %s", s);
	return s;
}

static int pic_next(struct pic *p)
{
	int r;
	if (0 > (r = core->com->input_next(p->cmd, &p->name, &p->basepath, 0))) {
		if (r == FCOM_COM_RINPUT_NOMORE) {
			return 'done';
		}
		return 'erro';
	}

	uint flags = fcom_file_cominfo_flags_i(p->cmd);
	flags |= FCOM_FILE_READ;
	r = core->file->open(p->in, p->name.ptr, flags);
	if (r == FCOM_FILE_ERR) return 'erro';

	r = core->file->info(p->in, &p->ifi);
	if (r == FCOM_FILE_ERR) return 'erro';

	if (0 != core->com->input_allowed(p->cmd, p->name, fffile_isdir(fffileinfo_attr(&p->ifi))))
		return 'skip';

	if (fffile_isdir(fffileinfo_attr(&p->ifi))) {
		fffd fd = core->file->fd(p->in, FCOM_FILE_ACQUIRE);
		core->com->input_dir(p->cmd, fd);
		return 'skip';
	}

	ffstr iext;
	ffpath_splitpath_str(p->name, NULL, &iext);
	ffpath_splitname_str(iext, &p->inameonly, &iext);
	p->read = get_format_r(iext);
	return 0;
}

static void pic_trash_complete(void *param, int result)
{
	struct pic *p = (struct pic*)param;
	pic_run(p);
}

static void pic_trash_src(struct pic *p)
{
	fcom_cominfo *ci = core->com->create();
	ci->operation = ffsz_dup("trash");
	ci->overwrite = 1; // if trash doesn't work: delete

	ffstr *ps = ffvec_pushT(&ci->input, ffstr);
	char *sz = ffsz_dup(p->name.ptr);
	ffstr_setz(ps, sz);

	ci->test = p->cmd->test;

	ci->on_complete = pic_trash_complete;
	ci->opaque = p;
	fcom_dbglog("pic: trash: %s", p->name.ptr);
	core->com->run(ci);
}

static void pic_run(fcom_op *op)
{
	struct pic *p = op;
	int r, rc = 1;
	enum { I_IN, I_READ, I_INPUT, I_CONV, I_OUTPUT, I_OUT_OPEN, I_WRITE, };

	while (!FFINT_READONCE(p->stop)) {
		switch (p->st) {

		case I_IN:
			switch (pic_next(p)) {
			case 'done':
				rc = 0; goto end;
			case 'erro':
				goto end;
			case 'skip':
				continue;
			}

			p->st = I_INPUT;
			continue;

		case I_READ:
			r = core->file->read(p->in, &p->idata, p->in_off);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) goto end;
			p->in_off += p->idata.len;

			p->st = I_INPUT;
			// fallthrough

		case I_INPUT:
			r = p->read(p, &p->idata, &p->tdata);
			switch (r) {
			case 'head': {
				p->out_info.width = p->in_info.width;
				p->out_info.height = p->in_info.height;
				continue;
			}

			case 'more':
				p->st = I_READ;
				continue;

			case 'erro':
				if (p->conf.skip_errors) {
					pic_reset(p);
					p->st = I_IN;
					continue;
				}
				goto end;
			}

			p->st = I_OUTPUT;
			if (p->conv)
				p->st = I_CONV;
			continue;

		case I_CONV:
			r = pic_conv(p, &p->tdata, &p->tdata);
			if (r != 0) goto end;
			p->st = I_OUTPUT;
			continue;

		case I_OUTPUT:
			r = p->write(p, &p->tdata, &p->odata);
			switch (r) {
			case 'conv':
				p->conv = 1;
				p->out_line_size = p->out_info.width * (p->out_info.format & 0xff) / 8;
				ffvec_realloc(&p->buf, p->out_line_size, 1);
				p->st = I_CONV;
				continue;

			case 'more':
				p->st = I_INPUT;
				continue;

			case 'done':
				pic_reset(p);
				p->st = I_IN;
				if (p->conf.delete_source) {
					pic_trash_src(p);
					return;
				}
				continue;

			case 'erro': goto end;
			}

			p->st = I_WRITE;
			if (!p->out_opened) {
				p->out_opened = 1;
				p->st = I_OUT_OPEN;
			}
			continue;

		case I_OUT_OPEN: {
			uint oflags = FCOM_FILE_WRITE;
			oflags |= fcom_file_cominfo_flags_o(p->cmd);
			ffmem_free(p->oname);
			p->oname = pic_oname(p, p->name, p->basepath);
			r = core->file->open(p->out, p->oname, oflags);
			if (r == FCOM_FILE_ERR) {
				if (p->conf.skip_errors) {
					pic_reset(p);
					p->st = I_IN;
					continue;
				}
				goto end;
			}
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

FCOM_MOD_DEFINE(pic, fcom_op_pic, core)
