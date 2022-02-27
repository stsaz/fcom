/** libjpeg wrapper
2016, Simon Zolin */

#include "jpeg-ff.h"
#include <setjmp.h>
#include <string.h>
#include <stdio.h>
#include <jpeglib.h>

/*
libjpeg uses callbacks for I/O and error handling, e.g.:

	jpeg_read() <-> jpeg_read_scanlines() (libjpeg) ... <-> r_fill_input_buffer()

	                                                                     longjmp()
	jpeg_read() -> jpeg_read_scanlines() (libjpeg) ... -> e_error_exit() --------> jpeg_read()
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

static size_t ffmin(size_t a, size_t b)
{
	return (a < b) ? a : b;
}


struct jpeg_err {
	struct jpeg_error_mgr jerr;
	jmp_buf jmp;
	ffarr errbuf;
};

const char* jpeg_errstr(void *j)
{
	struct jpeg_err *e = j;

	if (j == NULL)
		return "memory allocation error";

	return (e->errbuf.ptr != NULL) ? e->errbuf.ptr : "";
}

/** Fatal error handler.
Save error message and jump back to the function which called libjpeg. */
static void e_error_exit(struct jpeg_common_struct *jcom)
{
	struct jpeg_err *e = (void*)jcom->err;

	if (NULL != arr_grow(&e->errbuf, JMSG_LENGTH_MAX))
		e->jerr.format_message(jcom, e->errbuf.ptr);

	longjmp(e->jmp, 1);
}

/** Warning and trace messages handler. */
static void e_emit_message(struct jpeg_common_struct *jcom, int msg_level)
{}


struct jpeg_reader {
	struct jpeg_err e;
	unsigned int state;
	unsigned int line;

	struct jpeg_source_mgr jsrc;
	ffstr in;
	ffarr inbuf;
	unsigned int skipinput;

	struct jpeg_decompress_struct jd;
};

static void r_init_source(struct jpeg_decompress_struct *jd)
{}

/** Pass input data back to libjpeg.
Return 0 if there's no more data at this time.
 In this case we may still have some unprocessed data,
  copy it into temporary buffer, it will be returned next time. */
static boolean r_fill_input_buffer(struct jpeg_decompress_struct *jd)
{
	struct jpeg_reader *j = jd->client_data;

	if (j->inbuf.len != 0) {
		j->jsrc.next_input_byte = (void*)j->inbuf.ptr;
		j->jsrc.bytes_in_buffer = j->inbuf.len;
		j->inbuf.len = 0;
		return 1;
	}

	if (j->in.len == 0) {
		// in case 'next_input_byte' is within 'inbuf' it won't be invalidated here
		if (NULL == arr_grow(&j->inbuf, j->jsrc.bytes_in_buffer)) {
			j->e.errbuf.ptr = "memory allocation error";
			longjmp(j->e.jmp, 1);
		}

		memmove(j->inbuf.ptr, j->jsrc.next_input_byte, j->jsrc.bytes_in_buffer);
		j->inbuf.len = j->jsrc.bytes_in_buffer;
		j->jsrc.bytes_in_buffer = 0;
		return 0;
	}

	if (j->skipinput != 0) {
		unsigned int n = ffmin(j->skipinput, j->in.len);
		j->in.ptr += j->skipinput,  j->in.len -= j->skipinput;
		j->skipinput -= n;
		if (j->skipinput != 0)
			return 0;
	}

	j->jsrc.next_input_byte = (void*)j->in.ptr;
	j->jsrc.bytes_in_buffer = j->in.len;
	j->in.len = 0;
	return 1;
}

static void r_skip_input_data(struct jpeg_decompress_struct *jd, long num_bytes)
{
	struct jpeg_reader *j = jd->client_data;

	if (j->jsrc.bytes_in_buffer < (unsigned int)num_bytes) {
		// need to skip more data than we currently have
		j->skipinput = num_bytes - j->jsrc.bytes_in_buffer;
		j->jsrc.bytes_in_buffer = 0;
		return;
	}

	j->jsrc.next_input_byte += num_bytes;
	j->jsrc.bytes_in_buffer -= num_bytes;
}

static boolean r_resync_to_restart(struct jpeg_decompress_struct *jd, int desired)
{
	return jpeg_resync_to_restart(jd, desired);
}

static void r_term_source(struct jpeg_decompress_struct *jd)
{}

int jpeg_open(struct jpeg_reader **pj, const void *data, size_t *len, struct jpeg_conf *conf)
{
	struct jpeg_reader *j = *pj;
	unsigned int create = 0;

	if (j == NULL) {
		if (NULL == (j = calloc(1, sizeof(struct jpeg_reader))))
			return -1;
		*pj = j;
		create = 1;

		jpeg_std_error(&j->e.jerr);
		j->e.jerr.error_exit = &e_error_exit;
		j->e.jerr.emit_message = &e_emit_message;
		j->jd.err = &j->e.jerr;
	}

	if (0 != setjmp(j->e.jmp))
		return -1;

	if (create) {
		jpeg_create_decompress(&j->jd);

		j->jsrc.init_source = &r_init_source;
		j->jsrc.fill_input_buffer = &r_fill_input_buffer;
		j->jsrc.skip_input_data = &r_skip_input_data;
		j->jsrc.resync_to_restart = &r_resync_to_restart;
		j->jsrc.term_source = &r_term_source;
		j->jd.src = &j->jsrc;
		j->jd.client_data = j;
	}

	j->in.ptr = (void*)data,  j->in.len = *len;
	if (JPEG_SUSPENDED == jpeg_read_header(&j->jd, 1))
		return 0;

	conf->width = j->jd.image_width;
	conf->height = j->jd.image_height;
	return 1;
}

