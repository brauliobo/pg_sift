/*
 * col/unit_test.c - L1 tests: the unit record, its directory, inline text, tags and corrupt copies.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "col/enc.h"
#include "col/enc_test_int.h"
#include "core/popcount.h"

/* bits 0..7: CONST, PACK of up to 7 / 9..15 / 17..31 bits, FOR 8 / 16 / 32 / 64; bit 8: a text leaf with codes of 16 bits or more */
static void
note_encoding(TCtx *t, SiftColStore store, const SiftCChunk *c)
{
	int			bit;

	if (c->enc == SIFT_COLENC_CC || c->enc == SIFT_COLENC_STRS || c->enc == SIFT_COLENC_ISTR)
	{
		t->encodings |= 1 << (c->enc == SIFT_COLENC_CC ? 9 : c->enc == SIFT_COLENC_STRS ? 10 : 11);
		return;
	}
	if (c->enc == SIFT_COLENC_CONST)
		bit = 0;
	else if (c->enc == SIFT_COLENC_PACK)
		bit = c->bits < 8 ? 1 : c->bits < 16 ? 2 : 3;
	else
		bit = 4 + (c->bits == 8 ? 0 : c->bits == 16 ? 1 : c->bits == 32 ? 2 : 3);
	t->encodings |= 1 << bit;
	if (store == SIFT_COLS_STR && c->enc != SIFT_COLENC_CONST && c->bits >= 16)
		t->encodings |= 1 << 8;
}

/* The segment dictionaries of the STR leaves: a few unrelated values first, then the batch's (so that the codes move) */
static SiftCDict **
seg_dicts(TCtx *t, const SiftColBatch *b, uint32 ***dictmap)
{
	int			nc = b->schema->ncols;
	SiftCDict **dicts = palloc0(nc * sizeof(SiftCDict *));

	*dictmap = palloc0(nc * sizeof(uint32 *));
	for (int col = 0; col < nc; col++)
	{
		if (b->dict[col] == NULL)
			continue;
		dicts[col] = sift_cdict_create(CurrentMemoryContext);
		for (uint32 i = RAND(t, 4); i > 0; i--)
			sift_cdict_intern(dicts[col], "\x01\x02seg", 3 + (i % 2));
		(*dictmap)[col] = palloc(Max(b->dict[col]->n, 1) * sizeof(uint32));
		for (uint32 code = 0; code < b->dict[col]->n; code++)
			(*dictmap)[col][code] = sift_cdict_intern(dicts[col], b->dict[col]->bytes + b->dict[col]->off[code], dict_len(b->dict[col], code));
	}
	return dicts;
}

static void
check_directory(TCtx *t, const SiftCUnitView *v, const SiftColBatch *b)
{
	SiftKey    *keys = palloc(Max(v->nrows, 1) * sizeof(SiftKey));
	uint32		bad = 0;

	CHECK(t, v->nrows == b->nrows, "unit of %u rows, expected %u", v->nrows, b->nrows);
	sift_cunit_keys(v, b->sb, keys);
	for (uint32 r = 0; r < b->nrows; r++)
	{
		uint32		p = sift_cunit_row_page(v, r);
		uint64		mask[SIFT_OFFMASK_WORDS];
		uint32		off = b->keys[r] & 0xFFFF;

		sift_cunit_page_offmask(v, p, mask);
		bad += keys[r] != b->keys[r] || sift_cunit_page_block(v, b->sb, p) != sift_key_block(b->keys[r]) ||
			!((mask[(off - 1) / 64] >> ((off - 1) % 64)) & 1) || r < v->rowstart[p] || r >= v->rowstart[p + 1];
	}
	CHECK(t, bad == 0, "%u rows of the directory differ", bad);
	if (v->keyed)
	{
		/* the walk of a key-cut unit's pages gives the blocks, rows and offsets of the keys */
		SiftCKeyedWalk w;
		BlockNumber blks[SIFT_CKEYED_BATCH];
		uint32		rows[SIFT_CKEYED_BATCH],
					cnts[SIFT_CKEYED_BATCH],
					nb,
					p = 0,
					diff = 0;

		sift_cunit_keyed_walk_begin(v, &w);
		while ((nb = sift_cunit_keyed_walk_batch(&w, blks, rows, cnts)) > 0)
			for (uint32 i = 0; i < nb; i++, p++)
			{
				uint16		offs[MaxHeapTuplesPerPage];

				sift_cunit_keyed_walk_offs(&w, rows[i], cnts[i], offs);
				diff += rows[i] != v->rowstart[p] || cnts[i] != v->rowstart[p + 1] - v->rowstart[p] || blks[i] != sift_key_block(keys[rows[i]]);
				for (uint32 k = 0; k < cnts[i]; k++)
					diff += offs[k] != (keys[rows[i] + k] & 0xFFFF);
			}
		sift_cunit_keyed_walk_end(&w);
		CHECK(t, diff == 0, "%u pages or offsets of the walk differ", diff);
	}
	for (uint32 p = 0; p < v->npages; p++)
	{
		uint64		mask[SIFT_OFFMASK_WORDS];
		uint32		pop = 0;

		sift_cunit_page_offmask(v, p, mask);
		for (int w = 0; w < SIFT_OFFMASK_WORDS; w++)
			pop += sift_pc64(mask[w]);
		CHECK(t, pop == v->rowstart[p + 1] - v->rowstart[p], "offset mask of page %u has %u bits", p, pop);
	}
}

