/* Scalar UTF-8 byte emission for the host conversion wrappers. */
#ifndef SIFT_HOST_UTF8_H
#define SIFT_HOST_UTF8_H

#include <stdint.h>

/* The caller provides four bytes; length, termination and input policy stay with it. */
static inline char *
host_utf8_emit(char *p, uint32_t c)
{
	if (c < 0x80)
		*p++ = c;
	else if (c < 0x800)
	{
		*p++ = 0xC0 | (c >> 6);
		*p++ = 0x80 | (c & 0x3F);
	}
	else if (c < 0x10000)
	{
		*p++ = 0xE0 | (c >> 12);
		*p++ = 0x80 | ((c >> 6) & 0x3F);
		*p++ = 0x80 | (c & 0x3F);
	}
	else
	{
		*p++ = 0xF0 | (c >> 18);
		*p++ = 0x80 | ((c >> 12) & 0x3F);
		*p++ = 0x80 | ((c >> 6) & 0x3F);
		*p++ = 0x80 | (c & 0x3F);
	}
	return p;
}

#endif
