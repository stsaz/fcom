/** Icon reader.
2018, Simon Zolin
*/

/*
HDR ENTRY... DATA...
*/

#pragma once
#include <FFOS/string.h>
#include <ffbase/vector.h>

struct ffico_file {
	ffuint width, height;
	ffuint format; //enum FFPIC_FMT
	ffuint size;
	ffuint offset;
};

typedef struct ffico {
	ffuint state, nxstate;
	ffuint err;

	ffuint gathlen;
	ffvec buf;
	ffstr chunk;

	ffslice ents; // struct ico_ent[]
	struct ffico_file info;
	ffuint icons_total;
	ffuint off;
	ffuint size;
	ffuint filefmt;
	ffuint bmphdr_size;

	ffstr input;
	ffstr out;
} ffico;

enum FFICO_R {
	FFICO_ERR,
	FFICO_MORE,

	FFICO_FILEINFO,
	FFICO_HDR,

	FFICO_SEEK,
	FFICO_FILEFORMAT,
	FFICO_DATA,
	FFICO_FILEDONE,
};

/** Open .ico reader. */
FF_EXTERN int ffico_open(ffico *c);

/** Close .ico reader. */
FF_EXTERN void ffico_close(ffico *c);

/** Set input data. */
#define ffico_input(c, data, size)  ffstr_set(&(c)->input, data, size)

/** Process data.
Return enum FFICO_R. */
FF_EXTERN int ffico_read(ffico *c);

/** Get the current file information (FFICO_FILEINFO). */
static inline const struct ffico_file* ffico_fileinfo(ffico *c)
{
	return &c->info;
}

/** Start reading file data specified by 0-based index.
User gets this file info by ffico_fileinfo(). */
FF_EXTERN void ffico_readfile(ffico *c, ffuint idx);

/** Get file offset to seek to (FFICO_SEEK). */
#define ffico_offset(c)  ((c)->off)

/** Get output file data (FFICO_DATA). */
#define ffico_data(c)  ((c)->out)

enum FFICO_FILEFMT {
	FFICO_UKN,
	FFICO_BMP,
	FFICO_PNG,
};

/** Get data format (FFICO_FILEFORMAT).
Return enum FFICO_FILEFMT. */
#define ffico_fileformat(c)  ((c)->filefmt)

/** Get size of bmp header (FFICO_FILEFORMAT, FFICO_BMP). */
#define ffico_bmphdr_size(c)  ((c)->bmphdr_size)
