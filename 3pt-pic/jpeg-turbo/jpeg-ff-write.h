/** libjpeg writer */

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

	j->jc.err = j_err_init(&j->e);
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
	jpeg_set_quality(&j->jc, conf->quality, /*force_baseline*/ 1);

	j->out_buf_size = conf->buf_size;
	return 0;
}

void jpeg_wfree(struct jpeg_writer *j)
{
	jpeg_destroy_compress(&j->jc);
	j_err_free(&j->e);
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
				j_err_set(&j->e, "output buffer is too small");
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
