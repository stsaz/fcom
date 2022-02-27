/** Base64 converter.
2018, Simon Zolin */

#include <stdlib.h>

extern size_t base64_encode(char *dst, size_t cap, const void *src, size_t len);
