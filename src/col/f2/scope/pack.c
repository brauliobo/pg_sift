/*
 * col/f2/scope/pack.c - the object of a scope summary as the part keeps it (f2_scope.h): the serialized object behind an 8-byte header, zstd-compressed when that makes it smaller.
 * The cubes are small varint records, so the whole object compresses four to six times where each cube alone (cubewire.c) is under the size that pays for a codec.
 * The fixed-width record arrays of the object (the slices of level 0, the nodes of level 1, the roots of level 2 and the rings) are compressed byte plane by byte plane: the records of
 * an array differ in few of their bytes (offsets, counts, the steps of a ring), so the planes hold long runs where the records interleave them. A summary of 55,744 bytes packs to
 * 3,823 bytes so, against 8,161 for the records as they are (zstd 1.5.7, level 19): its part takes one page whatever the version of zstd.
 */
#include "postgres.h"

#include "col/compress.h"
#include "col/codec/codec.h"
#include "col/f2/scope/scope.h"
#include "col/f2/part/format.h"

#define SCOPE_PACK_RAW		0
#define SCOPE_PACK_ZSTD		1			/* the object as it is (summaries written before the planes) */
#define SCOPE_PACK_PLANES	2			/* the object with its record arrays in byte planes */
/* STOR[STOR-SCOPE-05]: scope object zstd level 19 (level 22 measured equal at 160k rows) */
#define SCOPE_PACK_LEVEL	19
/* the footer of a section of records: their count, a zero, the length of the pool before them (col/f2/cube/object.c) */
#define SCOPE_FOOTER_BYTES	16

typedef struct ScopePackHdr
{
	uint8		alg;
	uint8		pad[3];
	uint32		rawlen;
} ScopePackHdr;

StaticAssertDecl(sizeof(ScopePackHdr) == 8, "the header keeps the object 8-byte aligned");

typedef struct PlaneSpan
{
	uint64		off;
	uint64		nrec;
	uint32		width;
} PlaneSpan;

/* The records of a section whose footer counts them (levels 0, 1 and 2): they end where the footer begins */
static bool
footer_span(const char *obj, const SiftScopeSecEntry *e, uint32 width, PlaneSpan *span)
{
	uint32		nrec;

	if (e->len < SCOPE_FOOTER_BYTES)
		return false;
	memcpy(&nrec, obj + e->off + e->len - SCOPE_FOOTER_BYTES, sizeof(nrec));
	if ((uint64) nrec * width > e->len - SCOPE_FOOTER_BYTES)
		return false;
	*span = (PlaneSpan) {.off = e->off + e->len - SCOPE_FOOTER_BYTES - (uint64) nrec * width,.nrec = nrec,.width = width};
	return true;
}

/*
 * The record arrays of the object of len bytes, from its trailer, its section directory and its header, which no plane covers: the packed object tells them as the object does.
 * At most one a section; -1 when the object is not one (a damaged section of a part reads as no summary).
 */
static int
plane_spans(const char *obj, uint64 len, PlaneSpan *spans)
{
	SiftScopeTrailer tr;
	SiftScopeSecEntry dir[SIFT_SCOPE_SECTIONS_CAP];
	SiftScopeMetaHeader hdr = {0};
	uint32		ring;
	int			n = 0;

	if (len < sizeof(tr))
		return -1;
	memcpy(&tr, obj + len - sizeof(tr), sizeof(tr));
	if (tr.magic != SIFT_SCOPE_META_MAGIC || tr.nsec > SIFT_SCOPE_SECTIONS_CAP || tr.dir_off > len - sizeof(tr) || len - sizeof(tr) - tr.dir_off != (uint64) tr.nsec * sizeof(*dir))
		return -1;
	memcpy(dir, obj + tr.dir_off, tr.nsec * sizeof(*dir));
	for (uint32 i = 0; i < tr.nsec; i++)
	{
		if (dir[i].off > tr.dir_off || dir[i].len > tr.dir_off - dir[i].off)
			return -1;
		if (dir[i].tag == SEC_SCOPE_HDR && dir[i].len >= sizeof(hdr))
			memcpy(&hdr, obj + dir[i].off, sizeof(hdr));
	}
	if (hdr.magic != SIFT_SCOPE_META_MAGIC)
		return -1;
	ring = hdr.flags & SIFT_SMETA_TOPK_WIDE ? sizeof(SiftScopeTopKEntryWide) : sizeof(SiftScopeTopKEntry);
	for (uint32 i = 0; i < tr.nsec; i++)
	{
		bool		ok = true;

		switch (dir[i].tag)
		{
			case SEC_SCOPE_L0_SLICE:
				ok = footer_span(obj, &dir[i], sizeof(SiftScopeL0SliceEntry), &spans[n++]);
				break;
			case SEC_SCOPE_L1:
				ok = footer_span(obj, &dir[i], sizeof(SiftScopeL1Node), &spans[n++]);
				break;
			case SEC_SCOPE_L2:
				ok = footer_span(obj, &dir[i], sizeof(SiftScopeL2Root), &spans[n++]);
				break;
			case SEC_SCOPE_TOPK:
				if (dir[i].flags == 0)
					spans[n++] = (PlaneSpan) {.off = dir[i].off,.nrec = dir[i].len / ring,.width = ring};
				break;
		}
		if (!ok)
			return -1;
	}
	return n;
}

