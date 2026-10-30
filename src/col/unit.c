/*
 * col/unit.c - L1 of the column engine: reading a unit record, its row directory and its chunks.
 */
#include "core/host.h"

#include "col/enc.h"
#include "col/enc_int.h"
#include "col/kern.h"
#include "core/popcount.h"

/* ---------------------------------------------------------------------------
 * 8. Units
 * ------------------------------------------------------------------------- */

#define UNIT_HEAD		(16 + SIFT_PAGEMASK_WORDS * 8)

/* Bytes of the offset bitmap of page p (0 for a dense page) */
static inline uint32
page_sparse_len(const SiftCUnitView *v, uint32 p)
{
	if (v->keyed)
		return 0;
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
	uint32		nb = (mo + 7) / 8,
				pop = 0,
				i = 0;

	for (; i + 8 <= nb; i += 8)
	{
		uint64		w;

		memcpy(&w, bm + i, sizeof(w));
		pop += sift_pc64(w);
	}
	if (i < nb)
	{
		uint64		w = 0;

		memcpy(&w, bm + i, nb - i);
		pop += sift_pc64(w);
	}
	return pop == cnt && ((bm[(mo - 1) / 8] >> ((mo - 1) % 8)) & 1) && (mo % 8 == 0 || (bm[(mo - 1) / 8] >> (mo % 8)) == 0);
}

/* The strings chunks belong to the VAR (STRS) and the STR (ISTR) leaves, and to no other */
static bool
chunk_fits_leaf(const SiftColDef *def, const SiftCChunk *c)
{
	return (c->enc == SIFT_COLENC_STRS) == (def->store == SIFT_COLS_VAR) && (c->enc != SIFT_COLENC_ISTR || leaf_is_str(def));
}

/*
 * Reads n values of bits each (LSB first) from p, which holds avail bytes; false on a short buffer. The groups of 8 values that lie SIFT_COL_PAD bytes from the end of the
 * buffer go to the unpack kernel (the stream is the one of the column codec, the kernel reads that far past a group), then eight bytes at a time while they are there.
 */
static bool
get_packed(const uint8 *p, uint64 avail, uint32 n, int bits, uint32 *out)
{
	uint64		mask = (UINT64CONST(1) << bits) - 1,
				at = 0,
				bitpos = 0;
	uint32		i = 0;

	if (((uint64) n * bits + 7) / 8 > avail)
		return false;
	if (bits >= 1 && bits <= 31 && avail >= SIFT_COL_PAD + (uint64) bits)
	{
		i = Min(n / 8, (avail - SIFT_COL_PAD) / bits) * 8;
		sift_colk->unpack(p, bits, i, out);
		bitpos = (uint64) i * bits;
	}
	for (; i < n && (bitpos >> 3) + 8 <= avail; i++, bitpos += bits)
	{
		uint64		w;

		memcpy(&w, p + (bitpos >> 3), sizeof(w));
		out[i] = (uint32) ((w >> (bitpos & 7)) & mask);
	}
	for (; i < n; i++, bitpos += bits)		/* the last bytes of the buffer */
	{
		uint64		acc = 0;
		int			nb = (int) (bitpos & 7);

		for (at = bitpos >> 3; nb < bits + (int) (bitpos & 7); at++, nb += 8)
			acc |= (uint64) p[at] << (nb - (int) (bitpos & 7));
		out[i] = (uint32) ((acc >> (bitpos & 7)) & mask);
	}
	return true;
}

/* The bytes the packed directory of a key-cut unit takes after its 24-byte head */
static uint64
keyed_dir_bytes(uint32 nrows, uint32 np, const uint32 *bw)
{
	return 24 + ((uint64) (np > 0 ? np - 1 : 0) * bw[0] + 7) / 8 + ((uint64) np * bw[1] + 7) / 8 + ((uint64) nrows * bw[2] + 7) / 8;
}

/*
 * The bytes of a plain unit from its start to the end of its chunk table, as far as the bytes read tell: the arrays of its directory (the pages and rows of each),
 * then, with those read, the bitmaps of its sparse pages and the table. 0 when the head is damaged (the parse refuses it).
 */
