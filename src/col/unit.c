/*
 * col/unit.c - L1 of the column engine: reading a unit record, its row directory and its chunks (docs/COL.md 2.1).
 */
#include "core/host.h"

#include "col/enc.h"
#include "col/enc_int.h"
#include "core/popcount.h"

/* ---------------------------------------------------------------------------
 * 8. Units (docs/COL.md 2.1)
 * ------------------------------------------------------------------------- */

#define UNIT_HEAD		(16 + SIFT_PAGEMASK_WORDS * 8)

/* Bytes of the offset bitmap of page p (0 for a dense page) */
static inline uint32
page_sparse_len(const SiftCUnitView *v, uint32 p)
{
	return v->maxoff[p] == v->rowstart[p + 1] - v->rowstart[p] ? 0 : (v->maxoff[p] + 7) / 8;
}

static uint32
page_sparse_off(const SiftCUnitView *v, uint32 p)
{
	uint32		off = 0;

	for (uint32 q = 0; q < p; q++)
		off += page_sparse_len(v, q);
	return off;
}

/* Writes the offsets (1-based) of the rows of page p, which has rowstart[p + 1] - rowstart[p] of them; sparse: its bitmap */
static void
page_offsets(const SiftCUnitView *v, uint32 p, const uint8 *bitmap, uint16 *offs)
{
	uint32		cnt = v->rowstart[p + 1] - v->rowstart[p],
				n = 0;

	for (uint32 o = 1; o <= v->maxoff[p]; o++)
		if (bitmap == NULL || (bitmap[(o - 1) / 8] >> ((o - 1) % 8)) & 1)
			offs[n++] = (uint16) o;
	Assert(n == cnt);
	(void) cnt;
}

/* The bits of the sparse bitmap of a page: exactly cnt of them, the last at maxoff, none past it */
static bool
sparse_valid(const uint8 *bm, uint32 mo, uint32 cnt)
{
	uint32		pop = 0;

	for (uint32 i = 0; i < (mo + 7) / 8; i++)
		pop += sift_pc64(bm[i]);
	return pop == cnt && ((bm[(mo - 1) / 8] >> ((mo - 1) % 8)) & 1) && (mo % 8 == 0 || (bm[(mo - 1) / 8] >> (mo % 8)) == 0);
}

/* The strings chunks belong to the VAR (STRS) and the STR (ISTR) leaves, and to no other */
static bool
chunk_fits_leaf(const SiftColDef *def, const SiftCChunk *c)
{
	return (c->enc == SIFT_COLENC_STRS) == (def->store == SIFT_COLS_VAR) && (c->enc != SIFT_COLENC_ISTR || leaf_is_str(def));
}

/*
 * avail: the bytes of the record present (the head of a unit read lazily, docs/COL.md 7), len: its full length. chunks: also validate every
 * chunk (needs all of them present).
 */
static bool
unit_parse(const uint8 *rec, uint32 avail, uint32 len, const SiftColSchema *schema, SiftCUnitView *v, bool chunks)
{
	uint32		head[4];
	uint64		pos = UNIT_HEAD,
				dirend,
				pop = 0;
	uint32		np;

	if (avail < UNIT_HEAD || avail > len || ((uintptr_t) rec & 7) != 0)
		return false;
	memset(v, 0, sizeof(*v));
	memcpy(head, rec, sizeof(head));
	memcpy(v->pagemask, rec + sizeof(head), sizeof(v->pagemask));
	np = head[2];
	for (int w = 0; w < SIFT_PAGEMASK_WORDS; w++)
		pop += sift_pc64(v->pagemask[w]);
	if (head[0] != COLU_MAGIC || head[1] > SIFT_COL_UNIT_MAX_ROWS || np > SIFT_SB_PAGES || pop != np || (np == 0) != (head[1] == 0))
		return false;
	v->rec = rec;
	v->len = len;
	v->schema = schema;
	v->nrows = head[1];
	v->npages = np;
	for (uint32 w = 0, p = 0; w < SIFT_PAGEMASK_WORDS; w++)
		for (uint64 m = v->pagemask[w]; m; m &= m - 1)
			v->page[p++] = (uint8) (w * 64 + pg_rightmost_one_pos64(m));
	if (pos + (uint64) (np + 1) * 4 + (uint64) np * 2 > avail)
		return false;
	v->rowstart = (const uint32 *) (rec + pos);
	pos += (uint64) (np + 1) * 4;
	v->maxoff = (const uint16 *) (rec + pos);
	pos += (uint64) np * 2;
	v->sparse = rec + pos;
	if (v->rowstart[0] != 0 || v->rowstart[np] != v->nrows)
		return false;
	for (uint32 p = 0; p < np; p++)
	{
		uint32		cnt = v->rowstart[p + 1] - v->rowstart[p],
					mo = v->maxoff[p],
					sl;

		if (v->rowstart[p + 1] <= v->rowstart[p] || mo < cnt || mo > MaxHeapTuplesPerPage)
			return false;
		sl = page_sparse_len(v, p);
		if (pos + sl > avail || (sl > 0 && !sparse_valid(rec + pos, mo, cnt)))
			return false;
		pos += sl;
	}
	pos = (pos + 3) & ~UINT64CONST(3);
	if (pos + 8 * (uint64) schema->ncols > avail)
		return false;
	v->chunk_off = (const uint32 *) (rec + pos);
	v->chunk_len = v->chunk_off + schema->ncols;
	dirend = (pos + 8 * (uint64) schema->ncols + 7) & ~UINT64CONST(7);
	for (int col = 0; col < schema->ncols; col++)
	{
		SiftCChunk	c;

		if (v->chunk_len[col] == 0)
			continue;
		if (v->chunk_off[col] % 8 != 0 || v->chunk_off[col] < dirend || (uint64) v->chunk_off[col] + v->chunk_len[col] > len ||
			(chunks && (!sift_cchunk_parse(rec + v->chunk_off[col], v->chunk_len[col], &c) || (schema->cols[col].parent < 0 && c.n != v->nrows) ||
						!chunk_fits_leaf(&schema->cols[col], &c))))
			return false;
	}
	return true;
}

