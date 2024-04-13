/** fcom: .ico reader
2018, Simon Zolin */

/*
icoread_open
icoread_close
icoread_read
icoread_fileinfo
icoread_errstr
*/

/*
HDR ENTRY... DATA...
*/

#pragma once
#include <ffbase/vector.h>

struct icoread_file {
	ffuint width, height;
	ffuint bpp;
	ffuint size;
	ffuint offset;
};

typedef struct icoread {
	ffuint state, nxstate;
	const char *errmsg;

	ffuint gather_len;
	ffvec buf;
	ffstr chunk;

	ffslice ents; // struct ico_ent[]
	struct icoread_file info;
	ffuint icons_total;
	ffuint off;
	ffuint size;
	ffuint file_fmt;
	ffuint bmp_hdr_size;
} icoread;

enum ICOREAD_R {
	ICOREAD_ERROR,
	ICOREAD_MORE,

	ICOREAD_FILEINFO,
	ICOREAD_HEADER,

	/** Need input data at a new offset (returned by icoread_offset()) */
	ICOREAD_SEEK,

	ICOREAD_FILEFORMAT,
	ICOREAD_DATA,
	ICOREAD_FILEDONE,
};

/** Open .ico reader. */
static inline int icoread_open(icoread *c)
{
	return 0;
}

/** Close .ico reader. */
static inline void icoread_close(icoread *c)
{
	ffslice_free(&c->ents);
	ffvec_free(&c->buf);
}

static inline const char* icoread_errstr(icoread *c)
{
	return c->errmsg;
}

struct ico_hdr {
	ffbyte res[2];
	ffbyte type[2];
	ffbyte num[2];
};

struct ico_ent {
	ffbyte width; //0: 256
	ffbyte height;
	ffbyte colors;
	ffbyte res;
	ffbyte unused[2];
	ffbyte bpp[2];
	ffbyte size[4];
	ffbyte file_offset[4];
};

/** Fill in icoread_file object. */
static void _ico_f_info(struct icoread_file *f, const struct ico_ent *e)
{
	f->width = (e->width != 0) ? e->width : 256;
	f->height = (e->height != 0) ? e->height : 256;
	f->bpp = ffint_le_cpu16_ptr(e->bpp);
	f->size = ffint_le_cpu32_ptr(e->size);
	f->offset = ffint_le_cpu32_ptr(e->file_offset);
}

enum ICOREAD_FILEFMT {
	ICOREAD_UNKNOWN,
	ICOREAD_BMP,
	ICOREAD_PNG,
};

/** Detect file format. */
static int _icor_filefmt(icoread *c, const ffstr *d)
{
	static const ffbyte png_sign[] = { 0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A };
	if (ffstr_eq(d, png_sign, sizeof(png_sign)))
		return ICOREAD_PNG;

	if (ffint_le_cpu32_ptr(d->ptr + 4) == c->info.width) {
		c->bmp_hdr_size = ffint_le_cpu32_ptr(d->ptr);
		return ICOREAD_BMP;
	}

	return ICOREAD_UNKNOWN;
}

static void _icor_gather(icoread *c, ffuint nxstate, ffsize len)
{
	c->state = 1 /*R_GATHER*/,  c->nxstate = nxstate;
	c->gather_len = len;
	c->buf.len = 0;
}

static int _icor_err(icoread *c, const char *msg)
{
	c->errmsg = msg;
	return ICOREAD_ERROR;
}

/* .ico reading algorithm:
. Read header (ICOREAD_FILEINFO)
  User calls icoread_fileinfo() to get file info.
. Finish reading header (ICOREAD_HEADER)

. After icoread_readfile() has been called by user, seek to file data (ICOREAD_SEEK)
. Read file header and detect format (ICOREAD_FILEFORMAT)
. Read file data (ICOREAD_DATA, ICOREAD_FILEDONE)
*/

