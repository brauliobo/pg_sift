/*
 * table/f2/f2_row_pack.c - SEC_ROW_PACK of a part of a sift_table: the writer and the reader of its blocks (f2_row_pack.h)
 */
#include "postgres.h"

#include "col/codec.h"
#include "col/compress.h"
#include "core/catname.h"
#include "core/damage.h"
#include "core/memtrack.h"
#include "table/f2/f2_row_pack.h"

#ifdef SIFT_TESTING
#include "access/relation.h"
#include "fmgr.h"
#include "utils/builtins.h"

#include "col/f2/f2_cache.h"

static uint64 blocks_read,
			rows_read,
			external_cells;
static int	share_percent = SIFT_ROW_PACK_SHARE_PERCENT;

#define ROW_PACK_SHARE_PERCENT	share_percent
#else
#define ROW_PACK_SHARE_PERCENT	SIFT_ROW_PACK_SHARE_PERCENT
#endif

#define RPACK_NO_VALUE	UINT32_MAX

struct SiftRowPackRow
{
	uint16		ncols;
	uint32		len;
	const SiftRowPackCol *cols;
	const char *row;
	uint32	   *off;			/* of each column: where its value starts in the row, RPACK_NO_VALUE for a NULL */
};

static inline int
null_bytes(int ncols)
{
	return (ncols + 7) / 8;
}

static inline void
pad8(StringInfo s)
{
	while (s->len % 8 != 0)
		appendStringInfoChar(s, '\0');
}

/* ---- the writer ---- */

static bool
tags_clear(const SiftColIn *c, int n)
{
	for (int r = 0; c->tag != NULL && r < n; r++)
		if (c->tag[r] != 0)
			return false;
	return true;
}

static bool
is_null(const SiftColIn *c, int r)
{
	return c->isnull != NULL && c->isnull[r] && (c->tag == NULL || c->tag[r] == 0);
}

static const char *
str_of(const SiftColIn *c, int r, uint32 *len)
{
	const uint32 code = c->s != NULL ? 0 : c->dcode[r];

	*len = c->s != NULL ? c->slen[r] : c->pdict->len[code];
	return c->s != NULL ? c->s[r] : c->pdict->p[code];
}

static uint8
width_of(uint64 range)
{
	return range <= UINT8_MAX ? 1 : range <= UINT16_MAX ? 2 : range <= UINT32_MAX ? 4 : 8;
}

/* How column c of rows is stored, or false when the part cannot have a pack: arrays of objects, an exception in a column of integers or floats */
static bool
column_layout(const SiftRows *rows, int c, SiftRowPackCol *pc)
{
	const SiftColIn *col = &rows->cols[c];
	int64		lo = INT64_MAX,
				hi = INT64_MIN;

	*pc = (SiftRowPackCol) {.kind = (uint8) col->kind,.width = col->kind == SIFT_CELL_F8 ? 8 : 0};
	if (col->group1 > 0 || col->eoff != NULL || (col->kind != SIFT_CELL_STR && !tags_clear(col, rows->nrows)))
		return false;
	if (col->kind != SIFT_CELL_INT)
		return true;
	for (int r = 0; r < rows->nrows; r++)
		if (!is_null(col, r))
		{
			lo = Min(lo, col->i[r]);
			hi = Max(hi, col->i[r]);
		}
	pc->base = lo > hi ? 0 : lo;
	pc->width = width_of(lo > hi ? 0 : (uint64) hi - (uint64) lo);
	return true;
}

typedef struct Packer
{
	const SiftRows *rows;
	SiftRowPackCol *cols;
	uint8	   *null_bm;
	bool		has_wide;
} Packer;

static bool
string_is_external(const SiftColIn *c, int r, uint32 len)
{
	return (c->tag != NULL && c->tag[r] != 0) || len > SIFT_ROW_PACK_INLINE_MAX;
}

static void
append_fixed(StringInfo b, const SiftColIn *c, const SiftRowPackCol *pc, int r)
{
	if (c->kind == SIFT_CELL_F8)
		appendBinaryStringInfo(b, (const char *) &c->f[r], sizeof(double));
	else
	{
		uint64		v = (uint64) c->i[r] - (uint64) pc->base;

		appendBinaryStringInfo(b, (const char *) &v, pc->width);
	}
}

