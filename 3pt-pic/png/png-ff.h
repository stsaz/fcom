/** libpng wrapper
2016, Simon Zolin */

#include <stdlib.h>

#ifdef WIN32
	#define _EXPORT  __declspec(dllexport)
#else
	#define _EXPORT  __attribute__((visibility("default")))
#endif

struct png_reader;
struct png_writer;

struct png_conf {
	unsigned int width;
	unsigned int height;
	unsigned int bpp;
	unsigned int complevel;
	unsigned int comp_bufsize;
	unsigned int total_size;
};

enum {
	PNG_RMORE,
	PNG_RDONE = -2,
};

#ifdef __cplusplus
extern "C" {
#endif

_EXPORT const char* png_errstr(void *p);


/** Read header.
conf: 'total_size' is required parameter.
p: must point to NULL object on the first call.
Return 1 on success;
 0 if more data is needed;
 <0 on error (note: *p may be allocated). */
_EXPORT int png_open(struct png_reader **p, const void *data, size_t *len, struct png_conf *conf);

_EXPORT void png_rfree(struct png_reader *p);

/** Read one line.
line: receives uncompressed data;  must be large enough for a full line.
Return 1 if line is ready;
 0 if more data is needed;
 -2 if done;  <0 on error. */
_EXPORT int png_read(struct png_reader *p, const void *data, size_t *len, void *line);


/** Prepare to write.
conf: 'width', 'height', 'complevel' are required parameters.
Return 0 on success;
 <0 on error (note: *p may be allocated). */
_EXPORT int png_create(struct png_writer **p, struct png_conf *conf);

_EXPORT void png_wfree(struct png_writer *p);

/** Write one line.
data: receives pointer to PNG data.
Return number of bytes written;
 0 if more data is needed;
 -2 if done;
 <0 on error. */
_EXPORT int png_write(struct png_writer *p, const void *line, const void **data);

#ifdef __cplusplus
}
#endif

#undef _EXPORT
