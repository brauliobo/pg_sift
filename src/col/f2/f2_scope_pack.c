/*
 * col/f2/f2_scope_pack.c - the object of a scope summary as the part keeps it (f2_scope.h): the serialized object behind an 8-byte header, zstd-compressed when that makes it smaller.
 * The cubes are small varint records, so the whole object compresses four to six times where each cube alone (cubewire.c) is under the size that pays for a codec.
 */
#include "postgres.h"

#include "col/compress.h"
#include "col/codec.h"
#include "col/f2/f2_scope.h"
#include "col/f2/part_format.h"

#define SCOPE_PACK_RAW	0
#define SCOPE_PACK_ZSTD	1
/* STOR[STOR-SCOPE-05]: scope object zstd level 19 (level 22 measured equal at 160k rows) */
#define SCOPE_PACK_LEVEL	19

typedef struct ScopePackHdr
{
	uint8		alg;
	uint8		pad[3];
	uint32		rawlen;
} ScopePackHdr;

StaticAssertDecl(sizeof(ScopePackHdr) == 8, "the header keeps the object 8-byte aligned");

void
sift_f2_scope_pack(const StringInfoData *obj, StringInfo out)
{
	ScopePackHdr h = {.alg = SCOPE_PACK_RAW,.rawlen = obj->len};
	Size		clen = 0;
	char	   *packed = NULL;

	if (sift_colz_available(SIFT_CC_ALG_ZSTD))
	{
		Size		cap = sift_colz_bound(SIFT_CC_ALG_ZSTD, obj->len);

		packed = palloc(cap);
		clen = sift_colz_compress(SIFT_CC_ALG_ZSTD, SCOPE_PACK_LEVEL, obj->data, obj->len, packed, cap);
	}
	if (clen > 0 && clen < (Size) obj->len)
		h.alg = SCOPE_PACK_ZSTD;
	initStringInfo(out);
	appendBinaryStringInfo(out, (char *) &h, sizeof(h));
	if (h.alg == SCOPE_PACK_ZSTD)
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
	char	   *obj;

	if (bytes == NULL || plen < sizeof(h))
		return NULL;
	memcpy(&h, bytes, sizeof(h));
	*len = h.rawlen;
	if (h.alg == SCOPE_PACK_RAW)
		return plen - sizeof(h) == h.rawlen ? bytes + sizeof(h) : NULL;
	if (h.alg != SCOPE_PACK_ZSTD)
		return NULL;
	obj = palloc(h.rawlen);
	if (!sift_colz_decompress(SIFT_CC_ALG_ZSTD, bytes + sizeof(h), plen - sizeof(h), obj, h.rawlen))
		return NULL;
	return obj;
}