static void
append_string(Packer *pk, StringInfo b, const SiftColIn *c, int r)
{
	uint32		len;
	const char *p = str_of(c, r, &len);
	uint16		stored = string_is_external(c, r, len) ? SIFT_ROW_PACK_EXT_SENTINEL : (uint16) len;

	appendBinaryStringInfo(b, (const char *) &stored, sizeof(stored));
	if (stored == SIFT_ROW_PACK_EXT_SENTINEL)
		pk->has_wide = true;
	else
		appendBinaryStringInfo(b, p, len);
}

static void
append_row(Packer *pk, StringInfo b, int r)
{
	const SiftRows *rows = pk->rows;
	const int	nbm = null_bytes(rows->ncols);
	uint16		fixed_bytes;
	int			fixed_at;

	memset(pk->null_bm, 0, nbm);
	for (int c = 0; c < rows->ncols; c++)
		if (is_null(&rows->cols[c], r))
			pk->null_bm[c / 8] |= (uint8) (1 << (c % 8));
	appendBinaryStringInfo(b, (const char *) &rows->tid[r], sizeof(uint64));
	appendBinaryStringInfo(b, (const char *) pk->null_bm, nbm);
	fixed_at = b->len;
	appendStringInfoSpaces(b, sizeof(fixed_bytes));
	for (int c = 0; c < rows->ncols; c++)
		if (rows->cols[c].kind != SIFT_CELL_STR && !(pk->null_bm[c / 8] & (1 << (c % 8))))
			append_fixed(b, &rows->cols[c], &pk->cols[c], r);
	fixed_bytes = (uint16) (b->len - fixed_at - sizeof(fixed_bytes));
	memcpy(b->data + fixed_at, &fixed_bytes, sizeof(fixed_bytes));
	for (int c = 0; c < rows->ncols; c++)
		if (rows->cols[c].kind == SIFT_CELL_STR && !(pk->null_bm[c / 8] & (1 << (c % 8))))
			append_string(pk, b, &rows->cols[c], r);
}

/* The raw block of rows [from, from + n): the table of the offsets of the rows, then the rows */
static void
raw_block(Packer *pk, StringInfo b, int from, int n)
{
	uint32	   *off = palloc((n + 1) * sizeof(uint32));

	resetStringInfo(b);
	appendStringInfoSpaces(b, (n + 1) * sizeof(uint32));
	for (int i = 0; i < n; i++)
	{
		off[i] = b->len;
		append_row(pk, b, from + i);
	}
	off[n] = b->len;
	memcpy(b->data, off, (n + 1) * sizeof(uint32));
	pfree(off);
}

/* The block on disk: LZ4 of the raw bytes when it is smaller; its entry */
static SiftRowPackBlockEntry
pack_block(const StringInfoData *raw, int n, StringInfo payload)
{
	SiftRowPackBlockEntry e = {.off_qwords = payload->len / 8,.comp_len = raw->len,.raw_len = raw->len,.nrows = (uint16) n,.codec_flags = SIFT_RPACK_CODEC_RAW};
	const char *bytes = raw->data;
	char	   *comp = NULL;

	if (sift_colz_available(SIFT_CC_ALG_LZ4))
	{
		Size		cap = sift_colz_bound(SIFT_CC_ALG_LZ4, raw->len),
					clen;

		comp = palloc(cap);
		clen = sift_colz_compress(SIFT_CC_ALG_LZ4, 0, raw->data, raw->len, comp, cap);
		if (clen > 0 && clen < raw->len)
		{
			bytes = comp;
			e.comp_len = (uint32) clen;
			e.codec_flags = SIFT_RPACK_CODEC_LZ4;
		}
	}
	appendBinaryStringInfo(payload, bytes, e.comp_len);
	pad8(payload);
	if (comp != NULL)
		pfree(comp);
	return e;
}

static void
assemble(StringInfo out, const Packer *pk, const SiftRowPackBlockEntry *entries, int nblocks, const StringInfoData *payload)
{
	SiftRowPackHdr hdr = {SIFT_ROW_PACK_MAGIC, SIFT_ROW_PACK_VER, pk->has_wide ? SIFT_RPACK_FLAG_HAS_WIDE : SIFT_RPACK_FLAG_NONE, (uint16) pk->rows->ncols, SIFT_ROW_PACK_ROWS, nblocks, (uint32) pk->rows->nrows};

	appendStringInfoSpaces(out, sizeof(hdr));
	appendBinaryStringInfo(out, (const char *) pk->cols, pk->rows->ncols * sizeof(SiftRowPackCol));
	pad8(out);
	hdr.dir_off = out->len;
	appendBinaryStringInfo(out, (const char *) entries, nblocks * sizeof(SiftRowPackBlockEntry));
	pad8(out);
	hdr.data_off = out->len;
	memcpy(out->data, &hdr, sizeof(hdr));
	appendBinaryStringInfo(out, payload->data, payload->len);
}

