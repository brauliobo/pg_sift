/*
 * col/chunk_test.c - L1 tests: the zone, tests, decoding and tags of a column chunk.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include <math.h>

#include "col/enc.h"
#include "col/enc_test_int.h"
#include "core/popcount.h"

static int64
key_of(const SiftColDef *d, SiftCDict *segdict, const SiftColValue *v)
{
	return d->store == SIFT_COLS_INT ? v->v.i : d->store == SIFT_COLS_VAR ? v->len :
		d->store == SIFT_COLS_STR ? sift_cdict_find(segdict, v->v.s, v->len) : sift_col_f8_key(v->v.f);
}

/* The zone of the reference values, computed independently of the encoder (128 bit sums) */
void
check_zone(TCtx *t, const char *what, const SiftColDef *d, SiftCDict *segdict, const Ref *ref, const SiftCZone *z, bool tags)
{
	uint32		nn = 0;
	int64		mn = 0,
				mx = 0;
	__int128	sum = 0;
	double		sumf = 0;
	uint64		set[4] = {0};
	uint32		flags = 0;

	for (uint32 i = 0; i < ref->n; i++)
	{
		int64		k;

		if (ref->v[i].overflow)
			flags |= SIFT_COLZ_OVERFLOW;
		flags |= tags && !ref->v[i].overflow && ref->v[i].tag == SIFT_COLTAG_JNULL ? SIFT_COLZ_JNULL : 0;
		if (ref_null(&ref->v[i]))
			continue;
		k = key_of(d, segdict, &ref->v[i]);
		mn = nn == 0 ? k : Min(mn, k);
		mx = nn == 0 ? k : Max(mx, k);
		nn++;
		sum += k;
		if (d->store == SIFT_COLS_STR)
			set[(k & 255) >> 6] |= UINT64CONST(1) << (k & 63);
		if (d->store == SIFT_COLS_F4 || d->store == SIFT_COLS_F8)
		{
			sumf += ref->v[i].v.f;
			flags |= isnan(ref->v[i].v.f) ? SIFT_COLZ_NAN : 0;
		}
	}
	flags |= nn > 0 && mn == mx ? SIFT_COLZ_CONST : 0;
	if (d->store == SIFT_COLS_INT && (sum > PG_INT64_MAX || sum < PG_INT64_MIN))
		flags |= SIFT_COLZ_NOSUM;
	CHECK(t, z->nvals == ref->n && z->nnull == ref->n - nn, "%s zone count %u/%u, expected %u/%u", what, z->nnull, z->nvals, ref->n - nn, ref->n);
	CHECK(t, z->flags == flags, "%s zone flags %x, expected %x", what, z->flags, flags);
	CHECK(t, nn == 0 || (z->min == mn && z->max == mx), "%s zone range", what);
	CHECK(t, nn > 0 || (z->min == 0 && z->max == 0), "%s zone range of NULLs", what);
	CHECK(t, d->store != SIFT_COLS_INT || (flags & SIFT_COLZ_NOSUM) || z->sum_i == (int64) sum, "%s zone sum", what);
	CHECK(t, (d->store != SIFT_COLS_F4 && d->store != SIFT_COLS_F8) || memcmp(&z->sum_f, &sumf, sizeof(double)) == 0 || (isnan(z->sum_f) && isnan(sumf)),
		  "%s zone float sum", what);
	CHECK(t, d->store != SIFT_COLS_STR || memcmp(z->set, set, sizeof(set)) == 0, "%s zone set", what);
}

