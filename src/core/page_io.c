/*
 * core/page_io.c - the page header every format gives its pages (core/page_io.h).
 */
#include "core/host.h"

#include "core/page_io.h"

void
sift_page_init(Page page, SiftPageType type, uint64 gen)
{
	PageInit(page, BLCKSZ, sizeof(SiftPageOpaqueData));
	SiftPageGetOpaque(page)->type = type;
	SiftPageGetOpaque(page)->gen = gen;
}
