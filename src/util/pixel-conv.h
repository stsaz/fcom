/** fcom: Convert pixel lines
2016, Simon Zolin */

enum PIC_FMT {
	PIC_RGB = 24,
	PIC_RGBA = 32,
	_PIC_BGR = 0x100,
	_PIC_ALPHA1 = 0x200,
	PIC_ABGR = 32 | _PIC_BGR | _PIC_ALPHA1,
	PIC_BGR = 24 | _PIC_BGR,
	PIC_BGRA = 32 | _PIC_BGR,
};

#define CASE(a, b)  (((a) << 8) | (b))

/** Convert pixels
in_fmt, out_fmt: enum PIC_FMT */
static inline int pic_convert(ffuint in_fmt, const void *_src, ffuint out_fmt, void *_dst, ffuint pixels)
{
	ffuint i;
	const ffbyte *in, *src = _src;
	ffbyte *o, *dst = _dst;
	ffuint alpha;

	switch (CASE(in_fmt, out_fmt)) {
	case CASE(PIC_RGB, PIC_BGR):
	case CASE(PIC_BGR, PIC_RGB):
		for (i = 0;  i != pixels;  i++) {
			dst[0] = src[2];
			dst[1] = src[1];
			dst[2] = src[0];
			src += 3;
			dst += 3;
		}
		break;

	case CASE(PIC_ABGR, PIC_RGBA):
	case CASE(PIC_RGBA, PIC_ABGR):
		for (i = 0;  i != pixels;  i++) {
			in = src + i * 4;
			o = dst + i * 4;
			*(ffuint*)o = ffint_bswap32(*(ffuint*)in);
		}
		break;

	case CASE(PIC_RGBA, PIC_BGRA):
	case CASE(PIC_BGRA, PIC_RGBA):
		for (i = 0;  i != pixels;  i++) {
			in = src + i * 4;
			o = dst + i * 4;
			o[0] = in[2];
			o[1] = in[1];
			o[2] = in[0];
			o[3] = in[3];
		}
		break;

	case CASE(PIC_BGRA, PIC_ABGR):
		for (i = 0;  i != pixels;  i++) {
			in = src + i * 4;
			o = dst + i * 4;
			o[0] = in[3];
			o[1] = in[0];
			o[2] = in[1];
			o[3] = in[2];
		}
		break;

	case CASE(PIC_ABGR, PIC_BGR):
		for (i = 0;  i != pixels;  i++) {
			in = src + i * 4;
			o = dst + i * 3;
			alpha = in[0];
			// apply alpha channel (black background)
			o[0] = in[1] * alpha / 255;
			o[1] = in[2] * alpha / 255;
			o[2] = in[3] * alpha / 255;
		}
		break;

	case CASE(PIC_ABGR, PIC_RGB):
		for (i = 0;  i != pixels;  i++) {
			in = src + i * 4;
			o = dst + i * 3;
			alpha = in[0];
			// apply alpha channel (black background)
			o[0] = in[3] * alpha / 255;
			o[1] = in[2] * alpha / 255;
			o[2] = in[1] * alpha / 255;
		}
		break;

	case CASE(PIC_RGBA, PIC_RGB):
	case CASE(PIC_BGRA, PIC_BGR):
		for (i = 0;  i != pixels;  i++) {
			in = src + i * 4;
			o = dst + i * 3;
			alpha = in[3];
			// apply alpha channel (black background)
			o[0] = in[0] * alpha / 255;
			o[1] = in[1] * alpha / 255;
			o[2] = in[2] * alpha / 255;
		}
		break;

	case CASE(PIC_RGBA, PIC_BGR):
	case CASE(PIC_BGRA, PIC_RGB):
		for (i = 0;  i != pixels;  i++) {
			in = src + i * 4;
			o = dst + i * 3;
			alpha = in[3];
			// apply alpha channel (black background)
			o[0] = in[2] * alpha / 255;
			o[1] = in[1] * alpha / 255;
			o[2] = in[0] * alpha / 255;
		}
		break;

	default:
		return -1;
	}

	return 0;
}

#undef CASE
