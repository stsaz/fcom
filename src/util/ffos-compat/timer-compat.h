#include <FFOS/time.h>

#ifdef FF_WIN

static FFINL int ffclk_get(fftime *result) {
	LARGE_INTEGER val;
	QueryPerformanceCounter(&val);
	result->sec = val.QuadPart;
	return 0;
}

FF_EXTN void ffclk_totime(fftime *t);

#else

/** Get system-specific clock value (unrelated to UTC time). */
static FFINL int ffclk_get(fftime *result) {
	struct timespec ts;
	int r = clock_gettime(CLOCK_MONOTONIC, &ts);
	if (r == 0)
		fftime_fromtimespec(result, &ts);
	else
		fftime_null(result);
	return r;
}

/** Convert the value returned by ffclk_get() to fftime. */
#define ffclk_totime(t)

#endif

#define ffclk_gettime(t) \
do { \
	ffclk_get(t); \
	ffclk_totime(t); \
} while(0)