static uint64
plain_dir_end(const uint8 *rec, uint32 avail, uint32 np, int ncols)
{
	uint64		pos = UNIT_HEAD + (uint64) (np + 1) * 4 + (uint64) np * 2;
	const uint32 *rowstart = (const uint32 *) (rec + UNIT_HEAD);
	const uint16 *maxoff = (const uint16 *) (rec + UNIT_HEAD + (uint64) (np + 1) * 4);

	if (pos > avail)
		return pos;
	for (uint32 p = 0; p < np; p++)
	{
		uint32		cnt = rowstart[p + 1] - rowstart[p];

		if (rowstart[p + 1] <= rowstart[p] || maxoff[p] < cnt || maxoff[p] > MaxHeapTuplesPerPage)
			return 0;
		pos += maxoff[p] == cnt ? 0 : (maxoff[p] + 7) / 8;
	}
	return ((pos + 3) & ~UINT64CONST(3)) + 8 * (uint64) ncols;
}

/*
 * The bytes of a record from its start to the end of its chunk table, when more than the avail bytes read are needed to parse it: a key-cut unit whose directory is
 * longer than the bytes read (the 24-byte head is enough to know it), or a plain unit whose directory and chunk table are (a step at a time: the sizes of its
 * pages first, then its bitmaps); 0 for a record that avail covers or that is damaged.
 */
uint32
sift_cunit_head_need(const uint8 *rec, uint32 avail, uint32 len, int ncols)
{
	uint32		head[6];
	uint32		bw[3];
	uint64		need;

	if (avail < 24 || ((uintptr_t) rec & 7) != 0)
		return 0;
	memcpy(head, rec, sizeof(head));
	if (head[0] == COLU_MAGIC)
	{
		if (avail < UNIT_HEAD || head[2] > SIFT_SB_PAGES)
			return 0;
		need = plain_dir_end(rec, avail, head[2], ncols);
		return need > avail && need <= len ? (uint32) need : 0;
	}
	if (head[0] != COLK_MAGIC)
		return 0;
	bw[0] = head[3] & 0xFF;
	bw[1] = (head[3] >> 8) & 0xFF;
	bw[2] = (head[3] >> 16) & 0xFF;
	if (head[1] > SIFT_COL_UNIT_MAX_ROWS || head[2] > head[1] || bw[0] > 32 || bw[1] > 32 || bw[2] > 16)
		return 0;
	need = head[3] & COLK_TABFIRST ? 24 + 8 * (uint64) ncols : ((keyed_dir_bytes(head[1], head[2], bw) + 3) & ~UINT64CONST(3)) + 8 * (uint64) ncols;
	return need > avail && need <= len ? (uint32) need : 0;
}

/* Decodes the directory into the arrays of v; false when it is damaged */
static bool
keyed_decode(SiftCUnitView *v, uint32 avail)
{
	uint32		nrows = v->nrows,
				np = v->npages,
				bw[3] = {v->kwidths & 0xFF, (v->kwidths >> 8) & 0xFF, (v->kwidths >> 16) & 0xFF};
	uint64		at = v->kbody_at != 0 ? v->kbody_at : 24;
	const uint8 *p = v->rec + at;
	uint32	   *delta = palloc(Max(np, 1) * sizeof(uint32)),
			   *cnt = palloc(Max(np, 1) * sizeof(uint32)),
			   *off = palloc(Max(nrows, 1) * sizeof(uint32));
	BlockNumber *blocks = palloc(Max(np, 1) * sizeof(BlockNumber));
	uint32	   *rowstart = palloc((np + 1) * sizeof(uint32));
	uint16	   *koff = palloc(Max(nrows, 1) * sizeof(uint16));

	uint64		left = avail - at;		/* bytes from p to the end of the record: get_packed reads ahead inside it only */

	if (!get_packed(p, left, np > 0 ? np - 1 : 0, bw[0], delta))
		return false;
	at = ((uint64) (np > 0 ? np - 1 : 0) * bw[0] + 7) / 8;
	p += at;
	left -= at;
	if (!get_packed(p, left, np, bw[1], cnt))
		return false;
	at = ((uint64) np * bw[1] + 7) / 8;
	p += at;
	left -= at;
	if (!get_packed(p, left, nrows, bw[2], off))
		return false;
	rowstart[0] = 0;
	for (uint32 q = 0; q < np; q++)
	{
		uint64		blk = q == 0 ? v->kblk0 : (uint64) blocks[q - 1] + delta[q - 1] + 1;
		uint32		r0,
					r1;

		if (blk >= MaxBlockNumber)
			return false;
		blocks[q] = (BlockNumber) blk;
		r0 = rowstart[q];
		r1 = rowstart[q + 1] = r0 + cnt[q] + 1;
		if (r1 > nrows)
			return false;
		for (uint32 r = r0; r < r1; r++)
		{
			if (off[r] >= MaxHeapTuplesPerPage || (r > r0 && off[r] <= off[r - 1]))
				return false;
			koff[r] = (uint16) (off[r] + 1);
		}
	}
	if (rowstart[np] != nrows || (np > 0 && blocks[np - 1] != v->kblk1))
		return false;
	v->rowstart = rowstart;
	v->blocks = blocks;
	v->koff = koff;
	pfree(delta);
	pfree(cnt);
	pfree(off);
	return true;
}