bool
sift_row_pack_build(const SiftRows *rows, uint64 granules_len, StringInfo out)
{
	const int	nblocks = (rows->nrows + SIFT_ROW_PACK_ROWS - 1) / SIFT_ROW_PACK_ROWS,
				ncols = rows->ncols;
	Packer		pk = {.rows = rows};
	SiftRowPackBlockEntry *entries;
	StringInfoData raw,
				payload;
	uint64		sampled = 0;
	bool		built = true;

	if (ncols == 0 || ncols > UINT16_MAX)
		return false;
	pk.cols = palloc(ncols * sizeof(SiftRowPackCol));
	pk.null_bm = palloc(null_bytes(ncols));
	for (int c = 0; c < ncols; c++)
		if (!column_layout(rows, c, &pk.cols[c]))
			return false;
	/* check-memory: allow 16 bytes a block of 64 rows of one part, whose rows the writer holds */
	entries = palloc(nblocks * sizeof(SiftRowPackBlockEntry));
	initStringInfo(&raw);
	initStringInfo(&payload);
	for (int b = 0; b < nblocks && built; b++)
	{
		const int	from = b * SIFT_ROW_PACK_ROWS,
					n = Min(SIFT_ROW_PACK_ROWS, rows->nrows - from);

		raw_block(&pk, &raw, from, n);
		entries[b] = pack_block(&raw, n, &payload);
		if (b < SIFT_ROW_PACK_SAMPLE_BLOCKS)
			sampled += entries[b].comp_len;
		if (b == Min(SIFT_ROW_PACK_SAMPLE_BLOCKS, nblocks) - 1)
			built = (double) sampled * nblocks / Min(SIFT_ROW_PACK_SAMPLE_BLOCKS, nblocks) * 100 <= (double) granules_len * ROW_PACK_SHARE_PERCENT;
	}
	if (built)
		assemble(out, &pk, entries, nblocks, &payload);
	pfree(raw.data);
	pfree(payload.data);
	pfree(entries);
	pfree(pk.cols);
	pfree(pk.null_bm);
	return built;
}

/* ---- the reader ---- */

SiftRowPackBlk *
sift_row_pack_read_block(Relation rel, const SiftPart *part, uint32 blk)
{
	const SiftSecDirEntry *se = sift_part_section(part, SEC_ROW_PACK);
	const int	ncols = part->hdr.ncols;
	const uint64 head_len = sizeof(SiftRowPackHdr) + (uint64) ncols * sizeof(SiftRowPackCol);
	char	   *head = palloc(head_len);
	SiftRowPackHdr hdr;
	SiftRowPackBlockEntry e;
	SiftRowPackBlk *b;
	char	   *cols,
			   *bytes;

	if (se->len < head_len)
		SIFT_DAMAGE_DATA(rel, errmsg("sift: a row pack is shorter than its header"));
	sift_part_read_section_bytes(rel, part, se, 0, head_len, head);
	memcpy(&hdr, head, sizeof(hdr));
	if (hdr.magic != SIFT_ROW_PACK_MAGIC || hdr.version != SIFT_ROW_PACK_VER || hdr.ncols != ncols || hdr.rows_per_blk != SIFT_ROW_PACK_ROWS || blk >= hdr.nblocks ||
		hdr.dir_off + (uint64) hdr.nblocks * sizeof(e) > se->len || hdr.data_off > se->len)
		SIFT_DAMAGE_DATA(rel, errmsg("sift: a row pack does not match its part"));
	sift_part_read_section_bytes(rel, part, se, hdr.dir_off + (uint64) blk * sizeof(e), sizeof(e), &e);
	if (e.nrows == 0 || e.nrows > SIFT_ROW_PACK_ROWS || hdr.data_off + (uint64) e.off_qwords * 8 + e.comp_len > se->len || e.raw_len < (e.nrows + 1) * sizeof(uint32) ||
		(e.codec_flags & SIFT_RPACK_CODEC_MASK) > SIFT_RPACK_CODEC_LZ4 || ((e.codec_flags & SIFT_RPACK_CODEC_MASK) == SIFT_RPACK_CODEC_RAW && e.comp_len != e.raw_len))
		SIFT_DAMAGE_DATA(rel, errmsg("sift: a row pack block does not match its entry"));
	b = palloc(sizeof(SiftRowPackBlk) + (Size) ncols * sizeof(SiftRowPackCol) + e.raw_len);
	*b = (SiftRowPackBlk) {.nrows = e.nrows,.len = e.raw_len,.ncols = (uint16) ncols};
	cols = (char *) (b + 1);
	bytes = cols + (Size) ncols * sizeof(SiftRowPackCol);
	memcpy(cols, head + sizeof(hdr), (Size) ncols * sizeof(SiftRowPackCol));
	if ((e.codec_flags & SIFT_RPACK_CODEC_MASK) == SIFT_RPACK_CODEC_RAW)
		sift_part_read_section_bytes(rel, part, se, hdr.data_off + (uint64) e.off_qwords * 8, e.comp_len, bytes);
	else
	{
		char	   *comp = palloc(e.comp_len);

		sift_part_read_section_bytes(rel, part, se, hdr.data_off + (uint64) e.off_qwords * 8, e.comp_len, comp);
		if (!sift_colz_decompress(SIFT_CC_ALG_LZ4, comp, e.comp_len, bytes, e.raw_len))
			SIFT_DAMAGE_DATA(rel, errmsg("sift: a row pack block does not decompress"));
		pfree(comp);
	}
	pfree(head);
#ifdef SIFT_TESTING
	blocks_read++;
#endif
	return b;
}

