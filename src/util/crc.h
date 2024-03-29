/** CRC.
Copyright (c) 2013 Simon Zolin
*/

#pragma once
#include "string.h"

static inline uint ffcrc32_start() {
	return 0xffffffff;
}

FF_EXTERN const uint crc32_table256[];

static inline void ffcrc32_update(uint *crc, uint b) {
	*crc = crc32_table256[(*crc ^ b) & 0xff] ^ (*crc >> 8);
}

static inline void ffcrc32_iupdate(uint *crc, uint b) {
	ffcrc32_update(crc, ffchar_isup(b) ? ffchar_lower(b) : b);
}

static inline void ffcrc32_updatestr(uint *crc, const char *p, size_t len)
{
	size_t i;
	for (i = 0;  i != len;  i++) {
		ffcrc32_update(crc, (byte)p[i]);
	}
}

static inline void ffcrc32_finish(uint *crc) {
	*crc ^= 0xffffffff;
}

static inline uint ffcrc32_get(const char *p, size_t len)
{
	uint crc = ffcrc32_start();
	for (size_t i = 0;  i != len;  i++) {
		ffcrc32_update(&crc, (byte)p[i]);
	}
	ffcrc32_finish(&crc);
	return crc;
}

static inline uint ffcrc32_iget(const char *p, size_t len)
{
	uint crc = ffcrc32_start();
	for (size_t i = 0;  i != len;  i++) {
		ffcrc32_iupdate(&crc, (byte)p[i]);
	}
	ffcrc32_finish(&crc);
	return crc;
}