/* The bytes of the directory of a key-cut unit read to the end of its chunk table are read now, where a scan needs the pages or the TIDs */
static void
keyed_body(SiftCUnitView *v)
{
	if (v->kbody_at != 0 && !v->kbody_here)
	{
		SIFT_INVARIANT(v->fill != NULL, "sift_col: the directory of a key-cut unit is not read");
		v->fill(v->fill_seg, v->fill_u, v->kbody_at, v->kbody_end - v->kbody_at);
		v->kbody_here = true;
	}
}

void
sift_cunit_keyed_corrupt(void)
{
	ereport(ERROR, (errcode(ERRCODE_DATA_CORRUPTED), errmsg("invalid column unit: the TID directory of a key-cut unit does not decode")));
}

void
sift_cunit_keyed_load(const SiftCUnitView *cv)
{
	SiftCUnitView *v = (SiftCUnitView *) cv;

	if (!v->keyed || v->blocks != NULL)
		return;
	keyed_body(v);
	if (!keyed_decode(v, v->len))
		sift_cunit_keyed_corrupt();
}

void
sift_cunit_keyed_walk_begin(const SiftCUnitView *cv, SiftCKeyedWalk *w)
{
	SiftCUnitView *v = (SiftCUnitView *) cv;
	uint32		np = v->npages,
				bw[3] = {v->kwidths & 0xFF, (v->kwidths >> 8) & 0xFF, (v->kwidths >> 16) & 0xFF};
	uint64		at = v->kbody_at != 0 ? v->kbody_at : 24,
				dbytes = ((uint64) (np > 0 ? np - 1 : 0) * bw[0] + 7) / 8,
				cbytes = ((uint64) np * bw[1] + 7) / 8;

	keyed_body(v);
	if (at + dbytes + cbytes + ((uint64) v->nrows * bw[2] + 7) / 8 > v->len)
		sift_cunit_keyed_corrupt();
	w->dp = v->rec + at;
	w->cp = w->dp + dbytes;
	w->op = w->cp + cbytes;
	w->dleft = v->len - at;
	w->cleft = w->dleft - dbytes;
	w->oleft = w->cleft - cbytes;
	w->bwd = bw[0];
	w->bwc = bw[1];
	w->bwo = bw[2];
	w->dbit = 0;
	w->cbit = 0;
	w->q = 0;
	w->np = np;
	w->nrows = v->nrows;
	w->row = 0;
	w->blk = v->kblk0;
	w->last = v->kblk1;
}

