/*
 * col/gpu.c - the chunks of a part of format major 2 as the device takes them (col/gpu.h).
 */
#include "postgres.h"

#include "agg/vk/layout.h"
#include "col/codec.h"
#include "col/gpu.h"
#include "col/granule/granule_int.h"

SiftVkKey
sift_gpu_chunk_key(const RelFileLocator *loc, uint32 part_id, uint64 part_gen, uint32 phys, uint32 granule)
{
	return (SiftVkKey) {.spc = loc->spcOid,.db = loc->dbOid,.rel = loc->relNumber,.part_id = part_id,.col_id = phys,.part_gen = part_gen,.granule_id = granule};
}

bool
sift_gpu_leaf_ok(SiftCellKind kind, uint8 hint)
{
	if (kind == SIFT_CELL_F8)
		return hint == SIFT_CHINT_AUTO;
	return kind == SIFT_CELL_INT && (hint == SIFT_CHINT_AUTO || hint == SIFT_CHINT_NUM64 || hint == SIFT_CHINT_BOOL);
}

bool
sift_gpu_part_eligible(const SiftPartDesc *d, FullTransactionId snap_xmin, SiftVisClass *cls, const char **why)
{
	if (d->level == 0)
	{
		*why = "a level 0 part is read on the CPU";
		return false;
	}
	if (d->ext_flags & SIFT_PART_EXT_REMOTE)
	{
		*why = "a part in object storage is read on the CPU";
		return false;
	}
	*cls = sift_vis_part_class(d, false, snap_xmin);
	return true;
}

bool
sift_gpu_granule_eligible(const SiftPartDesc *d, const SiftGranuleDesc *g, FullTransactionId snap_xmin)
{
	return sift_vis_granule_class(d, g, InvalidFullTransactionId, false, snap_xmin) == SIFT_VIS_GRANULE_SETTLED;
}

static bool
refuse(const char **why, const char *msg)
{
	*why = msg;
	return false;
}

static bool
enc_ok(uint16 enc)
{
	switch (enc)
	{
		case SIFT_VK_ENC_INT_FOR:
		case SIFT_VK_ENC_INT_DELTA:
		case SIFT_VK_ENC_INT_CONST:
		case SIFT_VK_ENC_DICT_CODE:
		case SIFT_VK_ENC_FLOAT_RAW:
		case SIFT_VK_ENC_ALL_NULL:
		case SIFT_VK_ENC_BOOL_BITS:
		case SIFT_VK_ENC_NUM64:
		case SIFT_VK_ENC_RLE:
		case SIFT_VK_ENC_ALP:
		case SIFT_VK_ENC_ALP_RD:
			return true;
		default:
			return false;
	}
}

static uint32
null_flags(const SiftChunkRef *ref)
{
	if (ref->flags & SIFT_CHUNK_ALL_NULL)
		return SIFT_VK_CF_ALL_NULL;
	if (ref->flags & SIFT_CHUNK_ALL_VALID)
		return SIFT_VK_CF_ALL_VALID;
	if (ref->enc & SIFT_ENC_NULL_SPARSE)
		return SIFT_VK_CF_NULL_SPARSE;
	return (ref->enc & SIFT_ENC_NONNULL_SPARSE) ? SIFT_VK_CF_NONNULL_SPARSE : 0;
}

static bool
field_of(const SiftZone *z, SiftCellKind kind, bool all_null, SiftVkChunk *out)
{
	uint64		range;

	if (all_null)
		return true;
	if (kind == SIFT_CELL_STR)
	{
		out->w = 4;
		return true;
	}
	if (kind == SIFT_CELL_F8)
	{
		out->w = 8;
		return true;
	}
	if ((z->flags & SIFT_ZONE_NONE) || z->min > z->max)
		return false;
	range = (uint64) z->max - (uint64) z->min;
	out->base = z->min;
	out->w = range == 0 ? 0 : range <= 0xFF ? 1 : range <= 0xFFFF ? 2 : range <= 0xFFFFFFFFu ? 4 : 8;
	return true;
}

static bool
blob_enc(uint16 enc)
{
	return enc == SIFT_VK_ENC_INT_FOR || enc == SIFT_VK_ENC_INT_DELTA || enc == SIFT_VK_ENC_DICT_CODE || enc == SIFT_VK_ENC_RLE || enc == SIFT_VK_ENC_ALP || enc == SIFT_VK_ENC_NUM64;
}

