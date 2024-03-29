/**
2016, Simon Zolin
*/

#include <pic/util/bmp.h>
#include <pic/util/pic.h>
#include <FFOS/error.h>


enum COMP {
	COMP_NONE,
	COMP_BITFIELDS = 3,
};

struct bmp_hdr {
//file header:
	byte bm[2]; //"BM"
	byte filesize[4];
	byte reserved[4];
	byte headersize[4];

//bitmap header:
	byte infosize[4];
	byte width[4];
	byte height[4];
	byte planes[2];
	byte bpp[2];

	byte compression[4]; //enum COMP
	byte sizeimage[4];
	byte xscale[4];
	byte yscale[4];
	byte colors[4];
	byte clrimportant[4];
};

struct bmp_hdr4 {
	byte mask_rgba[4*4];
	byte cstype[4];
	byte red_xyz[4*3];
	byte green_xyz[4*3];
	byte blue_xyz[4*3];
	byte gamma_rgb[4*3];
};


static const char* const errs[] = {
	"unsupported format", //FFBMP_EFMT
	"incomplete input line", //FFBMP_ELINE
	"invalid format", //FFBMP_EINV
	"unsupported compression method", //FFBMP_ECOMP
	"unsupported ver.4 header", //FFBMP_EHDR4
};

const char* ffbmp_errstr(void *_b)
{
	ffbmp *b = _b;
	switch (b->e) {
	case FFBMP_ESYS:
		return fferr_strp(fferr_last());
	}
	return errs[(uint)b->e - 1];
}

#define ERR(b, r) \
	(b)->e = (r),  FFBMP_ERR


enum { R_GATHER, R_HDR, R_HDR4, R_SEEK, R_DATA, R_DONE };

static void GATHER(ffbmp *b, uint nxstate, ffsize len)
{
	b->state = R_GATHER,  b->nxstate = nxstate;
	b->gather_size = len;
}

void ffbmp_open(ffbmp *b)
{
	GATHER(b, R_HDR, sizeof(struct bmp_hdr));
}

void ffbmp_close(ffbmp *b)
{
	ffvec_free(&b->inbuf);
}

void ffbmp_region(const ffbmp_pos *pos)
{

}

/** Read header. */
static int bmp_hdr_read(ffbmp *b, const void *data)
{
	const struct bmp_hdr *h = (void*)data;
	if (!!memcmp(h->bm, "BM", 2))
		return ERR(b, FFBMP_EINV);
	b->dataoff = ffint_le_cpu32_ptr(h->headersize);
	if (b->dataoff < sizeof(struct bmp_hdr))
		return ERR(b, FFBMP_EFMT);
	b->info.width = ffint_le_cpu32_ptr(h->width);
	b->info.height = ffint_le_cpu32_ptr(h->height);
	uint bpp = ffint_le_cpu16_ptr(h->bpp);
	b->linesize_o = b->info.width * bpp / 8;
	b->linesize = ff_align_ceil2(b->linesize_o, 4);
	uint comp = ffint_le_cpu32_ptr(h->compression);

	switch (bpp) {
	case 24:
		if (comp != COMP_NONE)
			return ERR(b, FFBMP_ECOMP);
		b->info.format = FFPIC_BGR;
		break;
	case 32:
		if (comp != COMP_BITFIELDS)
			return ERR(b, FFBMP_ECOMP);
		if (b->dataoff < sizeof(struct bmp_hdr) + sizeof(struct bmp_hdr4))
			return ERR(b, FFBMP_EFMT);
		b->info.format = FFPIC_ABGR;
		GATHER(b, R_HDR4, sizeof(struct bmp_hdr4));
		return FFBMP_MORE;
	default:
		return ERR(b, FFBMP_EFMT);
	}

	b->state = R_SEEK;
	return FFBMP_HDR;
}

int ffbmp_read(ffbmp *b)
{
	ffssize r;

	for (;;) {
	switch (b->state) {

	case R_GATHER:
		r = ffstr_gather((ffstr*)&b->inbuf, &b->inbuf.cap, b->data.ptr, b->data.len, b->gather_size, &b->chunk);
		if (r == -1)
			return ERR(b, FFBMP_ESYS);
		ffstr_shift(&b->data, r);
		if (b->chunk.len == 0)
			return FFBMP_MORE;
		b->inbuf.len = 0;
		b->state = b->nxstate;
		continue;

	case R_HDR:
		r = bmp_hdr_read(b, b->chunk.ptr);
		if (r == FFBMP_MORE)
			continue;
		return r;

	case R_HDR4: {
		const struct bmp_hdr4 *h4 = (void*)b->chunk.ptr;
		if (0x000000ff != ffint_be_cpu32_ptr(h4->mask_rgba)
			|| 0x0000ff00 != ffint_be_cpu32_ptr(h4->mask_rgba + 4)
			|| 0x00ff0000 != ffint_be_cpu32_ptr(h4->mask_rgba + 8)
			|| 0xff000000 != ffint_be_cpu32_ptr(h4->mask_rgba + 12)
			|| !!memcmp(h4->cstype, "BGRs", 4))
			return ERR(b, FFBMP_EHDR4);
		b->state = R_SEEK;
		return FFBMP_HDR;
	}

	case R_SEEK:
		b->seekoff = b->dataoff + (b->info.height - b->line - 1) * b->linesize;
		GATHER(b, R_DATA, b->linesize);
		return FFBMP_SEEK;

	case R_DATA:
		ffstr_set(&b->rgb, b->chunk.ptr, b->linesize_o);

		b->state = R_SEEK;
		if (++b->line == b->info.height)
			b->state = R_DONE;
		return FFBMP_DATA;

	case R_DONE:
		return FFBMP_DONE;
	}
	}
}


