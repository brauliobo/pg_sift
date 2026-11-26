/*
 * bm25/wbuf_int.h - the layout of the pages and records of a write buffer that its appends (wbuf.c) and its reads (wread.c) share
 */
#ifndef SIFT_WBUF_INT_H
#define SIFT_WBUF_INT_H

#include "core/page.h"

#define WBUFPAGE(page)	((SiftWbufPageHeaderData *) SiftPageGetData(page))
#define RECS(page, hs)	(SiftPageGetData(page) + (hs))
#define ROOM(page)		(SIFT_PAGE_PAYLOAD - SiftPageGetUsed(page))
/* The lengths of the fields of a doc (SIFT_WBUF_FIELDDL) follow its header, nfields of them, so that the hashes stay aligned; nfields is in the record */
#define FDL_BYTES(nfields) TYPEALIGN(4, (Size) (nfields) * sizeof(uint16))
#define RECSIZE(nterms, termbytes, nfields) \
	TYPEALIGN(4, sizeof(SiftWbufDocData) + FDL_BYTES(nfields) + (Size) (nterms) * 6 + (termbytes))
#define REC_NFIELDS(d)	(((d)->flags & SIFT_WBUF_FIELDDL) ? (d)->reserved : 0)
#define REC_HASH(d)		((const uint32 *) ((const char *) ((d) + 1) + FDL_BYTES(REC_NFIELDS(d))))

/* The header of a page of a buffer of this format: the buffers of format 0 have the 256-bit bloom in it, those of SIFT_WBUF_FORMAT_WIDE a bloom of 8,192 bits after it */
static inline Size
hdr_size(uint16 format)
{
	return format >= SIFT_WBUF_FORMAT_WIDE ? sizeof(SiftWbufWideHeaderData) : sizeof(SiftWbufPageHeaderData);
}

/* Two probes in a bloom of nwords words (a power of two): the low bits of the hash, then the ones above them */
static inline void
bloom_probes(int nwords, uint32 h, uint32 *a, uint32 *b)
{
	uint32		bits = (uint32) nwords * 64;

	*a = h & (bits - 1);
	*b = (h >> pg_leftmost_one_pos32(bits)) & (bits - 1);
}

static inline void
bloom_add(uint64 *bloom, int nwords, uint32 h)
{
	uint32		a,
				b;

	bloom_probes(nwords, h, &a, &b);
	bloom[a >> 6] |= UINT64CONST(1) << (a & 63);
	bloom[b >> 6] |= UINT64CONST(1) << (b & 63);
}

extern bool sift_wbuf_has_docs(Relation index, const SiftSegEntry *entry);

#endif							/* SIFT_WBUF_INT_H */
