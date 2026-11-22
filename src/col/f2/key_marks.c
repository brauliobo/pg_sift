/*
 * col/f2/key_marks.c - the writer and the reader of SEC_KEY_MARKS (col/f2/key_marks.h).
 */
#include "core/host.h"

#include "col/f2/key_marks.h"

struct SiftKeyMarksBuilder
{
	SiftKeySpec spec;
	bool		has_hi;
	bool		exact;
	uint32		n;
	uint32		cap;
	uint8	   *bytes;			/* n marks of (has_hi ? 2 : 1) * key_bytes */
};

uint64
sift_key_marks_size(uint32 n, uint8 key_bytes, bool has_hi)
{
	return sizeof(SiftKeyMarksHdr) + (uint64) n * key_bytes * (has_hi ? 2 : 1);
}

SiftKeyMarksBuilder *
sift_key_marks_begin(const SiftKeySpec *spec, bool has_hi)
{
	SiftKeyMarksBuilder *b;

	SIFT_INVARIANT(sift_key_spec_valid(spec), "the key of the marks has %d columns and a stride of %d bytes", spec->ncols, spec->key_bytes);
	b = palloc0(sizeof(SiftKeyMarksBuilder));
	b->spec = *spec;
	b->has_hi = has_hi;
	b->exact = true;
	return b;
}

void
sift_key_marks_add(SiftKeyMarksBuilder *b, const SiftKeyVal *first, const SiftKeyVal *last)
{
	uint32		kb = b->spec.key_bytes,
				stride = kb * (b->has_hi ? 2 : 1);
	bool		cut = false;

	if (b->n == b->cap)
	{
		b->cap = b->cap ? b->cap * 2 : 64;
		b->bytes = b->bytes ? repalloc(b->bytes, (size_t) b->cap * stride) : palloc((size_t) b->cap * stride);
	}
	sift_key_enc_tuple(&b->spec, first, b->spec.ncols, b->bytes + (size_t) b->n * stride, &cut);
	if (b->has_hi)
		sift_key_enc_tuple(&b->spec, last, b->spec.ncols, b->bytes + (size_t) b->n * stride + kb, &cut);
	b->exact &= !cut;
	b->n++;
}

void *
sift_key_marks_finish(SiftKeyMarksBuilder *b, uint64 *len)
{
	uint64		body = sift_key_marks_size(b->n, b->spec.key_bytes, b->has_hi) - sizeof(SiftKeyMarksHdr);
	SiftKeyMarksHdr hdr = {.magic = SIFT_KEY_MARKS_MAGIC,.version = SIFT_KEY_MARKS_VERSION,.nkey_cols = b->spec.ncols,.key_bytes = b->spec.key_bytes,.ngranules = b->n,
	.flags = (b->has_hi ? SIFT_KEY_MARKS_HAS_HI : 0) | (b->exact ? SIFT_KEY_MARKS_EXACT : 0)};
	uint8	   *out = palloc(sizeof(hdr) + body);

	memcpy(out, &hdr, sizeof(hdr));
	if (body > 0)
		memcpy(out + sizeof(hdr), b->bytes, body);
	*len = sizeof(hdr) + body;
	if (b->bytes != NULL)
		pfree(b->bytes);
	pfree(b);
	return out;
}