enum { W_HDR, W_MORE, W_SEEK, W_DATA, W_PAD };

int ffbmp_create(ffbmp_cook *b, ffpic_info *info)
{
	switch (info->format) {
	case FFPIC_BGR:
	case FFPIC_ABGR:
		break;
	default:
		info->format = (ffpic_bits(info->format) == 32) ? FFPIC_ABGR : FFPIC_BGR;
		return FFBMP_EFMT;
	}

	b->info = *info;
	b->state = W_HDR;
	return 0;
}

void ffbmp_wclose(ffbmp_cook *b)
{
	ffvec_free(&b->buf);
}

/** Write .bmp header. */
static int bmp_hdr_write(ffbmp_cook *b, void *dst)
{
	struct bmp_hdr *h = dst;
	ffmem_zero_obj(h);
	ffmem_copy(h->bm, "BM", 2);
	*(ffuint*)h->width = ffint_le_cpu32(b->info.width);
	*(ffuint*)h->height = ffint_le_cpu32(b->info.height);
	*(ffuint*)h->bpp = ffint_le_cpu32(ffpic_bits(b->info.format));
	*(ffushort*)h->planes = ffint_le_cpu16(1);
	*(ffuint*)h->sizeimage = ffint_le_cpu32(b->info.height * b->linesize);

	uint hdrsize = sizeof(struct bmp_hdr);
	if (b->info.format == FFPIC_ABGR) {
		hdrsize = sizeof(struct bmp_hdr) + sizeof(struct bmp_hdr4);
		*(ffuint*)h->compression = ffint_le_cpu32(COMP_BITFIELDS);
		struct bmp_hdr4 *h4 = (void*)(h + 1);
		ffmem_zero_obj(h4);
		*(ffuint*)h4->mask_rgba = ffint_be_cpu32(0x000000ff);
		*(ffuint*)(h4->mask_rgba+4) = ffint_be_cpu32(0x0000ff00);
		*(ffuint*)(h4->mask_rgba+8) = ffint_be_cpu32(0x00ff0000);
		*(ffuint*)(h4->mask_rgba+12) = ffint_be_cpu32(0xff000000);
		ffmem_copy(h4->cstype, "BGRs", 4);
	}

	*(ffuint*)h->infosize = ffint_le_cpu32(hdrsize - 14);
	*(ffuint*)h->headersize = ffint_le_cpu32(hdrsize);
	*(ffuint*)h->filesize = ffint_le_cpu32(hdrsize + b->info.height * b->linesize);

	return hdrsize;
}

int ffbmp_write(ffbmp_cook *b)
{
	for (;;) {
	switch (b->state) {

	case W_HDR: {
		uint bpp = ffpic_bits(b->info.format);
		b->linesize = ff_align_ceil2(b->info.width * bpp / 8, 4);
		b->linesize_o = b->info.width * bpp / 8;

		if (NULL == ffvec_alloc(&b->buf, sizeof(struct bmp_hdr) + sizeof(struct bmp_hdr4), 1))
			return ERR(b, FFBMP_ESYS);

		int r = bmp_hdr_write(b, (void*)b->buf.ptr);
		b->dataoff = r;
		ffstr_set(&b->data, b->buf.ptr, r);
		b->state = W_SEEK;
		return FFBMP_DATA;
	}

	case W_SEEK:
		b->state = W_DATA;
		if (b->input_reverse)
			continue;
		b->seekoff = b->dataoff + (b->info.height - b->line - 1) * b->linesize;
		return FFBMP_SEEK;

	case W_DATA:
		if (b->rgb.len < b->linesize_o) {
			if (b->rgb.len != 0)
				return ERR(b, FFBMP_ELINE);
			return FFBMP_MORE;
		}

		ffstr_set(&b->data, b->rgb.ptr, b->linesize_o);
		ffstr_shift(&b->rgb, b->linesize_o);
		b->state = W_PAD;
		return FFBMP_DATA;

	case W_PAD:
		if (b->linesize == b->linesize_o) {
			b->state = W_MORE;
			break;
		}
		ffmem_zero(b->buf.ptr, b->linesize - b->linesize_o);
		ffstr_set(&b->data, b->buf.ptr, b->linesize - b->linesize_o);
		b->state = W_MORE;
		return FFBMP_DATA;

	case W_MORE:
		if (++b->line == b->info.height)
			return FFBMP_DONE;
		b->state = W_SEEK;
		break;
	}
	}
}