/* The zone of an inline text chunk, from the reference values: prefix keys of the smallest and largest, no set */
void
check_zone_inline(TCtx *t, const char *what, const Ref *ref, const SiftCZone *z, bool tags)
{
	uint32		nn = 0,
				flags = SIFT_COLZ_INLINE;
	int64		mn = 0,
				mx = 0;
	uint64		ones[4] = {~UINT64CONST(0), ~UINT64CONST(0), ~UINT64CONST(0), ~UINT64CONST(0)};

	for (uint32 i = 0; i < ref->n; i++)
	{
		int64		k;

		flags |= ref->v[i].overflow ? SIFT_COLZ_OVERFLOW : 0;
		flags |= tags && !ref->v[i].overflow && ref->v[i].tag == SIFT_COLTAG_JNULL ? SIFT_COLZ_JNULL : 0;
		if (ref_null(&ref->v[i]))
			continue;
		k = sift_col_str_prefix(ref->v[i].v.s, ref->v[i].len);
		mn = nn == 0 ? k : Min(mn, k);
		mx = nn == 0 ? k : Max(mx, k);
		nn++;
	}
	CHECK(t, z->nvals == ref->n && z->nnull == ref->n - nn && z->flags == flags, "%s inline zone counts/flags %u/%u %x, expected %u/%u %x", what,
		  z->nnull, z->nvals, z->flags, ref->n - nn, ref->n, flags);
	CHECK(t, z->min == mn && z->max == mx && memcmp(z->set, ones, sizeof(ones)) == 0, "%s inline zone range or set", what);
}

#define NPREDS	8

/* direct: the chunk is inline text and is tested as stored, by the strings (and its prefix blob), not through a dictionary */
void
check_tests(TCtx *t, const char *what, const SiftColDef *d, SiftCDict *segdict, const Ref *ref, const SiftCChunk *ch, const SiftCZone *z, bool direct)
{
	uint32		n = ref->n,
				nw = SIFT_COL_NWORDS(n);
	uint64	   *bits = palloc(nw * sizeof(uint64)),
			   *want = palloc(nw * sizeof(uint64));
	SiftColValue consts[96];

	for (int k = 0; k < NPREDS; k++)
	{
		SiftColPred p;
		SiftCTest	test;
		bool		any = false,
					masked = RAND(t, 2);

		gen_pred(t, d, ref, &p, consts, RAND(t, 4) == 0 ? 90 : 13);
		if (direct)
			sift_ctest_make_strs(&p, &test);
		else
			sift_ctest_make(d, segdict, &p, &test);
		memset(bits, 0, nw * sizeof(uint64));
		memset(want, 0, nw * sizeof(uint64));
		for (uint32 i = 0; i < n; i++)
		{
			bool		in = !masked || RAND(t, 2),
						m = naive_row(d->store, p.op, consts, p.nvals, &ref->v[i]);

			any |= m;
			if (in)
				bits[i >> 6] |= UINT64CONST(1) << (i & 63);
			if (in && m)
				want[i >> 6] |= UINT64CONST(1) << (i & 63);
		}
		sift_ctest_run(&test, ch, bits);
		CHECK(t, memcmp(bits, want, nw * sizeof(uint64)) == 0, "%s: op %d (%d constants, kind %d) selects the wrong rows of %u",
			  what, p.op, p.nvals, test.kind, n);
		CHECK(t, !any || sift_ctest_zone(&test, z), "%s: op %d (kind %d) prunes a chunk with matches", what, p.op, test.kind);
	}
}

/* The bytes of a string list chunk are the reference values; NULL slots are empty */
void
check_strs(TCtx *t, const char *what, const Ref *ref, const SiftCChunk *ch)
{
	uint32	   *off;
	char	   *bytes;
	uint32		bad = 0;

	sift_cchunk_strs(ch, CurrentMemoryContext, &off, &bytes);
	for (uint32 i = 0; i < ref->n; i++)
	{
		uint32		len = ref_null(&ref->v[i]) ? 0 : ref->v[i].len;

		bad += off[i + 1] - off[i] != len || (len > 0 && memcmp(bytes + off[i], ref->v[i].v.s, len) != 0);
	}
	CHECK(t, bad == 0, "%s: %u strings differ", what, bad);
	pfree(off);
	pfree(bytes);
}

/* The byte lengths of a string chunk's rows against sift_cchunk_strs_length */
void
check_lengths(TCtx *t, const char *what, const Ref *ref, const SiftCChunk *ch)
{
	uint32		nw = SIFT_COL_NWORDS(ref->n);
	uint64	   *bits = palloc(nw * sizeof(uint64));

	for (int k = 0; k < 4; k++)
	{
		uint32		lo = RAND(t, 3) == 0 ? 0 : RAND(t, 300),
					hi = RAND(t, 4) == 0 ? PG_UINT32_MAX : lo + RAND(t, 400) - (k == 3 ? 5 : 0),
					bad = 0;

		sift_cchunk_strs_length(ch, lo, hi, bits);
		for (uint32 i = 0; i < ref->n; i++)
			bad += sift_col_bit_test(bits, i) != (!ref_null(&ref->v[i]) && ref->v[i].len >= lo && ref->v[i].len <= hi);
		CHECK(t, bad == 0, "%s: %u rows differ for lengths in [%u, %u]", what, bad, lo, hi);
	}
	pfree(bits);
}

