/** fcom: .bmp r/w
2023, Simon Zolin */

static void bmp_free(struct pic *p)
{
	bmpread_close(&p->bmpr);
	ffmem_zero_obj(&p->bmpr);
	bmpwrite_close(&p->bmpw);
	ffmem_zero_obj(&p->bmpw);
}

static int bmp_read(struct pic *p, ffstr *input, ffstr *output)
{
	// bmpread_open(&p->bmpr);
	int r = bmpread_process(&p->bmpr, input, output);
	switch (r) {
	case BMPREAD_HEADER: {
		const struct bmp_info *bi = bmpread_info(&p->bmpr);
		fcom_dbglog("%u/%u %u"
			, bi->width, bi->height, bi->bpp);

		p->in_info.width = bi->width;
		p->in_info.height = bi->height;
		p->in_info.format = _PIC_BGR | bi->bpp;
		if (bi->bpp == 32)
			p->in_info.format |= _PIC_ALPHA1;
		return 'head';
	}

	case BMPREAD_LINE:
		break;

	case BMPREAD_DONE:
		p->r_done = 1;
		return 0;

	case BMPREAD_SEEK:
		p->in_off = bmpread_seek_offset(&p->bmpr);
		return 'more';
	case BMPREAD_MORE:
		return 'more';

	case BMPREAD_ERROR:
		fcom_errlog("bmpread_process(): %s", bmpread_error(&p->bmpr));
		return 'erro';
	}

	fcom_dbglog("read line %u", (int)bmpread_line(&p->bmpr));
	return 0;
}

static int bmp_write(struct pic *p, ffstr *input, ffstr *output)
{
	if (!p->writer_opened) {
		if (!(p->out_info.format & _PIC_BGR)) {
			p->out_info.format = _PIC_BGR | (p->in_info.format & 0xff);
			if ((p->in_info.format & 0xff) == 32)
				p->out_info.format |= _PIC_ALPHA1;
			return 'conv';
		}

		p->writer_opened = 1;
		bmpwrite_info bi = {};
		bi.width = p->out_info.width;
		bi.height = p->out_info.height;
		bi.bpp = p->out_info.format & 0xff;
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
			return 0;

		case BMPWRITE_MORE:
			return 'more';

		case BMPWRITE_DONE:
			return 'done';

		case BMPWRITE_ERROR:
			fcom_errlog("bmpwrite_process(): %s", bmpwrite_error(&p->bmpw));
			return 'erro';
		}
	}
}
