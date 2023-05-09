/** libpng writer */

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