const char *
sift_key_marks_open(const void *blob, uint64 len, const SiftKeySpec *spec, uint32 ngranules, SiftKeyMarks *km)
{
	SiftKeyMarksHdr hdr;
	bool		has_hi;

	if (len < sizeof(hdr))
		return "the key marks are shorter than their header";
	memcpy(&hdr, blob, sizeof(hdr));
	if (hdr.magic != SIFT_KEY_MARKS_MAGIC || hdr.version != SIFT_KEY_MARKS_VERSION)
		return "the key marks have another magic or version";
	if (hdr.flags & ~(SIFT_KEY_MARKS_HAS_HI | SIFT_KEY_MARKS_EXACT))
		return "the key marks have flags this build does not know";
	if (hdr.nkey_cols != spec->ncols || hdr.key_bytes != spec->key_bytes || !sift_key_spec_valid(spec))
		return "the key marks are of another key than the index's";
	if (hdr.ngranules != ngranules)
		return "the key marks are for another number of granules than the part has";
	has_hi = (hdr.flags & SIFT_KEY_MARKS_HAS_HI) != 0;
	if (len != sift_key_marks_size(hdr.ngranules, hdr.key_bytes, has_hi))
		return "the key marks are not the size their header says";
	*km = (SiftKeyMarks) {.base = (const uint8 *) blob + sizeof(hdr),.ngranules = hdr.ngranules,.nkey = hdr.nkey_cols,.key_bytes = hdr.key_bytes,.has_hi = has_hi,
	.exact = (hdr.flags & SIFT_KEY_MARKS_EXACT) != 0};
	return NULL;
}

const char *
sift_key_marks_check(const SiftKeyMarks *km)
{
	for (uint32 g = 0; g < km->ngranules; g++)
	{
		const uint8 *lo = sift_key_marks_lo(km, g),
				   *hi = sift_key_marks_hi(km, g);

		if (hi != NULL && memcmp(lo, hi, km->key_bytes) > 0)
			return "a granule's first key is above its last";
		if (g + 1 < km->ngranules && memcmp(hi != NULL ? hi : lo, sift_key_marks_lo(km, g + 1), km->key_bytes) > 0)
			return "a granule's last key is above the next one's first";
	}
	return NULL;
}

void *
sift_key_marks_concat(const void *const *blobs, const uint64 *lens, int n, uint64 *len)
{
	SiftKeyMarksHdr first,
				out_hdr;
	uint64		total = sizeof(SiftKeyMarksHdr),
				at;
	uint32		ng = 0;
	uint8	   *out;
	bool		exact = true;

	for (int i = 0; i < n; i++)
	{
		SiftKeyMarksHdr h;

		if (lens[i] < sizeof(h))
			return NULL;
		memcpy(&h, blobs[i], sizeof(h));
		if (i == 0)
			first = h;
		if (h.magic != SIFT_KEY_MARKS_MAGIC || h.nkey_cols != first.nkey_cols || h.key_bytes != first.key_bytes || (h.flags & SIFT_KEY_MARKS_HAS_HI) != (first.flags & SIFT_KEY_MARKS_HAS_HI) ||
			lens[i] != sift_key_marks_size(h.ngranules, h.key_bytes, (h.flags & SIFT_KEY_MARKS_HAS_HI) != 0))
			return NULL;
		exact &= (h.flags & SIFT_KEY_MARKS_EXACT) != 0;
		ng += h.ngranules;
		total += lens[i] - sizeof(h);
	}
	if (n == 0)
		return NULL;
	out = palloc(total);
	out_hdr = first;
	out_hdr.ngranules = ng;
	out_hdr.flags = (first.flags & SIFT_KEY_MARKS_HAS_HI) | (exact ? SIFT_KEY_MARKS_EXACT : 0);
	memcpy(out, &out_hdr, sizeof(out_hdr));
	at = sizeof(out_hdr);
	for (int i = 0; i < n; i++)
	{
		memcpy(out + at, (const uint8 *) blobs[i] + sizeof(out_hdr), lens[i] - sizeof(out_hdr));
		at += lens[i] - sizeof(out_hdr);
	}
	{
		SiftKeyMarks km = {.base = out + sizeof(out_hdr),.ngranules = ng,.nkey = first.nkey_cols,.key_bytes = first.key_bytes,.has_hi = (first.flags & SIFT_KEY_MARKS_HAS_HI) != 0};

		if (sift_key_marks_check(&km) != NULL)
		{
			pfree(out);
			return NULL;
		}
	}
	*len = total;
	return out;
}