/* Corrupt copies of the record must not crash the parser or read past the length */
static void
check_corrupt(TCtx *t, const uint8 *rec, uint32 len, const SiftColSchema *s)
{
	for (int k = 0; k < 12; k++)
	{
		uint32		cut = len - RAND(t, Min(len - 1, 4096));
		uint8	   *copy = palloc(cut + 8 + SIFT_COL_PAD);
		SiftCUnitView v;

		copy = (uint8 *) TYPEALIGN(8, copy);
		memcpy(copy, rec, cut);
		if (k % 2)
			copy[RAND(t, cut)] ^= (uint8) (1 << RAND(t, 8));
		(void) sift_cunit_parse(copy, cut, s, &v);
		if (!(k % 2) && cut < len)
			CHECK(t, !sift_cunit_parse(copy, cut, s, &v), "truncated unit (%u of %u bytes) parsed", cut, len);
	}
}

/* A policy per leaf: each of the codec's families in turn */
SiftCcPolicy *
random_policy(TCtx *t, int ncols)
{
	static const char *const specs[] = {"auto", "none", "lz4", "zstd:3", "for+zstd", "dict+lz4", "delta", "rle", "alp"};
	SiftCcPolicy *pol = palloc(ncols * sizeof(SiftCcPolicy));

	for (int i = 0; i < ncols; i++)
		if (sift_cc_policy_parse(specs[RAND(t, lengthof(specs))], &pol[i]) != NULL)
			elog(ERROR, "bad test policy");
	return pol;
}

/* The rule that sends a unit's text inline: a string over 255 bytes, or a dictionary that would pass the limit with the entries the unit adds */
static void
check_inline_rule(TCtx *t, const SiftColBatch *b, int col, SiftCDict *seg)
{
	const SiftCDict *bd = b->dict[col];
	Size		limit = RAND(t, 4) == 0 ? seg->nbytes + RAND(t, 200) : RAND(t, 2000),
				add = 0;
	bool		want = false;

	for (uint32 c = 0; c < bd->n; c++)
	{
		want |= dict_len(bd, c) > SIFT_COL_DICT_MAX_ENTRY;
		add += sift_cdict_find(seg, bd->bytes + bd->off[c], dict_len(bd, c)) < 0 ? dict_len(bd, c) : 0;
	}
	want |= seg->nbytes + add > limit;
	CHECK(t, sift_cbatch_str_inline(b, col, seg, limit) == want, "leaf %d: inline rule for a limit of %zu (dictionary %zu, adds %zu)", col, limit, seg->nbytes, add);
}

/* The rows as a unit written without tags (column format minor 3) returns them: an overflow value has no type, a slot no kind */
static SiftColRow *
rows_untagged(const Case *c, const SiftColRow *rows, uint32 n)
{
	const SiftColSchema *s = c->schema;
	SiftColRow *out = palloc(Max(n, 1) * sizeof(SiftColRow));

	for (uint32 i = 0; i < n; i++)
	{
		out[i] = rows[i];
		out[i].val = palloc(s->ncols * sizeof(SiftColValue));
		memcpy(out[i].val, rows[i].val, s->ncols * sizeof(SiftColValue));
		out[i].elem = palloc0((s->ncols + 1) * sizeof(SiftColValue *));
		for (int id = 0; id < s->ncols; id++)
		{
			int			parent = s->cols[id].parent;
			int64		cnt = parent >= 0 && rows[i].elem[id] != NULL ? rows[i].val[parent].v.i : 0;

			out[i].val[id].tag = 0;
			if (cnt == 0)
				continue;
			out[i].elem[id] = palloc(cnt * sizeof(SiftColValue));
			memcpy(out[i].elem[id], rows[i].elem[id], cnt * sizeof(SiftColValue));
			for (int64 j = 0; j < cnt; j++)
				out[i].elem[id][j].tag = 0;
		}
	}
	return out;
}