/* Decodes the chunk (all, and a selection) against the reference keys */
void
check_decode(TCtx *t, const char *what, const SiftColDef *d, SiftCDict *segdict, const Ref *ref, const SiftCChunk *ch)
{
	int64	   *got = palloc((ref->n + SIFT_COL_PAD / 8) * sizeof(int64)),
			   *sel = palloc((ref->n + SIFT_COL_PAD / 8) * sizeof(int64));
	uint32	   *idx = palloc((ref->n + 1) * sizeof(uint32));
	uint32		m = 0,
				bad = 0,
				bad_sel;

	CHECK(t, ch->n == ref->n, "%s: chunk of %u values, expected %u", what, ch->n, ref->n);
	if (ch->n != ref->n)
		return;
	sift_cchunk_decode(ch, got);
	for (uint32 i = 0; i < ref->n; i++)
	{
		bad += got[i] != (ref_null(&ref->v[i]) ? 0 : key_of(d, segdict, &ref->v[i]));
		if (RAND(t, 4) == 0)
			idx[m++] = i;
	}
	CHECK(t, bad == 0, "%s: %u values decode wrongly", what, bad);
	sift_cchunk_decode_sel(ch, idx, m, sel);
	bad_sel = 0;
	for (uint32 k = 0; k < m; k++)
		bad_sel += sel[k] != got[idx[k]];
	CHECK(t, bad_sel == 0, "%s: %u selected values decode wrongly", what, bad_sel);
	if (d->store == SIFT_COLS_VAR)
		check_strs(t, what, ref, ch);
}

/* The tags of a chunk: the flags exactly when the unit was written with tags and the chunk has overflow values (TAGS) or kinds, and every accessor gives the tag of the slot */
void
check_tags(TCtx *t, const char *what, const Ref *ref, const SiftCChunk *ch, bool tags)
{
	uint8	   *all = palloc(ch->n + 1);
	uint64	   *bits = palloc((SIFT_COL_NWORDS(ch->n) + 1) * sizeof(uint64));
	uint32		bad = 0,
				badbits = 0;
	bool		kinds = false;

	for (uint32 i = 0; i < ch->n; i++)
		kinds |= tags && !ref->v[i].overflow && ref->v[i].tag != SIFT_COLTAG_UNKNOWN;
	CHECK(t, ((ch->flags & SIFT_COLC_TAGS) != 0) == (tags && ch->ovf != NULL), "%s: tags flag %d, tags %d, overflow %d", what,
		  (ch->flags & SIFT_COLC_TAGS) != 0, tags, ch->ovf != NULL);
	CHECK(t, ((ch->flags & SIFT_COLC_KINDS) != 0) == kinds, "%s: kinds flag %d, kinds %d", what, (ch->flags & SIFT_COLC_KINDS) != 0, kinds);
	CHECK(t, sift_cchunk_tags(ch, all) == (ch->tags != NULL || kinds), "%s: tags expanded for a chunk without them", what);
	for (uint32 i = 0; i < ch->n; i++)
	{
		uint8		want = tags ? ref->v[i].tag : SIFT_COLTAG_UNKNOWN;

		bad += sift_cchunk_ovf_tag(ch, i) != (ref->v[i].overflow ? want : SIFT_COLTAG_UNKNOWN) || ((ch->tags != NULL || kinds) && all[i] != want);
	}
	for (uint8 tag = 0; tag <= SIFT_COLTAG_MAX; tag++)
	{
		sift_cchunk_tag_bits(ch, tag, bits);
		for (uint32 i = 0; i < ch->n; i++)
		{
			uint8		have = tags ? ref->v[i].tag : SIFT_COLTAG_UNKNOWN;

			badbits += sift_col_bit_test(bits, i) != (tag == SIFT_COLTAG_UNKNOWN ? ref->v[i].overflow && have == tag : have == tag);
		}
	}
	CHECK(t, bad == 0 && badbits == 0, "%s: %u slots with a wrong tag, %u wrong in the tag bitmaps", what, bad, badbits);
}

