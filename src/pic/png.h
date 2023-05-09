/** fcom: .png r/w
2023, Simon Zolin */

static void png_free(struct pic *p)
{
	if (p->pngr != NULL) {
		png_rfree(p->pngr);
		p->pngr = NULL;
	}
	if (p->pngw != NULL) {
		png_wfree(p->pngw);
		p->pngw = NULL;
	}
}

static int pic_png_read(struct pic *p, ffstr *input, ffstr *output)
{
	int r;
	ffsize n;
	if (!p->reader_opened) {
		struct png_conf conf = {};
		n = input->len;
		conf.total_size = fffileinfo_size(&p->ifi);
		r = png_open(&p->pngr, input->ptr, &n, &conf);
		if (r == PNG_RMORE) {
			return 'more';
		} else if (r < 0) {
			fcom_errlog("png_open: (%d) %s", r, png_errstr(p->pngr));
			return 'erro';
		}
		ffstr_shift(input, n);

		p->reader_opened = 1;
		fcom_dbglog("%u/%u %u"
			, conf.width, conf.height, conf.bpp);

		p->in_line_size = conf.width * conf.bpp / 8;
		ffvec_alloc(&p->png_buf, p->in_line_size, 1);

		p->in_info.width = conf.width;
		p->in_info.height = conf.height;
		p->in_info.format = conf.bpp;
		return 'head';
	}

	n = input->len;
	r = png_read(p->pngr, input->ptr, &n, p->png_buf.ptr);
	ffstr_shift(input, n);
	if (r == PNG_RMORE) {
		return 'more';
	} else if (r == PNG_RDONE) {
		p->r_done = 1;
		return 0;
	} else if (r < 0) {
		fcom_errlog("png_read: (%d) %s", r, png_errstr(p->pngr));
		return 'erro';
	}

	ffstr_set(output, p->png_buf.ptr, p->in_line_size);
	return 0;
}

static int pic_png_write(struct pic *p, ffstr *input, ffstr *output)
{
	if (p->pngw == NULL) {
		uint fmt = p->out_info.format;
		if (!(fmt == PIC_RGB || fmt == PIC_RGBA)) {
			p->out_info.format = ((p->in_info.format & 0xff) == 32) ? PIC_RGBA : PIC_RGB;
			return 'conv';
		}

		struct png_conf conf = {};
		conf.width = p->out_info.width;
		conf.height = p->out_info.height;
		conf.bpp = p->out_info.format & 0xff;
		conf.complevel = p->png_comp;
		if (0 != png_create(&p->pngw, &conf)) {
			fcom_errlog("png_create");
			return 'erro';
		}
	}

	if (input->len == 0)
		return 'more';
	FCOM_ASSERT(input->len == p->out_info.width * (p->out_info.format & 0xff) / 8);

	const void *ptr;
	int r = png_write(p->pngw, input->ptr, &ptr);
	if (r == PNG_RMORE)
		return 'more';
	else if (r == PNG_RDONE)
		return 'done';
	else if (r < 0) {
		fcom_errlog("png_write: (%d) %s", r, png_errstr(p->pngw));
		return 'erro';
	}

	ffstr_set(output, ptr, r);
	return 0;
}
