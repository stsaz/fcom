/** Read .md5 file

Format:
(SUM[32] [ *]NAME [CR]LF)...

2025, Simon Zolin */

#pragma once
#include <ffbase/string.h>

static int md5sum_read(ffstr *data, u_char sum[16], ffstr *name)
{
	ssize_t r;
	int e = 0;
	if (32+3 > data->len) {
		e = 1;
		r = data->len;
		goto end;
	}

	const char *d = data->ptr;
	if (!(16 == ffs_tohex(sum, 16, d, 32)
		&& d[32] == ' '
		&& (d[33] == ' ' || d[33] == '*'))) {
		e = 1;
		r = ffstr_findany(data, "\r\n", 2);
		if (r < 0)
			r = data->len;
	}

	ffstr s = *data;
	ffstr_shift(&s, 32+2);
	r = ffstr_findany(&s, "\0\r\n\n", 4);
	if (r >= 0 && s.ptr[r] == '\0') {
		e = 1;
		r = ffstr_findany(&s, "\r\n", 2);
	}
	if (r < 0) {
		*name = s;
		r = s.len;
	} else {
		ffstr_set(name, s.ptr, r);
		if (s.ptr[r] == '\r')
			r++;
		if (s.ptr[r] == '\n')
			r++;
	}
	r += 32+2;

end:
	ffstr_shift(data, r);
	return e;
}