/** Process data.
Return enum ICOREAD_R. */
static inline int icoread_read(icoread *c, ffstr *input, ffstr *output)
{
	enum {
		R_INIT, R_GATHER = 1, R_ERR = 2,
		R_HDR, R_ENTS, R_SEEK = 5, R_FMT, R_DATA_CACHED, R_DATA,
	};
	int r;

	for (;;) {
		switch (c->state) {

		case R_INIT:
			_icor_gather(c, R_HDR, sizeof(struct ico_hdr));
			continue;

		case R_GATHER:
			r = ffstr_gather((ffstr*)&c->buf, &c->buf.cap, input->ptr, input->len, c->gather_len, &c->chunk);
			if (r == -1)
				return _icor_err(c, "memory allocation");
			ffstr_shift(input, r);
			if (c->chunk.len == 0)
				return ICOREAD_MORE;
			c->state = c->nxstate;
			c->buf.len = 0;
			continue;

		case R_HDR: {
			const struct ico_hdr *h = (void*)c->chunk.ptr;
			ffuint t = ffint_le_cpu16_ptr(h->type);
			if (t != 1)
				return _icor_err(c, "invalid ico hdr type");
			c->icons_total = ffint_le_cpu16_ptr(h->num);
			if (c->icons_total == 0)
				return ICOREAD_HEADER;
			_icor_gather(c, R_ENTS, c->icons_total * sizeof(struct ico_ent));
			continue;
		}

		case R_ENTS:
			if (c->chunk.len == 0) {
				c->state = R_ERR;
				return ICOREAD_HEADER;
			}

			if (c->ents.len == 0) {
				if (NULL == ffstr_dupstr((ffstr*)&c->ents, &c->chunk))
					return _icor_err(c, "memory allocation");
				c->ents.len = c->chunk.len / sizeof(struct ico_ent);
			}

			_ico_f_info(&c->info, (void*)c->chunk.ptr);
			ffstr_shift(&c->chunk, sizeof(struct ico_ent));
			return ICOREAD_FILEINFO;

		case R_SEEK:
			if (c->size <= 8)
				return _icor_err(c, "data too small");
			_icor_gather(c, R_FMT, 8);
			return ICOREAD_SEEK;

		case R_FMT:
			c->file_fmt = _icor_filefmt(c, &c->chunk);
			c->state = R_DATA_CACHED;
			return ICOREAD_FILEFORMAT;

		case R_DATA_CACHED:
			*output = c->chunk;
			c->size -= c->chunk.len;
			c->off += c->chunk.len;
			c->state = R_DATA;
			return ICOREAD_DATA;

		case R_DATA: {
			if (c->size == 0) {
				c->state = R_ERR;
				return ICOREAD_FILEDONE;
			}
			if (input->len == 0)
				return ICOREAD_MORE;
			ffsize n = ffmin(input->len, c->size);
			c->size -= n;
			c->off += n;
			ffstr_set(output, input->ptr, n);
			ffstr_shift(input, n);
			return ICOREAD_DATA;
		}

		case R_ERR:
			return _icor_err(c, "invalid usage");
		}
	}
}

/** Get the current file information (ICOREAD_FILEINFO). */
static inline const struct icoread_file* icoread_fileinfo(icoread *c)
{
	return &c->info;
}

/** Start reading file data specified by 0-based index.
User gets this file info by icoread_fileinfo(). */
void icoread_readfile(icoread *c, ffuint idx)
{
	if (idx >= c->ents.len) {
		c->state = 2 /*R_ERR*/;
		return;
	}

	const struct ico_ent *e = ffslice_itemT(&c->ents, idx, struct ico_ent);
	_ico_f_info(&c->info, e);

	c->off = ffint_le_cpu32_ptr(e->file_offset);
	c->size = ffint_le_cpu32_ptr(e->size);
	c->state = 5 /*R_SEEK*/;
}

/** Get file offset to seek to (ICOREAD_SEEK). */
#define icoread_offset(c)  ((c)->off)

/** Get data format (ICOREAD_FILEFORMAT).
Return enum ICOREAD_FILEFMT. */
#define icoread_fileformat(c)  ((c)->file_fmt)

/** Get size of bmp header (ICOREAD_FILEFORMAT, ICOREAD_BMP). */
#define icoread_bmphdr_size(c)  ((c)->bmp_hdr_size)
