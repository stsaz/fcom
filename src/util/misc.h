#pragma once

/** Compare files by content.
@limit: maximum amount of data to process (0:unlimited). */
int fffile_cmp(const char *fn1, const char *fn2, uint64 limit)
{
	int r;
	ssize_t n;
	fffd f1 = FF_BADFD, f2 = FF_BADFD;
	ffarr a1 = {}, a2 = {};
	if (NULL == ffarr_alloc(&a1, 64 * 1024)
		|| NULL == ffarr_alloc(&a2, 64 * 1024)) {
		r = -2;
		goto end;
	}

	f1 = fffile_open(fn1, O_RDONLY);
	f2 = fffile_open(fn2, O_RDONLY);
	if (f1 == FF_BADFD || f2 == FF_BADFD) {
		r = -2;
		goto end;
	}

	if (limit == 0)
		limit = (uint64)-1;

	for (;;) {
		n = fffile_read(f1, a1.ptr, a1.cap);
		if (n < 0) {
			r = -2;
			break;
		}
		a1.len = n;

		n = fffile_read(f2, a2.ptr, a2.cap);
		if (n < 0) {
			r = -2;
			break;
		}
		a2.len = n;

		ffint_setmin(a1.len, limit);
		ffint_setmin(a2.len, limit);
		limit -= a1.len;

		r = ffstr_cmp2((ffstr*)&a1, (ffstr*)&a2);
		if (r != 0
			|| a1.len == 0)
			break;
	}

end:
	ffarr_free(&a1);
	ffarr_free(&a2);
	fffile_safeclose(f1);
	fffile_safeclose(f2);
	return r;
}
