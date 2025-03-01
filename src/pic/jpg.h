/** fcom: .jpg r/w
2023, Simon Zolin */

static void pic_jpeg_free(struct pic *p)
{
	if (p->jpegr != NULL) {
		jpeg_free(p->jpegr);
		p->jpegr = NULL;
	}
	if (p->jpegw != NULL) {
		jpeg_wfree(p->jpegw);
		p->jpegw = NULL;
	}
}

static int pic_jpg_read(struct pic *p, ffstr *input, ffstr *output)
{
	int r;
	ffsize n;

	if (!p->reader_opened) {
		if (input->len == 0)
			return 'more';

		struct jpeg_conf conf = {};
		n = input->len;
		r = jpeg_open(&p->jpegr, input->ptr, &n, &conf);
		if (r == JPEG_RMORE) {
			return 'more';
		} else if (r < 0) {
			fcom_errlog("jpeg_open: (%d) %s", r, jpeg_errstr(p->jpegr));
			return 'erro';
		}
		ffstr_shift(input, n);

		p->reader_opened = 1;
		fcom_dbglog("%u/%u", conf.width, conf.height);

		p->in_line_size = conf.width * 24 / 8;
		ffvec_realloc(&p->jpg_buf, p->in_line_size, 1);

		p->in_info.width = conf.width;
		p->in_info.height = conf.height;
		p->in_info.format = PIC_RGB;
		return 'head';
	}

	n = input->len;
	r = jpeg_read(p->jpegr, input->ptr, &n, p->jpg_buf.ptr);
	ffstr_shift(input, n);
	if (r == JPEG_RMORE) {
		return 'more';
	} else if (r == JPEG_RDONE) {
		p->r_done = 1;
		return 0;
	} else if (r < 0) {
		fcom_errlog("jpeg_read: (%d) %s", r, jpeg_errstr(p->jpegr));
		return 'erro';
	}

	ffstr_set(output, p->jpg_buf.ptr, p->in_line_size);
	return 0;
}

static int pic_jpg_write(struct pic *p, ffstr *input, ffstr *output)
{
	if (p->jpegw == NULL) {
		if (p->out_info.format != PIC_RGB) {
			p->out_info.format = PIC_RGB;
			return 'conv';
		}

		struct jpeg_conf conf = {};
		conf.width = p->out_info.width;
		conf.height = p->out_info.height;
		conf.quality = p->conf.jpeg_quality;
		ffvec_realloc(&p->jpg_wbuf, 8*1024*1024, 1); // libjpeg may return JERR_CANT_SUSPEND if it's not large enough
		conf.buf_size = p->jpg_wbuf.cap;
		if (0 != jpeg_create(&p->jpegw, &conf))
			return 'erro';
	}

	if (input->len == 0)
		return 'more';
	FCOM_ASSERT(input->len == p->out_info.width * 24 / 8);

	int r = jpeg_write(p->jpegw, input->ptr, p->jpg_wbuf.ptr);
	if (r == JPEG_RMORE)
		return 'more';
	else if (r == JPEG_RDONE)
		return 'done';
	else if (r < 0) {
		fcom_errlog("jpeg_write: (%d) %s", r, jpeg_errstr(p->jpegw));
		return 'erro';
	}

	ffstr_set(output, p->jpg_wbuf.ptr, r);
	return 0;
}
