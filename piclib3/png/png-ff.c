/** libpng wrapper
2016, Simon Zolin */

#include "png-ff.h"
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <png.h>

/*
libpng uses callbacks for I/O and error handling, e.g.:

	png_read() <-> png_read_row() (libpng) ... <-> r_read_data()

	                                                     longjmp()
	png_read() -> png_read_row() (libpng) ... -> error() --------> png_read()
*/


typedef struct ffstr {
	char *ptr;
	size_t len;
} ffstr;

typedef struct ffarr {
	char *ptr;
	size_t len;
	size_t cap;
} ffarr;

static void* arr_grow(ffarr *a, size_t n)
{
	void *p;
	n += a->len;
	if (a->cap >= n)
		return a->ptr;

	if (NULL == (p = realloc(a->ptr, n)))
		return NULL;
	a->ptr = p;
	a->cap = n;
	return a->ptr;
}

static void* arr_append(ffarr *a, const void *d, size_t n)
{
	if (NULL == arr_grow(a, n))
		return NULL;
	memcpy(a->ptr + a->len, d, n);
	a->len += n;
	return a->ptr;
}

static size_t ffmin(size_t a, size_t b)
{
	return (a < b) ? a : b;
}


struct png_err {
	jmp_buf jmp;
	ffarr errbuf;
};

const char* png_errstr(void *p)
{
	struct png_err *e = p;

	if (e == NULL)
		return "memory allocation error";

	return (e->errbuf.ptr != NULL) ? e->errbuf.ptr : "";
}

/** Fatal error handler.
Save error message and jump back to the function which called libpng. */
static void error(png_struct *png, const char *msg)
{
	struct png_err *e = png_get_error_ptr(png);
	arr_append(&e->errbuf, msg, strlen(msg) + 1);

	longjmp(e->jmp, 1);
}

static void warning(png_struct *png, const char *msg)
{
	struct png_err *e = png_get_error_ptr(png);
	if (e->errbuf.len != 0)
		arr_append(&e->errbuf, " : ", 3);
	arr_append(&e->errbuf, msg, strlen(msg) + 1);
}


void* mem_alloc(png_struct *png, size_t sz)
{
	//png_writer *p = png_get_mem_ptr(png);
	return malloc(sz);
}

void mem_free(png_struct *png, void *ptr)
{
	//png_writer *p = png_get_mem_ptr(png);
	free(ptr);
}


struct png_reader {
	struct png_err e;
	unsigned int state;
	unsigned int line;
	unsigned int height;
	unsigned int npasses;
	ffstr in;
	ffarr inbuf;

	png_struct *png;
	png_info *pnginfo;
	png_info *pngendinfo;
};

static void r_read_data(png_struct *png, unsigned char *data, size_t length)
{
	struct png_reader *p = png_get_io_ptr(png);

	if (p->in.len < length)
		error(png, "need more data");

	memcpy(data, p->in.ptr, length);
	p->in.ptr += length,  p->in.len -= length;
}

int png_open(struct png_reader **pp, const void *data, size_t *len, struct png_conf *conf)
{
	struct png_reader *p = *pp;

	if (p == NULL) {
		if (NULL == (p = calloc(1, sizeof(struct png_reader))))
			return -1;

		if (NULL == (p->png = png_create_read_struct_2(PNG_LIBPNG_VER_STRING
			, &p->e, &error, &warning
			, NULL, &mem_alloc, &mem_free)))
			goto err;

		if (NULL == (p->pnginfo = png_create_info_struct(p->png)))
			goto err;

		if (NULL == (p->pngendinfo = png_create_info_struct(p->png)))
			goto err;

		if (NULL == arr_grow(&p->inbuf, conf->total_size))
			goto err;

		png_set_read_fn(p->png, p, &r_read_data);
		*pp = p;
	}

	unsigned int n = ffmin(*len, conf->total_size - p->inbuf.len);
	arr_append(&p->inbuf, data, n);
	*len = n;
	if (p->inbuf.len != conf->total_size)
		return 0;
	p->in.ptr = p->inbuf.ptr,  p->in.len = p->inbuf.len;

	if (0 != setjmp(p->e.jmp))
		return -1;

	png_read_info(p->png, p->pnginfo);

	int bits, color, ilace;
	png_get_IHDR(p->png, p->pnginfo, &conf->width, &conf->height, &bits, &color, &ilace, NULL, NULL);

	switch (color) {
	case PNG_COLOR_TYPE_RGB:
		conf->bpp = 24;
		break;
	case PNG_COLOR_TYPE_RGB_ALPHA:
		conf->bpp = 32;
		png_set_alpha_mode(p->png, PNG_ALPHA_PNG, PNG_DEFAULT_sRGB);
		break;
	default:
		error(p->png, "unsupported color format");
	}

	if (bits != 8)
		error(p->png, "unsupported bit depth");

	switch (ilace) {
	case PNG_INTERLACE_NONE:
		break;
	case PNG_INTERLACE_ADAM7:
		p->npasses = png_set_interlace_handling(p->png) - 1;
		break;
	default:
		error(p->png, "unsupported interlace");
	}

	if (conf->width * conf->bpp / 8 != png_get_rowbytes(p->png, p->pnginfo))
		error(p->png, "row size mismatch");

	p->height = conf->height;
	return 1;

err:
	png_rfree(p);
	return -1;
}