/* to: the planes of the records of from (put), or the records of the planes of from */
static void
planes_move(char *to, const char *from, const PlaneSpan *s, bool put)
{
	for (uint64 r = 0; r < s->nrec; r++)
		for (uint32 b = 0; b < s->width; b++)
		{
			const uint64 rec = r * s->width + b,
						plane = b * s->nrec + r;

			to[put ? plane : rec] = from[put ? rec : plane];
		}
}

/* to: the object of len bytes from with its record arrays in planes (put), or from in planes with them as records; false when from is no object */
static bool
planes_apply(char *to, const char *from, uint64 len, bool put)
{
	PlaneSpan	spans[SIFT_SCOPE_SECTIONS_CAP];
	const int	n = plane_spans(from, len, spans);

	memcpy(to, from, len);
	for (int i = 0; i < n; i++)
		planes_move(to + spans[i].off, from + spans[i].off, &spans[i], put);
	return n >= 0;
}

void
sift_f2_scope_pack(const StringInfoData *obj, StringInfo out)
{
	ScopePackHdr h = {.alg = SCOPE_PACK_RAW,.rawlen = obj->len};
	Size		clen = 0;
	char	   *packed = NULL;

	if (sift_colz_available(SIFT_CC_ALG_ZSTD))
	{
		Size		cap = sift_colz_bound(SIFT_CC_ALG_ZSTD, obj->len);
		char	   *planes = palloc(obj->len);
		const bool	whole = planes_apply(planes, obj->data, obj->len, true);

		SIFT_INVARIANT(whole, "scope summary: the object it packs has no trailer");
		packed = palloc(cap);
		clen = sift_colz_compress(SIFT_CC_ALG_ZSTD, SCOPE_PACK_LEVEL, planes, obj->len, packed, cap);
		pfree(planes);
	}
	if (clen > 0 && clen < (Size) obj->len)
		h.alg = SCOPE_PACK_PLANES;
	initStringInfo(out);
	appendBinaryStringInfo(out, (char *) &h, sizeof(h));
	if (h.alg == SCOPE_PACK_PLANES)
		appendBinaryStringInfo(out, packed, clen);
	else
		appendBinaryStringInfo(out, obj->data, obj->len);
	if (packed != NULL)
		pfree(packed);
}

char *
sift_f2_scope_read_object(Relation index, const SiftPart *part, uint64 *len)
{
	uint64		plen;
	char	   *bytes = sift_part_read_section(index, part, SEC_SCOPE_HDR, &plen);
	ScopePackHdr h;
	char	   *obj,
			   *planes;

	if (bytes == NULL || plen < sizeof(h))
		return NULL;
	memcpy(&h, bytes, sizeof(h));
	*len = h.rawlen;
	if (h.alg == SCOPE_PACK_RAW)
		return plen - sizeof(h) == h.rawlen ? bytes + sizeof(h) : NULL;
	if (h.alg != SCOPE_PACK_ZSTD && h.alg != SCOPE_PACK_PLANES)
		return NULL;
	planes = palloc(h.rawlen);
	if (!sift_colz_decompress(SIFT_CC_ALG_ZSTD, bytes + sizeof(h), plen - sizeof(h), planes, h.rawlen))
		return NULL;
	if (h.alg == SCOPE_PACK_ZSTD)
		return planes;
	obj = palloc(h.rawlen);
	if (!planes_apply(obj, planes, h.rawlen, false))
		return NULL;
	pfree(planes);
	return obj;
}
