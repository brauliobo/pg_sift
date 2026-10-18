/* The error channel of the codec (sift_codec_corrupt), which the extension's host provides: the standalone tools stop at the first corrupt byte. */
#include <stdio.h>
#include <stdlib.h>

#include "sift_bitmap.h"

void sift_codec_corrupt(const char *what)
{
	fprintf(stderr, "corrupt posting data: %s\n", what);
	abort();
}
