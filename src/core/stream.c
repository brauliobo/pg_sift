/*
 * core/stream.c - what every store does the same way (core/store.h): the page layout of its pages and the padding of its byte
 * streams, on top of PageInit, sift_sw_tell and sift_sw_write.
 */
#include "core/host.h"

#include "core/store.h"

void
sift_page_init(Page page, SiftPageType type, uint64 gen)
{
	PageInit(page, BLCKSZ, sizeof(SiftPageOpaqueData));
	SiftPageGetOpaque(page)->type = type;
	SiftPageGetOpaque(page)->gen = gen;
}

/*
 * The state of the page is a function of the stream position alone: a page holds SIFT_PAGE_PAYLOAD bytes from
 * SIFT_PAGE_DATA_OFFSET, and a full page (or none yet) is the same as an empty one for the next byte. When the padding would
 * reach the end of the page the next byte lands on the unaligned start of the next one: that page is filled first and the
 * padding repeated.
 */
void
sift_sw_align(SiftStreamWriter *sw, size_t align)
{
	static const char zeros[SIFT_SIMD_ALIGN];

	SIFT_INVARIANT(align != 0 && (align & (align - 1)) == 0 && align <= SIFT_SIMD_ALIGN, "sift: invalid stream alignment %zu", align);
	for (;;)
	{
		size_t		used = sift_sw_tell(sw) % SIFT_PAGE_PAYLOAD;
		size_t		room = SIFT_PAGE_PAYLOAD - used;
		size_t		pad = (size_t) -(SIFT_PAGE_DATA_OFFSET + used) & (align - 1);

		if (pad < room)
		{
			sift_sw_write(sw, zeros, pad);
			return;
		}
		sift_sw_write(sw, zeros, room);
	}
}

#define SPOOL_CHUNK		65536

void
sift_sw_write_file(SiftStreamWriter *sw, BufFile *file, uint64 len)
{
	char	   *buf = palloc(SPOOL_CHUNK);

	SIFT_INVARIANT(BufFileSeek(file, 0, 0, SEEK_SET) == 0, "sift: could not rewind a spool file");
	for (uint64 n; len > 0; len -= n)
	{
		n = Min(len, SPOOL_CHUNK);
		BufFileReadExact(file, buf, n);
		sift_sw_write(sw, buf, n);
	}
	pfree(buf);
}