Size
sift_row_pack_blk_bytes(const SiftRowPackBlk *b)
{
	return sizeof(SiftRowPackBlk) + (Size) b->ncols * sizeof(SiftRowPackCol) + b->len;
}

static inline uint64
load_uint(const char *p, int width)
{
	uint64		v = 0;

	memcpy(&v, p, width);
	return v;
}

static void
damaged(void)
{
	SIFT_DAMAGE_OTHER_DATA(errmsg("sift: a row of a row pack is damaged"));
}

/* Where the value of each column starts in the row: the fixed ones by their widths, the strings by their lengths */
static void
place_values(SiftRowPackRow *r)
{
	const int	nbm = null_bytes(r->ncols);
	uint16		fixed_bytes;
	uint32		fixed,
				strs;

	if (r->len < sizeof(uint64) + nbm + sizeof(fixed_bytes))
		damaged();
	memcpy(&fixed_bytes, r->row + sizeof(uint64) + nbm, sizeof(fixed_bytes));
	fixed = sizeof(uint64) + nbm + sizeof(fixed_bytes);
	strs = fixed + fixed_bytes;
	for (int c = 0; c < r->ncols; c++)
	{
		const SiftRowPackCol *pc = &r->cols[c];

		if (r->row[sizeof(uint64) + c / 8] & (1 << (c % 8)))
			r->off[c] = RPACK_NO_VALUE;
		else if (pc->kind == SIFT_CELL_STR)
		{
			uint16		len;

			if (strs + sizeof(len) > r->len)
				damaged();
			memcpy(&len, r->row + strs, sizeof(len));
			r->off[c] = strs;
			strs += sizeof(len) + (len == SIFT_ROW_PACK_EXT_SENTINEL ? 0 : len);
		}
		else
		{
			r->off[c] = fixed;
			fixed += pc->kind == SIFT_CELL_F8 ? sizeof(double) : pc->width;
		}
	}
	if (fixed > sizeof(uint64) + nbm + sizeof(fixed_bytes) + fixed_bytes || strs > r->len)
		damaged();
}

static const char *
row_bytes(const SiftRowPackBlk *b, uint32 row, uint32 *len)
{
	const char *bytes = (const char *) ((const SiftRowPackCol *) (b + 1) + b->ncols);
	uint32		from,
				to;

	memcpy(&from, bytes + row * sizeof(uint32), sizeof(from));
	memcpy(&to, bytes + (row + 1) * sizeof(uint32), sizeof(to));
	if (from > to || to > b->len || to - from < sizeof(uint64))
		damaged();
	*len = to - from;
	return bytes + from;
}

bool
sift_row_pack_holds(const SiftRowPackBlk *b, uint32 row, uint64 tid)
{
	uint32		len;
	uint64		stored;

	if (row >= b->nrows)
		return false;
	memcpy(&stored, row_bytes(b, row, &len), sizeof(stored));
	return stored == tid;
}

