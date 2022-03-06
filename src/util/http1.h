
/** Replace each "%XX" escape sequence in URL string with a byte value
Return N of bytes written
 <0 if not enough space */
static inline int httpurl_unescape(char *buf, ffsize cap, ffstr url)
{
	char *d = url.ptr, *end = url.ptr + url.len, *p = buf, *ebuf = buf + cap;

	if (buf == NULL) {
		while (d < end) {
			ffssize r = ffs_skip_ranges(d, end - d, "\x21\x24\x26\x7e\x80\xff", 6);
			if (r < 0) {
				r = end - d;
			} else {
				cap++;
				d += 3;
			}
			d += r;
			cap += r;
		}
		return cap;
	}

	while (d != end) {
		ffssize r = ffs_skip_ranges(d, end - d, "\x21\x24\x26\x7e\x80\xff", 6); // printable except '%'
		if (r < 0)
			r = end - d;
		if (r > ebuf - p)
			return -1;
		p = ffmem_copy(p, d, r);
		d += r;

		if (d == end)
			break;

		if (d+3 > end || d[0] != '%')
			return -1;
		int h = ffchar_tohex(d[1]);
		int l = ffchar_tohex(d[2]);
		if (h < 0 || l < 0)
			return -1;
		if (p == ebuf)
			return -1;
		*p++ = (h<<4) | l;
		d += 3;
	}

	return p - buf;
}