void jpeg_free(struct jpeg_reader *j)
{
	jpeg_destroy_decompress(&j->jd);
	free(j->inbuf.ptr);
	free(j->e.errbuf.ptr);
	free(j);
}

enum { R_START, R_READ, R_FIN };

int jpeg_read(struct jpeg_reader *j, const void *data, size_t *len, void *line)
{
	int r;

	if (0 != setjmp(j->e.jmp))
		return -1;

	j->in.ptr = (void*)data,  j->in.len = *len;

	switch (j->state) {
	case R_START:
		if (0 == jpeg_start_decompress(&j->jd))
			return 0;
		j->state = R_READ;
		// break

	case R_READ:
		if (1 != jpeg_read_scanlines(&j->jd, (void*)&line, 1))
			return 0;
		if (++j->line == j->jd.image_height)
			j->state = R_FIN;
		r = 1;
		break;

	case R_FIN:
		if (0 == jpeg_finish_decompress(&j->jd))
			return 0;
		r = JPEG_RDONE;
		break;
	}

	return r;
}


struct jpeg_writer {
	struct jpeg_err e;
	unsigned int state;
	unsigned int line;

	struct jpeg_destination_mgr jdest;
	ffstr out;
	unsigned int out_buf_size;

	struct jpeg_compress_struct jc;
};

static void w_init_destination(struct jpeg_compress_struct *jc)
{
	struct jpeg_writer *j = jc->client_data;
	j->jdest.next_output_byte = (void*)j->out.ptr;
	j->jdest.free_in_buffer = j->out_buf_size;
}

static int w_buf_filled(struct jpeg_writer *j)
{
	return j->jdest.next_output_byte - (unsigned char*)j->out.ptr;
}

/**
After the call to this function libjpeg returns back to its caller
 because there's not enough space in output buffer. */
static boolean w_empty_output_buffer(struct jpeg_compress_struct *jc)
{
	struct jpeg_writer *j = jc->client_data;
	j->out.len = w_buf_filled(j);
	w_init_destination(jc);
	return 0;
}

static void w_term_destination(struct jpeg_compress_struct *jc)
{
	struct jpeg_writer *j = jc->client_data;
	j->out.len = w_buf_filled(j);
}

int jpeg_create(struct jpeg_writer **pj, struct jpeg_conf *conf)
{
	struct jpeg_writer *j;

	if (NULL == (j = calloc(1, sizeof(struct jpeg_writer))))
		return -1;
	*pj = j;

	jpeg_std_error(&j->e.jerr);
	j->e.jerr.error_exit = &e_error_exit;
	j->e.jerr.emit_message = &e_emit_message;
	j->jc.err = &j->e.jerr;

	if (0 != setjmp(j->e.jmp))
		return -1;

	jpeg_create_compress(&j->jc);

	j->jdest.init_destination = &w_init_destination;
	j->jdest.empty_output_buffer = &w_empty_output_buffer;
	j->jdest.term_destination = &w_term_destination;
	j->jc.dest = &j->jdest;
	j->jc.client_data = j;

	j->jc.image_width = conf->width;
	j->jc.image_height = conf->height;
	j->jc.input_components = 3;
	j->jc.in_color_space = JCS_RGB;

	jpeg_set_defaults(&j->jc);
	jpeg_set_quality(&j->jc, conf->quality, 1 /*force_baseline*/);

	j->out_buf_size = conf->buf_size;
	return 0;
}

void jpeg_wfree(struct jpeg_writer *j)
{
	jpeg_destroy_compress(&j->jc);
	free(j->e.errbuf.ptr);
	free(j);
}

enum { W_START, W_DATA, W_FIN, W_DONE };

int jpeg_write(struct jpeg_writer *j, const void *line, void *data)
{
	if (0 != setjmp(j->e.jmp))
		return -1;

	j->out.ptr = data;

	switch (j->state) {

	case W_START:
		jpeg_start_compress(&j->jc, 1);
		j->state = W_DATA;
		// break

	case W_DATA:
		if (0 == jpeg_write_scanlines(&j->jc, (unsigned char**)&line, 1)) {
			if (j->out.len == 0) {
				j->e.errbuf.ptr = "output buffer is too small";
				return -1;
			}
			// output buffer is almost full: some data wasn't stored and it will be regenerated next time
			return j->out.len;
		}

		if (++j->line == j->jc.image_height)
			j->state = W_FIN;

		if (j->state != W_FIN)
			break;
		// break

	case W_FIN:
		jpeg_finish_compress(&j->jc);
		j->state = W_DONE;
		return j->out.len;

	case W_DONE:
		return JPEG_RDONE;
	}

	return 0;
}