/* The chunk whose blob has a node wrapped in zstd, laid out again with the node unwrapped (the validity, the blob, what follows it); body_off is where the blob's length word is */
static bool
unwrap_chunk(SiftVkChunk *out, uint32 body_off, const char **why)
{
	const uint8 *raw = out->bytes;
	uint32		blen,
				tail;
	StringInfoData blob,
				chunk;
	uint32		head[2];

	if ((uint64) body_off + 8 > out->len)
		return refuse(why, "a chunk is shorter than its blob");
	memcpy(&blen, raw + body_off, sizeof(blen));
	tail = body_off + 8 + ((blen + 7) & ~7u);
	if (tail > out->len)
		return refuse(why, "the blob of a chunk is longer than the chunk");
	initStringInfo(&blob);
	if (!sift_gpu_blob_unwrap(raw + body_off + 8, blen, &blob, why))
		return false;
	if (blob.len == 0)
		return true;
	head[0] = (uint32) blob.len;
	head[1] = 0;
	initStringInfo(&chunk);
	appendBinaryStringInfo(&chunk, (const char *) raw, (int) body_off);
	appendBinaryStringInfo(&chunk, (const char *) head, sizeof(head));
	appendBinaryStringInfo(&chunk, blob.data, blob.len);
	appendBinaryStringInfo(&chunk, (const char *) raw + tail, (int) (out->len - tail));
	out->bytes = (const uint8 *) chunk.data;
	out->len = out->rawlen = (uint32) chunk.len;
	return true;
}

bool
sift_gpu_chunk_fill(const SiftGranuleView *v, int phys, SiftCellKind kind, const SiftZone *z, SiftVkChunk *out, const char **why)
{
	const SiftChunkRef *ref;
	uint32		nrows = v->hdr->nrows,
				flags;
	uint16		enc;

	if (phys < 0 || phys + 1 >= v->hdr->nchunks)
		return refuse(why, "the column is not in the granule");
	ref = &v->hdr->chunks[phys + 1];
	enc = ref->enc & SIFT_ENC_MASK;
	if ((ref->flags & (SIFT_CHUNK_HAS_KINDS | SIFT_CHUNK_HAS_OVERFLOW | SIFT_CHUNK_ELEMS)) || (ref->enc & (SIFT_ENC_KEY_ANY | SIFT_ENC_FRAMED)) || (kind != SIFT_CELL_STR && (ref->enc & SIFT_ENC_PART_DICT)))
		return refuse(why, "a chunk holds exceptions, elements or a dictionary of strings");
	if (kind == SIFT_CELL_STR && (enc != SIFT_VK_ENC_DICT_CODE || !(ref->enc & SIFT_ENC_PART_DICT)))
		return refuse(why, "a string chunk is not coded by the dictionary of its part");
	if (!enc_ok(enc))
		return refuse(why, "a chunk has an encoding the device does not decode");
	flags = null_flags(ref);
	out->bytes = v->rec + ref->off;
	out->len = ref->len;
	out->rawlen = ref->len;
	if (ref->flags & SIFT_CHUNK_COMPRESSED)
	{
		uint32		wrap[2];

		if (ref->len < sizeof(wrap))
			return refuse(why, "a wrapped chunk is shorter than its header");
		memcpy(wrap, out->bytes, sizeof(wrap));
		if (wrap[0] != SIFT_CC_ALG_LZ4)
			return refuse(why, "a chunk is wrapped in zstd, which the device does not unwrap");
		flags |= SIFT_VK_CF_LZ4;
		out->rawlen = wrap[1];
	}
	out->enc = enc;
	out->flags = flags;
	out->nrows = nrows;
	out->nnonnull = (flags & SIFT_VK_CF_ALL_NULL) ? 0 : (flags & SIFT_VK_CF_ALL_VALID) ? nrows : ref->nnonnull;
	out->vlen = (flags & (SIFT_VK_CF_ALL_NULL | SIFT_VK_CF_ALL_VALID)) ? 0 : sift_chunk_validity_len(ref, nrows);
	if (!(flags & SIFT_VK_CF_ALL_NULL) && !(flags & SIFT_VK_CF_LZ4) && blob_enc(enc) && !unwrap_chunk(out, out->vlen + (enc == SIFT_VK_ENC_NUM64 ? 8 : 0), why))
		return false;
	out->w = 0;
	out->base = 0;
	if (!field_of(z, kind, (flags & SIFT_VK_CF_ALL_NULL) != 0, out))
		return refuse(why, "the zone of a column holds no range of its values");
	return true;
}