bool
sift_cunit_parse(const uint8 *rec, uint32 len, const SiftColSchema *schema, SiftCUnitView *v)
{
	return unit_parse(rec, len, len, schema, v, true);
}

bool
sift_cunit_parse_head(const uint8 *rec, uint32 avail, uint32 len, const SiftColSchema *schema, SiftCUnitView *v)
{
	return unit_parse(rec, avail, len, schema, v, false);
}

bool
sift_cunit_chunk(const SiftCUnitView *v, int col, SiftCChunk *c)
{
	if (v->chunk_len[col] == 0)
		return false;
	if (!sift_cchunk_parse(v->rec + v->chunk_off[col], v->chunk_len[col], c))
		ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column unit: the chunk of leaf %d does not parse", col)));
	return true;
}

/* The p-th set bit of the page mask: the page of the superblock (the table is built when the unit is parsed) */
static uint32
unit_page(const SiftCUnitView *v, uint32 p)
{
	SIFT_INVARIANT(p < v->npages, "sift_col: page index %u is outside the unit", p);
	return v->page[p];
}

BlockNumber
sift_cunit_page_block(const SiftCUnitView *v, uint32 sb, uint32 p)
{
	return sb * SIFT_SB_PAGES + unit_page(v, p);
}

uint32
sift_cunit_page_sparse_len(const SiftCUnitView *v, uint32 p)
{
	return page_sparse_len(v, p);
}

void
sift_cunit_page_offmask_at(const SiftCUnitView *v, uint32 p, uint32 soff, uint64 *offmask)
{
	uint32		cnt = v->rowstart[p + 1] - v->rowstart[p],
				sl = page_sparse_len(v, p);

	memset(offmask, 0, SIFT_OFFMASK_WORDS * sizeof(uint64));
	if (sl > 0)
		memcpy(offmask, v->sparse + soff, sl);
	else
		for (uint32 w = 0; w * 64 < cnt; w++)
			offmask[w] = cnt - w * 64 >= 64 ? ~UINT64CONST(0) : (UINT64CONST(1) << (cnt - w * 64)) - 1;
}

void
sift_cunit_page_offmask(const SiftCUnitView *v, uint32 p, uint64 *offmask)
{
	sift_cunit_page_offmask_at(v, p, page_sparse_off(v, p), offmask);
}

uint32
sift_cunit_row_page(const SiftCUnitView *v, uint32 row)
{
	uint32		lo = 0,
				hi = v->npages;		/* rowstart[lo] <= row < rowstart[hi] */

	while (hi - lo > 1)
	{
		uint32		mid = (lo + hi) / 2;

		if (v->rowstart[mid] <= row)
			lo = mid;
		else
			hi = mid;
	}
	return lo;
}

void
sift_cunit_keys(const SiftCUnitView *v, uint32 sb, SiftKey *keys)
{
	const uint8 *bm = v->sparse;
	uint16		offs[MaxHeapTuplesPerPage];

	for (uint32 p = 0; p < v->npages; p++)
	{
		uint32		sl = page_sparse_len(v, p);
		SiftKey		blk = sift_cunit_page_block(v, sb, p);

		page_offsets(v, p, sl > 0 ? bm : NULL, offs);
		for (uint32 r = v->rowstart[p]; r < v->rowstart[p + 1]; r++)
			keys[r] = (blk << 16) | offs[r - v->rowstart[p]];
		bm += sl;
	}
}

/* The keys of the rows ords (ascending) only: the pages that hold them are decoded, the others skipped */
void
sift_cunit_row_keys(const SiftCUnitView *v, uint32 sb, const uint32 *ords, uint32 n, SiftKey *out)
{
	const uint8 *bm = v->sparse;
	uint16		offs[MaxHeapTuplesPerPage];
	uint32		k = 0;

	for (uint32 p = 0; p < v->npages && k < n; p++)
	{
		uint32		sl = page_sparse_len(v, p);

		if (ords[k] < v->rowstart[p + 1])
		{
			SiftKey		blk = sift_cunit_page_block(v, sb, p);

			page_offsets(v, p, sl > 0 ? bm : NULL, offs);
			for (; k < n && ords[k] < v->rowstart[p + 1]; k++)
				out[k] = (blk << 16) | offs[ords[k] - v->rowstart[p]];
		}
		bm += sl;
	}
}