typedef struct HeadFill
{
	uint8	   *buf;
	const uint8 *rec;
} HeadFill;

static void
head_fill(void *seg, uint32 u, uint32 off, uint32 len)
{
	HeadFill   *h = seg;

	memcpy(h->buf + off, h->rec + off, len);
}

/* A key-cut unit read as a scan reads it: its head only (to the end of the chunk table with the table first), the directory read when the TIDs are asked for */
static void
check_head_lazy(TCtx *t, const uint8 *rec, uint32 len, const SiftColSchema *s, const SiftColBatch *b, bool tabfirst)
{
	HeadFill	h = {palloc0(len + 8 + SIFT_COL_PAD), rec};
	uint32		avail = Min(len, SIFT_COL_UNIT_HEAD);
	SiftKey    *keys = palloc(Max(b->nrows, 1) * sizeof(SiftKey));
	SiftCUnitView v;
	uint32		bad = 0;

	h.buf = (uint8 *) TYPEALIGN(8, h.buf);
	memcpy(h.buf, rec, avail);
	for (uint32 need; avail < len && (need = sift_cunit_head_need(h.buf, avail, len, s->ncols)) > 0;)
	{
		memcpy(h.buf + avail, rec + avail, need - avail);
		avail = need;
	}
	if (!sift_cunit_parse_head(h.buf, avail, len, s, &v))
	{
		CHECK(t, false, "the head of a key-cut unit does not parse");
		return;
	}
	CHECK(t, !tabfirst || avail == len || avail <= Max(SIFT_COL_UNIT_HEAD, 24 + 8 * s->ncols), "a unit with the table first read %u bytes of its head", avail);
	CHECK(t, (v.kbody_at != 0) == tabfirst, "the table is %s", tabfirst ? "not first" : "first");
	v.fill = head_fill;
	v.fill_seg = &h;
	sift_cunit_keys(&v, b->sb, keys);
	for (uint32 r = 0; r < b->nrows; r++)
		bad += keys[r] != b->keys[r];
	CHECK(t, bad == 0, "%u keys differ after the directory was read lazily", bad);
}

