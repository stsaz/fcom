/** fcom: extract files
2024, Simon Zolin */

static const char* extract_help()
{
	return "\
Extract files.\n\
Usage:\n\
  `fcom extract` INPUT... [OPTIONS]\n\
\n\
OPTIONS:\n\
\n\
        `--minsize` N     Minimum file size\n\
";
}

#include <fcom.h>
#include <avpack/mkv-read.h>

static const fcom_core *core;

struct extract {
	fcom_cominfo cominfo;

	uint state;
	fcom_cominfo *cmd;
	uint stop;
	fcom_file_obj *fi, *fo;
	ffstr data, idata, odata;
	ffvec buf;
	char *oname;
	uint counter;
	const char *oext;

	size_t png_start, webp_start, id3v2_start, mkv_start;

	uint minsize;
};

static int args_parse(struct extract *x, fcom_cominfo *cmd)
{
	#define O(member)  (void*)FF_OFF(struct extract, member)
	static const struct ffarg args[] = {
		{ "--minsize",			'u',	O(minsize) },
		{}
	};
	#undef O
	if (core->com->args_parse(cmd, args, x, FCOM_COM_AP_INOUT))
		return -1;

	return 0;
}

static void extract_close(fcom_op *op)
{
	struct extract *x = op;
	core->file->destroy(x->fi);
	core->file->destroy(x->fo);
	ffvec_free(&x->buf);
	ffmem_free(x->oname);
	ffmem_free(x);
}

static fcom_op* extract_create(fcom_cominfo *cmd)
{
	struct extract *x = ffmem_new(struct extract);
	x->cmd = cmd;

	if (args_parse(x, cmd))
		goto end;

	struct fcom_file_conf fc = {};
	fc.buffer_size = cmd->buffer_size;
	x->fi = core->file->create(&fc);
	x->fo = core->file->create(&fc);

	return x;

end:
	extract_close(x);
	return NULL;
}

static size_t extract_png_find(struct extract *x, ffstr in, ffstr *out, size_t i, uint state)
{
	static const char png_sign[] = "\x89\x50\x4E\x47\x0D\x0A\x1A\x0A";
	static const char png_end[] = "\x00\x00\x00\x00" "IEND" "\xAE\x42\x60\x82";

	ssize_t r;

	switch (state) {
	case 0:
		r = ffstr_find(&in, png_sign, FFS_LEN(png_sign));
		x->png_start = r;
		if (r >= 0) {
			uint64 off = in.ptr + r - (char*)x->buf.ptr;
			fcom_verblog("PNG header   @%xU", off);
			return ffmin(r, i);
		}
		return i;

	case 1: {
		if (i < x->png_start)
			return i;

		ffstr s = in;
		ffstr_shift(&s, x->png_start);
		r = ffstr_find(&s, png_end, FFS_LEN(png_end));
		if (r >= 0) {
			uint64 off = in.ptr + r - (char*)x->buf.ptr;
			fcom_verblog(" PNG footer   @%xU", off);
			r += FFS_LEN(png_end);
		}
		ffstr_set(out, in.ptr + x->png_start, r);
		ffstr_shift(&x->idata, x->png_start + r);
		x->oext = "png";
		return 0;
	}
	}
	return 0;
}

static size_t extract_webp_find(struct extract *x, ffstr in, ffstr *out, size_t i, uint state)
{
	switch (state) {
	case 0: {
		ssize_t r = ffstr_find(&in, "RIFF", 4);
		x->webp_start = r;
		if (r >= 0) {
			uint64 off = in.ptr + r - (char*)x->buf.ptr;
			fcom_verblog("WEBP header   @%xU", off);
			return ffmin(r, i);
		}

		return i;
	}

	case 1: {
		if (i < x->webp_start)
			return i;

		ffstr s = in;
		ffstr_shift(&s, x->webp_start);
		uint n = ffint_le_cpu32_ptr(s.ptr + 4);
		size_t r = 8 + n;
		if (r > s.len)
			r = s.len;

		uint64 off = in.ptr + r - (char*)x->buf.ptr;
		fcom_verblog(" WEBP ends   @%xU", off);

		ffstr_set(out, in.ptr + x->webp_start, r);
		ffstr_shift(&x->idata, x->webp_start + r);
		x->oext = "webp";
		return 0;
	}
	}
	return 0;
}

static size_t extract_mkv_find(struct extract *x, ffstr in, ffstr *out, size_t i, uint state)
{
	switch (state) {
	case 0: {
		ssize_t r = ffstr_find(&in, "\x1a\x45\xdf\xa3", 4);
		x->mkv_start = r;
		if (r >= 0) {
			uint64 off = in.ptr + r - (char*)x->buf.ptr;
			fcom_verblog("MKV header   @%xU", off);
			return ffmin(r, i);
		}

		break;
	}

	case 1: {
		if (i < x->mkv_start)
			return i;

		ffstr s = in;
		ffstr_shift(&s, x->mkv_start + 4);

		uint64 r;
		int n = mkv_varint(s.ptr, s.len, &r);
		if (n <= 0
			|| n + r > s.len)
			goto err;
		ffstr_shift(&s, n + r);

		if (4 > s.len
			|| ffmem_cmp(s.ptr, "\x18\x53\x80\x67", 4))
			goto err;
		ffstr_shift(&s, 4);
		n = mkv_varint(s.ptr, s.len, &r);
		if (n <= 0
			|| n + r > s.len)
			goto err;
		ffstr_shift(&s, n + r);

		r = s.ptr - (in.ptr + x->mkv_start);

		uint64 off = in.ptr + r - (char*)x->buf.ptr;
		fcom_verblog(" MKV ends   @%xU", off);

		ffstr_set(out, in.ptr + x->mkv_start, r);
		ffstr_shift(&x->idata, x->mkv_start + r);
		x->oext = "mkv";
		return 0;
	}
	}
	return i;

err:
	fcom_errlog("bad MKV data");
	return -2;
}