SiftRowPackRow *
sift_row_pack_row(const SiftRowPackBlk *b, uint32 row)
{
	const Size	head = MAXALIGN(sizeof(SiftRowPackRow));
	uint32		len;
	const char *bytes = row_bytes(b, row, &len);
	SiftRowPackRow *r = palloc(head + (Size) b->ncols * (sizeof(SiftRowPackCol) + sizeof(uint32)) + len);

	r->ncols = b->ncols;
	r->len = len;
	r->cols = (const SiftRowPackCol *) ((char *) r + head);
	r->off = (uint32 *) (r->cols + b->ncols);
	r->row = (const char *) (r->off + b->ncols);
	memcpy((void *) r->cols, b + 1, (Size) b->ncols * sizeof(SiftRowPackCol));
	memcpy((void *) r->row, bytes, len);
	place_values(r);
#ifdef SIFT_TESTING
	rows_read++;
#endif
	return r;
}

SiftRowPackCell
sift_row_pack_cell(const SiftRowPackRow *r, int col, SiftF2Cell *cell)
{
	const SiftRowPackCol *pc = &r->cols[col];
	const char *p = r->row + r->off[col];

	*cell = (SiftF2Cell) {0};
	if (r->off[col] == RPACK_NO_VALUE)
	{
		cell->isnull = true;
		return SIFT_RPACK_NULL;
	}
	if (pc->kind == SIFT_CELL_F8)
		memcpy(&cell->f, p, sizeof(double));
	else if (pc->kind == SIFT_CELL_INT)
		cell->i = (int64) ((uint64) pc->base + load_uint(p, pc->width));
	else
	{
		uint16		len;

		memcpy(&len, p, sizeof(len));
		if (len == SIFT_ROW_PACK_EXT_SENTINEL)
		{
#ifdef SIFT_TESTING
			external_cells++;
#endif
			return SIFT_RPACK_EXTERNAL;
		}
		cell->s = (SiftStrCell) {.p = p + sizeof(len),.len = len};
	}
	return SIFT_RPACK_VALUE;
}

#ifdef SIFT_TESTING
PG_FUNCTION_INFO_V1(sift_test_f2_row_pack_stats);
PG_FUNCTION_INFO_V1(sift_test_f2_row_pack_parts);
PG_FUNCTION_INFO_V1(sift_test_f2_row_pack_policy);

/* (percent): the parts written from now on in this backend get a row pack when it is at most percent of SEC_GRANULES (the default is SIFT_ROW_PACK_SHARE_PERCENT); the percent before */
Datum
sift_test_f2_row_pack_policy(PG_FUNCTION_ARGS)
{
	int			before = share_percent;

	share_percent = PG_GETARG_INT32(0);
	PG_RETURN_INT32(before);
}

/* (reset): the blocks of row packs read, the rows taken from them and the cells they left to the granules since the last reset, as "blocks rows external" */
Datum
sift_test_f2_row_pack_stats(PG_FUNCTION_ARGS)
{
	char	   *out = psprintf("%llu %llu %llu", (unsigned long long) blocks_read, (unsigned long long) rows_read, (unsigned long long) external_cells);

	if (PG_GETARG_BOOL(0))
		blocks_read = rows_read = external_cells = 0;
	PG_RETURN_TEXT_P(cstring_to_text(out));
}

static uint64
section_bytes(const SiftPart *part, uint16 tag)
{
	const SiftSecDirEntry *se = sift_part_section(part, tag);

	return se != NULL ? se->len : 0;
}

/* (table): a line for each part of the table, "level ext_flags nrows granules tid_hash row_pack bytes" of its sections, in the order of the manifest */
Datum
sift_test_f2_row_pack_parts(PG_FUNCTION_ARGS)
{
	Relation	rel = relation_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift row pack parts");
	const SiftPlanSnapshot *v = sift_plan_cache_get(rel, cx);
	StringInfoData out;

	initStringInfo(&out);
	for (int i = 0; i < v->npartitions; i++)
		for (int k = 0; k < v->partlists[i].nparts; k++)
		{
			const SiftPartDesc *d = &v->partlists[i].parts[k];
			const SiftPart *part = sift_part_open(rel, d->header_blk, d->part_gen, (uint32) v->partitions[i].part_id);

			appendStringInfo(&out, "%u %u %llu %llu %llu %llu\n", d->level, d->ext_flags, (unsigned long long) d->nrows, (unsigned long long) section_bytes(part, SEC_GRANULES),
							 (unsigned long long) section_bytes(part, SEC_TID_HASH), (unsigned long long) section_bytes(part, SEC_ROW_PACK));
		}
	MemoryContextDelete(cx);
	relation_close(rel, AccessShareLock);
	PG_RETURN_TEXT_P(cstring_to_text(out.data));
}
#endif