void
check_unit(TCtx *t, const Case *c, const SiftColBatch *b0, const SiftColRow *rows, uint32 n, const SiftCcPolicy *pol)
{
	const SiftColSchema *s = c->schema;
	SiftColBatch kb = *b0;
	const SiftColBatch *b = &kb;
	bool		tabfirst;
	uint32	  **dictmap = NULL;
	bool		identity = RAND(t, 4) == 0;		/* the segment's dictionaries are the batch's: no map */
	SiftCDict **dicts = identity ? b->dict : seg_dicts(t, b, &dictmap);
	SiftCZone  *zones = palloc(s->ncols * sizeof(SiftCZone));
	bool	   *inl = palloc0(s->ncols * sizeof(bool));
	StringInfoData buf;
	Size		start;
	uint8	   *rec;
	SiftCUnitView v;
	SiftColBatch *dst;
	uint64	   *live;
	bool		tags = RAND(t, 4) != 0;
	const SiftColRow *want = tags ? rows : rows_untagged(c, rows, n);

	for (int col = 0; col < s->ncols; col++)
	{
		inl[col] = b->dict[col] != NULL && (sift_cbatch_str_inline(b, col, dicts[col], SIFT_COL_DICT_MAX_BYTES) || RAND(t, 3) == 0);
		if (b->dict[col] != NULL && b->col[col].n > 0)
			check_inline_rule(t, b, col, dicts[col]);
	}
	kb.keyed = RAND(t, 3) == 0;
	tabfirst = kb.keyed && RAND(t, 2) == 0;
	initStringInfo(&buf);
	appendStringInfoString(&buf, "x");	/* the record starts after a pad: encode aligns it */
	sift_cunit_encode(b, dictmap, inl, pol, tags, tabfirst, &buf, &start, zones);
	{
		static const char zero[SIFT_COL_PAD];

		CHECK(t, buf.maxlen - buf.len >= SIFT_COL_PAD && memcmp(buf.data + buf.len, zero, SIFT_COL_PAD) == 0, "no zero pad after the record");
	}
	rec = palloc(buf.len - start + 8 + SIFT_COL_PAD);
	rec = (uint8 *) TYPEALIGN(8, rec);
	memcpy(rec, buf.data + start, buf.len - start);
	memset(rec + (buf.len - start), 0, SIFT_COL_PAD);
	CHECK(t, start % 8 == 0 && start == 8, "unit starts at %zu", start);
	if (!sift_cunit_parse(rec, buf.len - start, s, &v))
	{
		CHECK(t, false, "unit does not parse");
		return;
	}
	check_directory(t, &v, b);

	for (int col = 0; col < s->ncols; col++)
	{
		Ref			ref = ref_leaf(c, rows, n, col);
		SiftCChunk	ch;
		SiftCZone	z2;
		char		what[64];

		snprintf(what, sizeof(what), "leaf %d (store %d)", col, s->cols[col].store);
		if (!sift_cunit_chunk(&v, col, &ch))
		{
			CHECK(t, false, "%s: no chunk", what);
			continue;
		}
		note_encoding(t, s->cols[col].store, &ch);
		check_tags(t, what, &ref, &ch, tags);
		check_tags_corrupt(t, what, rec + v.chunk_off[col], v.chunk_len[col]);
		sift_cchunk_zone(&ch, s->cols[col].store, &z2);
		CHECK(t, memcmp(&z2, &zones[col], sizeof(z2)) == 0, "%s: recomputed zone differs from the encoder's", what);
		CHECK(t, (ch.enc == SIFT_COLENC_ISTR) == (inl[col] && ch.n > 0), "%s: inline chunk %d, asked %d", what, ch.enc == SIFT_COLENC_ISTR, inl[col]);
		if (ch.enc == SIFT_COLENC_ISTR)
		{
			SiftCDict  *own;
			SiftCChunk	codes;

			check_strs(t, what, &ref, &ch);
			check_zone_inline(t, what, &ref, &zones[col], tags);
			CHECK(t, sift_cchunk_istr_codes(&ch, CurrentMemoryContext, NULL, 0, &own, &codes), "%s: no codes of an inline chunk", what);
			check_decode(t, what, &s->cols[col], own, &ref, &codes);
			check_tests(t, what, &s->cols[col], own, &ref, &codes, &zones[col], false);
			check_tests(t, what, &s->cols[col], NULL, &ref, &ch, &zones[col], true);
			check_lengths(t, what, &ref, &ch);
			continue;
		}
		check_decode(t, what, &s->cols[col], dicts[col], &ref, &ch);
		check_zone(t, what, &s->cols[col], dicts[col], &ref, &zones[col], tags);
		check_tests(t, what, &s->cols[col], dicts[col], &ref, &ch, &zones[col], false);
		if (s->cols[col].store == SIFT_COLS_VAR)
			check_lengths(t, what, &ref, &ch);
	}

	dst = sift_cbatch_create(CurrentMemoryContext, s);
	sift_cbatch_reset(dst, b->sb);
	sift_cunit_decode(&v, b->sb, dicts, NULL, dst);
	check_batch_rows(t, "unit decode", c, dst, want, n);

	live = palloc0(SIFT_COL_NWORDS(n) * sizeof(uint64));
	{
		SiftColRow *kept = palloc(Max(n, 1) * sizeof(SiftColRow));
		uint32		nk = 0;

		for (uint32 r = 0; r < n; r++)
			if (RAND(t, 3) != 0)
			{
				live[r >> 6] |= UINT64CONST(1) << (r & 63);
				kept[nk++] = want[r];
			}
		sift_cbatch_reset(dst, b->sb);
		sift_cunit_decode(&v, b->sb, dicts, live, dst);
		check_batch_rows(t, "unit decode of live rows", c, dst, kept, nk);
	}
	if (b->keyed)
		check_head_lazy(t, rec, buf.len - start, s, b, tabfirst);
	check_corrupt(t, rec, buf.len - start, s);
}

#endif							/* SIFT_TESTING */