uint32
sift_cunit_keyed_walk_batch(SiftCKeyedWalk *w, BlockNumber *blk, uint32 *row, uint32 *cnt)
{
	uint32		n = Min(SIFT_CKEYED_BATCH, w->np - w->q),
				bwd = w->bwd,
				bwc = w->bwc;
	uint64		dbit = w->dbit,
				cbit = w->cbit,
				b = w->blk,
				r = w->row,
				dmask = (UINT64CONST(1) << bwd) - 1,
				cmask = (UINT64CONST(1) << bwc) - 1;
	/* eight bytes are read at the bit position: the last of the batch must leave them inside the record */
	bool		fast = ((dbit + (uint64) n * bwd) >> 3) + 8 <= w->dleft && ((cbit + (uint64) n * bwc) >> 3) + 8 <= w->cleft;

	for (uint32 i = 0; i < n; i++)
	{
		uint64		x,
					c;

		if (w->q + i > 0)
		{
			if (fast)
			{
				memcpy(&x, w->dp + (dbit >> 3), sizeof(x));
				x = (x >> (dbit & 7)) & dmask;
			}
			else
				x = sift_ckeyed_get(w->dp, w->dleft, dbit, bwd);
			dbit += bwd;
			b += x + 1;
		}
		if (fast)
		{
			memcpy(&c, w->cp + (cbit >> 3), sizeof(c));
			c = ((c >> (cbit & 7)) & cmask) + 1;
		}
		else
			c = sift_ckeyed_get(w->cp, w->cleft, cbit, bwc) + 1;
		cbit += bwc;
		if (c > MaxHeapTuplesPerPage)
			sift_cunit_keyed_corrupt();
		blk[i] = (BlockNumber) b;
		row[i] = (uint32) r;
		cnt[i] = (uint32) c;
		r += c;
	}
	if (b >= MaxBlockNumber || r > w->nrows)
		sift_cunit_keyed_corrupt();
	w->dbit = dbit;
	w->cbit = cbit;
	w->blk = (BlockNumber) b;
	w->row = (uint32) r;
	w->q += n;
	return n;
}

void
sift_cunit_keyed_walk_end(const SiftCKeyedWalk *w)
{
	if (w->q != w->np || w->row != w->nrows || (w->np > 0 && w->blk != w->last))
		sift_cunit_keyed_corrupt();
}

/* The head of a key-cut unit (enc.c unit_put_directory_keyed); pos is where its directory ends, or, with the table first, where its head does */
static bool
unit_parse_keyed(const uint8 *rec, uint32 avail, const uint32 *head, SiftCUnitView *v, uint64 *pos)
{
	uint32		nrows = head[1],
				np = head[2],
				bw[3] = {head[3] & 0xFF, (head[3] >> 8) & 0xFF, (head[3] >> 16) & 0xFF};

	if (nrows > SIFT_COL_UNIT_MAX_ROWS || np > nrows || (np == 0) != (nrows == 0) || bw[0] > 32 || bw[1] > 32 || bw[2] > 16 ||
		(head[3] >> 24) > (COLK_TABFIRST >> 24) || head[5] < head[4])
		return false;
	*pos = keyed_dir_bytes(nrows, np, bw);
	if (head[3] & COLK_TABFIRST)
	{
		v->kbody_end = (uint32) (*pos - 24);	/* its length until unit_parse_chunks knows where */
		*pos = 24;
	}
	if (*pos > avail)
		return false;
	v->keyed = true;
	v->npages = np;
	v->kdir = rec + 24;
	v->kwidths = head[3];
	v->kblk0 = head[4];
	v->kblk1 = head[5];
	return true;
}