/* A chunk with bad kinds does not parse: a tag that is no kind, a bit past the end, a slot of two kinds, a JSON null in a slot with a value; copy is the chunk, restored */
static void
check_kinds_corrupt(TCtx *t, const char *what, const uint8 *chunk, uint8 *copy, uint32 len)
{
	SiftCChunk	ch;
	Size		off[SIFT_COLTAG_MAX + 1] = {0},
				bytes,
				head;
	uint8		first = 0,
				other = 0;
	uint32		n;

	if (!sift_cchunk_parse(copy, len, &ch))
		return;
	n = ch.n;
	bytes = (Size) ((n + 63) / 64) * 8;
	for (uint8 tag = 1; tag <= SIFT_COLTAG_MAX; tag++)
		if (ch.kind[tag])
		{
			off[tag] = (const uint8 *) ch.kind[tag] - copy;
			if (first == 0)
				first = tag;
			else if (other == 0)
				other = tag;
		}
	head = off[first] - 8;
	copy[head] |= 1 << SIFT_COLTAG_OBJECT;
	CHECK(t, !sift_cchunk_parse(copy, len, &ch), "%s: a kind of objects parsed", what);
	copy[head] = chunk[head];
	if (n % 64 != 0)
	{
		Size		at = off[first] + (n / 64) * 8 + 7;

		copy[at] |= 0x80;
		CHECK(t, !sift_cchunk_parse(copy, len, &ch), "%s: a kind past the last slot parsed", what);
		copy[at] = chunk[at];
	}
	if (other)
	{
		memcpy(copy + off[other], chunk + off[first], bytes);
		CHECK(t, !sift_cchunk_parse(copy, len, &ch), "%s: a slot of two kinds parsed", what);
		memcpy(copy + off[other], chunk + off[other], bytes);
	}
}

/* A chunk with a bad tag does not parse: a nibble past 5, a set padding bit, the flag without an overflow bitmap */
void
check_tags_corrupt(TCtx *t, const char *what, const uint8 *chunk, uint32 len)
{
	SiftCChunk	ch;
	uint8	   *copy = (uint8 *) TYPEALIGN(8, palloc(len + 8 + SIFT_COL_PAD));
	Size		toff;
	uint32		nov = 0;

	memcpy(copy, chunk, len);
	memset(copy + len, 0, SIFT_COL_PAD);
	if (!sift_cchunk_parse(copy, len, &ch))
		return;
	if (ch.kmask)
		check_kinds_corrupt(t, what, chunk, copy, len);
	if (!sift_cchunk_parse(copy, len, &ch))
		return;
	if (ch.tags == NULL)
	{
		if (ch.ovf == NULL)
		{
			copy[2] |= SIFT_COLC_TAGS;
			CHECK(t, !sift_cchunk_parse(copy, len, &ch), "%s: the tags flag without overflow bits parsed", what);
		}
		return;
	}
	for (uint32 w = 0; w < (ch.n + 63) / 64; w++)
		nov += pg_popcount64(ch.ovf[w]);
	toff = ch.tags - copy;
	copy[toff] |= 0xF;
	CHECK(t, !sift_cchunk_parse(copy, len, &ch), "%s: a tag nibble of 15 parsed", what);
	copy[toff] = chunk[toff];
	if (nov % 2)
	{
		copy[toff + nov / 2] |= 0x10;
		CHECK(t, !sift_cchunk_parse(copy, len, &ch), "%s: a set unused nibble parsed", what);
		copy[toff + nov / 2] = chunk[toff + nov / 2];
	}
	if (((nov + 1) / 2 + 7) / 8 * 8 > (nov + 1) / 2)
	{
		Size		pad = toff + ((nov + 1) / 2 + 7) / 8 * 8 - 1;

		copy[pad] = 1;
		CHECK(t, !sift_cchunk_parse(copy, len, &ch), "%s: a set padding byte parsed", what);
	}
}

#endif							/* SIFT_TESTING */
