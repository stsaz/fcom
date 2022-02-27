/** BMP.
2016, Simon Zolin
*/

/*
FILEHDR (HDRV3 | HDRV4) (ROW#HEIGHT..ROW#1(BGR#1..BGR#WIDTH [PADDING:1..3]))
*/

#pragma once
#include <pic/util/pic.h>
#include <ffbase/vector.h>


enum FFBMP_E {
	FFBMP_EFMT = 1,
	FFBMP_ELINE,
	FFBMP_EINV,
	FFBMP_ECOMP,
	FFBMP_EHDR4,

	FFBMP_ESYS,
};


typedef struct ffbmp {
	//shared:
	uint e;

	uint state;
	uint nxstate;
	ffstr data;
	ffstr rgb;
	ffvec inbuf;
	ffstr chunk;
	uint linesize;
	uint linesize_o;
	uint line;
	uint dataoff;
	uint gather_size;
	uint64 seekoff;

	ffpic_info info;
} ffbmp;

enum FFBMP_R {
	FFBMP_ERR,
	FFBMP_MORE,
	FFBMP_DONE,
	FFBMP_HDR,
	FFBMP_DATA,
	FFBMP_SEEK,
};

FF_EXTERN const char* ffbmp_errstr(void *b);

FF_EXTERN void ffbmp_open(ffbmp *b);

FF_EXTERN void ffbmp_close(ffbmp *b);

typedef struct ffbmp_pos {
	uint x, y;
	uint width, height;
} ffbmp_pos;

FF_EXTERN void ffbmp_region(const ffbmp_pos *pos);

#define ffbmp_input(b, _data, len)  ffstr_set(&(b)->data, _data, len)
#define ffbmp_output(b)  (b)->rgb

FF_EXTERN int ffbmp_read(ffbmp *b);

/** Get input/output stream seek offset. */
#define ffbmp_seekoff(b)  ((b)->seekoff)

#define ffbmp_line(b)  ((b)->line)


typedef struct ffbmp_cook {
	//shared:
	uint e;

	uint state;
	uint linesize;
	uint linesize_o;
	uint dataoff;
	ffstr data;
	ffstr rgb;
	ffvec buf;
	uint line;
	uint64 seekoff;

	ffpic_info info;
	uint input_reverse :1; //input lines order is HEIGHT..1, as in .bmp
} ffbmp_cook;

/**
Return 0 on success;  enum FFBMP_E on error. */
FF_EXTERN int ffbmp_create(ffbmp_cook *b, ffpic_info *info);

FF_EXTERN void ffbmp_wclose(ffbmp_cook *b);

static FFINL uint ffbmp_wsize(ffbmp_cook *b)
{
	uint lnsize = b->info.width * (ffpic_bits(b->info.format) / 8);
	return b->info.height * ff_align_ceil2(lnsize, 4);
}

#define ffbmp_winput(b, data, len)  ffstr_set(&(b)->rgb, data, len)
#define ffbmp_woutput(b)  (b)->data

FF_EXTERN int ffbmp_write(ffbmp_cook *b);