void png_rfree(struct png_reader *p)
{
	png_destroy_read_struct(&p->png, &p->pnginfo, &p->pngendinfo);
	free(p->inbuf.ptr);
	free(p->e.errbuf.ptr);
	free(p);
}

enum { R_READ, R_FIN };

int png_read(struct png_reader *p, const void *data, size_t *len, void *line)
{
	int r;

	p->e.errbuf.len = 0;
	if (0 != setjmp(p->e.jmp))
		return -1;

	for (;  p->npasses > 0;  p->npasses--) {
		for (;  p->line != p->height;  p->line++) {
			png_read_row(p->png, line, NULL);
		}
		p->line = 0;
	}

	switch (p->state) {
	case R_READ:
		png_read_row(p->png, line, NULL);
		if (++p->line == p->height)
			p->state = R_FIN;
		r = 1;
		break;

	case R_FIN:
		png_read_end(p->png, p->pngendinfo);
		r = PNG_RDONE;
		break;
	}

	return r;
}


struct png_writer {
	struct png_err e;
	unsigned int state;
	unsigned int line;
	unsigned int height;
	ffarr out;

	png_struct *png;
	png_info *pnginfo;
};

static void write_data(png_struct *png, unsigned char *data, size_t length)
{
	struct png_writer *p = png_get_io_ptr(png);
	if (NULL == arr_append(&p->out, data, length))
		error(png, "memory allocation error");
}

static void flush_data(png_struct *png)
{}

int png_create(struct png_writer **pp, struct png_conf *conf)
{
	struct png_writer *p;

	if (NULL == (p = calloc(1, sizeof(struct png_writer))))
		return -1;

	if (NULL == (p->png = png_create_write_struct_2(PNG_LIBPNG_VER_STRING
		, &p->e, &error, &warning
		, NULL, &mem_alloc, &mem_free)))
		goto err;

	if (NULL == (p->pnginfo = png_create_info_struct(p->png)))
		goto err;

	if (NULL == arr_grow(&p->out, 16 * 1024))
		goto err;

	*pp = p;

	png_set_compression_level(p->png, conf->complevel);
	if (conf->comp_bufsize != 0)
		png_set_compression_buffer_size(p->png, conf->comp_bufsize);

	if (0 != setjmp(p->e.jmp))
		return -1;

	int color;
	switch (conf->bpp) {
	case 24:
		color = PNG_COLOR_TYPE_RGB;
		break;
	case 32:
		color = PNG_COLOR_TYPE_RGB_ALPHA;
		break;
	default:
		error(p->png, "unsupported color format");
		return -1;
	}

	png_set_IHDR(p->png, p->pnginfo, conf->width, conf->height, 8, color
		, PNG_INTERLACE_NONE, PNG_COMPRESSION_TYPE_DEFAULT, PNG_FILTER_TYPE_DEFAULT);
	p->height = conf->height;

	png_set_write_fn(p->png, p, &write_data, &flush_data);
	return 0;

err:
	png_wfree(p);
	return -1;
}

void png_wfree(struct png_writer *p)
{
	png_destroy_write_struct(&p->png, &p->pnginfo);
	free(p->out.ptr);
	free(p->e.errbuf.ptr);
	free(p);
}

enum { W_START, W_DATA, W_MORE, W_FIN, W_DONE };

int png_write(struct png_writer *p, const void *line, const void **data)
{
	int r;

	switch (p->state) {

	case W_MORE:
		p->state = W_DATA;
		return 0;

	case W_DONE:
		return PNG_RDONE;
	}

	p->e.errbuf.len = 0;
	if (0 != setjmp(p->e.jmp))
		return -1;

	switch (p->state) {

	case W_START:
		png_write_info(p->png, p->pnginfo);
		p->state = W_DATA;
		// break

	case W_DATA:
		png_write_row(p->png, line);

		p->state = W_MORE;
		if (++p->line == p->height)
			p->state = W_FIN;

		if (p->out.len == 0) {
			p->state = W_DATA;
			return 0;
		}
		break;

	case W_FIN:
		png_write_end(p->png, p->pnginfo);
		p->state = W_DONE;
		break;
	}

	*data = p->out.ptr;
	r = p->out.len;
	p->out.len = 0;
	return r;
}