/* The chunk table and the chunks of a record whose row directory ends at pos (shared by both directory kinds) */
static bool
unit_parse_chunks(const uint8 *rec, uint32 avail, uint32 len, const SiftColSchema *schema, SiftCUnitView *v, bool chunks, uint64 pos)
{
	uint64		dirend;

	pos = (pos + 3) & ~UINT64CONST(3);
	if (pos + 8 * (uint64) schema->ncols > avail)
		return false;
	v->chunk_off = (const uint32 *) (rec + pos);
	v->chunk_len = v->chunk_off + schema->ncols;
	dirend = pos + 8 * (uint64) schema->ncols;
	if (v->kwidths & COLK_TABFIRST)
	{
		v->kbody_at = (uint32) dirend;
		dirend += v->kbody_end;
		v->kbody_end = (uint32) dirend;
		v->kbody_here = dirend <= avail;
	}
	dirend = (dirend + 7) & ~UINT64CONST(7);
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

/*
 * avail: the bytes of the record present, len: its full length. chunks: also validate every
 * chunk (needs all of them present).
 */
static bool
unit_parse(const uint8 *rec, uint32 avail, uint32 len, const SiftColSchema *schema, SiftCUnitView *v, bool chunks)
{
	uint32		head[6];
	uint64		pos = UNIT_HEAD,
				pop = 0;
	uint32		np;

	if (avail >= 24 && avail <= len && ((uintptr_t) rec & 7) == 0)
	{
		memcpy(head, rec, sizeof(head));
		if (head[0] == COLK_MAGIC)
		{
			memset(v, 0, sizeof(*v));
			v->rec = rec;
			v->len = len;
			v->schema = schema;
			v->nrows = head[1];
			if (!unit_parse_keyed(rec, avail, head, v, &pos) || !unit_parse_chunks(rec, avail, len, schema, v, chunks, pos))
				return false;
			return !chunks || ((v->kbody_at == 0 || v->kbody_here) && keyed_decode(v, avail));
		}
	}
	if (avail < UNIT_HEAD || avail > len || ((uintptr_t) rec & 7) != 0)
		return false;
	memset(v, 0, sizeof(*v));
	memcpy(head, rec, 4 * sizeof(uint32));
	memcpy(v->pagemask, rec + 4 * sizeof(uint32), sizeof(v->pagemask));
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
	return unit_parse_chunks(rec, avail, len, schema, v, chunks, pos);
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
	if (v->keyed)
	{
		sift_cunit_keyed_load(v);
		return v->blocks[p];
	}
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
	if (v->keyed)
		sift_cunit_keyed_load(v);
	if (v->keyed)
		for (uint32 r = v->rowstart[p]; r < v->rowstart[p + 1]; r++)
			offmask[(v->koff[r] - 1) / 64] |= UINT64CONST(1) << ((v->koff[r] - 1) % 64);
	else if (sl > 0)
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

	sift_cunit_keyed_load(v);

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

	if (v->keyed)
	{
		sift_cunit_keyed_load(v);
		for (uint32 p = 0; p < v->npages; p++)
			for (uint32 r = v->rowstart[p]; r < v->rowstart[p + 1]; r++)
				keys[r] = ((SiftKey) v->blocks[p] << 16) | v->koff[r];
		return;
	}

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

/*
 * The rows of a unit that are in the superblock sb (a unit that is not key-cut) at the keys (ascending, those of the superblock): the number of the
 * row of each key the unit holds is put in rows, in order. The pages of the unit are bisected, not decoded.
 */
uint32
sift_cunit_find_rows(const SiftCUnitView *v, uint32 sb, const SiftKey *keys, uint32 n, uint32 *rows)
{
	uint32		p = 0,
				soff = 0,
				found = 0;

	Assert(!v->keyed);
	for (uint32 k = 0; k < n && p < v->npages; k++)
	{
		uint32		pg = (uint32) (keys[k] >> 16) - sb * SIFT_SB_PAGES,
					off = (uint32) (keys[k] & 0xFFFF);

		while (p < v->npages && unit_page(v, p) < pg)
			soff += page_sparse_len(v, p++);
		if (p == v->npages)
			break;
		if (unit_page(v, p) != pg || off < 1 || off > v->maxoff[p])
			continue;
		if (page_sparse_len(v, p) == 0)
			rows[found++] = v->rowstart[p] + off - 1;
		else
		{
			const uint8 *bm = v->sparse + soff;
			uint32		below = 0;

			if (!((bm[(off - 1) / 8] >> ((off - 1) % 8)) & 1))
				continue;
			for (uint32 i = 0; i < (off - 1) / 8; i++)
				below += sift_pc64(bm[i]);
			below += sift_pc64(bm[(off - 1) / 8] & ((1U << ((off - 1) % 8)) - 1));
			rows[found++] = v->rowstart[p] + below;
		}
	}
	return found;
}

/* The keys of the rows ords (ascending) only: the pages that hold them are decoded, the others skipped */
void
sift_cunit_row_keys(const SiftCUnitView *v, uint32 sb, const uint32 *ords, uint32 n, SiftKey *out)
{
	const uint8 *bm = v->sparse;
	uint16		offs[MaxHeapTuplesPerPage];
	uint32		k = 0;

	if (v->keyed)
	{
		sift_cunit_keyed_load(v);		/* before v->blocks is read: the directory is decoded on first use */
		for (; k < n; k++)
			out[k] = ((SiftKey) v->blocks[sift_cunit_row_page(v, ords[k])] << 16) | v->koff[ords[k]];
		return;
	}
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