static size_t extract_id3v2_find(struct extract *x, ffstr in, ffstr *out, size_t i, uint state)
{
	switch (state) {
	case 0: {
		x->id3v2_start = ~0ULL;
		ffstr s = in;
		for (;;) {
			ssize_t i = ffstr_find(&s, "ID3", 3);
			if (i < 0)
				break;
			ffstr_shift(&s, i);
			if ((s.ptr[3] == 2 || s.ptr[3] == 3 || s.ptr[3] == 4)
				&& s.ptr[4] == 0) {

				size_t r = s.ptr - in.ptr;
				uint64 off = in.ptr + r - (char*)x->buf.ptr;
				fcom_verblog(" MP3 header   @%xU", off);

				x->id3v2_start = r;
				return r;
			}
			ffstr_shift(&s, 1);
		}
		return i;
	}

	case 1: {
		if (i < x->id3v2_start)
			return i;

		ffstr s = in;
		ffstr_shift(&s, x->id3v2_start + 1);
		for (;;) {
			ssize_t i = ffstr_find(&s, "ID3", 3);
			if (i < 0)
				break;
			ffstr_shift(&s, i);
			if ((s.ptr[3] == 2 || s.ptr[3] == 3 || s.ptr[3] == 4)
				&& s.ptr[4] == 0) {
				size_t r = s.ptr - in.ptr;
				uint64 off = in.ptr + r - (char*)x->buf.ptr;
				fcom_verblog(" MP3 ends   @%xU", off);
				ffstr_set(out, in.ptr + x->id3v2_start, r);
				ffstr_shift(&x->idata, x->id3v2_start + r);
				x->oext = "mp3";
				return 0;
			}
			ffstr_shift(&s, 1);
		}
		return i;
	}
	}
	return 0;
}

typedef size_t (*find_t)(struct extract *x, ffstr in, ffstr *out, size_t i, uint state);
static const find_t f_find[] = {
	extract_png_find,
	extract_webp_find,
	extract_mkv_find,
	extract_id3v2_find,
};

static int extract_analyze(struct extract *x, ffstr *out)
{
	size_t r = ~0ULL;

	const find_t *f;
	FF_FOREACH(f_find, f) {
		r = (*f)(x, x->idata, out, r, 0);
		if (r == 0)
			break;
	}
	if (r == ~0ULL)
		return -1;
	FF_FOREACH(f_find, f) {
		r = (*f)(x, x->idata, out, r, 1);
		if (r == 0)
			break;
		if (r == (size_t)-2)
			return -1;
	}
	if (r == ~0ULL)
		return -1;

	return 0;
}

static void extract_run(fcom_op *op)
{
	struct extract *x = op;
	int r, rc = 1;
	enum { I_INPUT, I_READ, I_ANALYZE, I_OUT, };

	while (!FFINT_READONCE(x->stop)) {
		switch (x->state) {
		case I_INPUT: {
			ffstr in;
			if (0 > (r = core->com->input_next(x->cmd, &in, NULL, 0))) {
				if (r == FCOM_COM_RINPUT_NOMORE)
					rc = 0;
				goto end;
			}

			if (core->com->input_allowed(x->cmd, in, FCOM_COM_IA_AUTO))
				continue;

			uint iflags = fcom_file_cominfo_flags_i(x->cmd);
			r = core->file->open(x->fi, in.ptr, iflags);
			if (r == FCOM_FILE_ERR) goto end;

			x->state = I_READ;
			continue;
		}

		case I_READ:
			r = core->file->read(x->fi, &x->data, -1);
			if (r == FCOM_FILE_ERR) goto end;
			if (r == FCOM_FILE_EOF) {
				x->idata = *(ffstr*)&x->buf;
				x->state = I_ANALYZE;
				continue;
			}
			ffvec_addstr(&x->buf, &x->data);
			continue;

		case I_ANALYZE:
			if (extract_analyze(x, &x->odata)) {
				x->idata.len = 0;
				core->file->close(x->fi);
				x->buf.len = 0;
				x->state = I_INPUT;
				continue;
			}
			x->state = I_OUT;
			continue;

		case I_OUT: {
			if (x->odata.len < x->minsize) {
				core->file->close(x->fo);
				x->odata.len = 0;
				x->state = I_ANALYZE;
				continue;
			}

			uint oflags = FCOM_FILE_WRITE;
			oflags |= fcom_file_cominfo_flags_o(x->cmd);
			ffmem_free(x->oname);
			x->oname = ffsz_allocfmt("%u.%s", ++x->counter, x->oext);
			r = core->file->open(x->fo, x->oname, oflags);
			if (r == FCOM_FILE_ERR) goto end;

			r = core->file->write(x->fo, x->odata, -1);
			if (r == FCOM_FILE_ERR) goto end;

			core->file->close(x->fo);
			x->odata.len = 0;
			x->state = I_ANALYZE;
			continue;
		}
		}
	}

end:
	{
	fcom_cominfo *cmd = x->cmd;
	extract_close(x);
	core->com->complete(cmd, rc);
	}
}

static void extract_signal(fcom_op *op, uint signal)
{
	struct extract *x = op;
	FFINT_WRITEONCE(x->stop, 1);
}

static const fcom_operation fcom_op_extract = {
	extract_create, extract_close,
	extract_run, extract_signal,
	extract_help,
};

FCOM_MOD_DEFINE(extract, fcom_op_extract, core)
