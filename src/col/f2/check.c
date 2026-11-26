/*
 * col/f2/check.c - the key invariants of the parts of an index of format major 2: the sort cells of the rows of a part do not descend (rows that tie on them may be in any order of
 * their addresses: sift_rowbuf_cluster), the key range of a part in the manifest is exactly that of its rows (the least and the greatest), every granule's rows lie inside it and so does
 * their zone of the lead key column, and the part lies inside its partition.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/pg_class.h"
#include "catalog/pg_operator_d.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "utils/snapmgr.h"

#include "col/f2/del/delvec.h"
#include "col/f2/f2.h"
#include "col/f2/point_dir.h"
#include "col/f2/preagg/v2.h"
#include "col/f2/sink/sink.h"
#include "col/f2/store/store.h"
#include "col/f2/tid/hash.h"
#include "core/memtrack.h"
#include "col/skip/skip.h"
#include "core/page_io.h"
#include "table/f2/row/pack.h"

static void
cells_of_row(const SiftPartSpec *spec, int nint, int64 *const *ints, bool *const *nulls, int r, SiftCell *cells)
{
	for (int k = 0; k < nint; k++)
		cells[spec->sort_cols[k]] = (SiftCell) {.isnull = nulls[k][r],.i = ints[k][r]};
}

/* The quantile sketches of a part: the section is well formed and no sketch holds more values than the part has rows */
static void
check_sketches(Relation rel, const SiftPartSpec *spec, const SiftPart *part, const SiftPartDesc *d)
{
	SiftF2KllSec sec;

	if (!sift_f2_kll_load(rel, part, &sec))
		return;
	for (int i = 0; spec->kll != NULL && i < spec->kll->n; i++)
	{
		const uint8 *blob;
		uint32		len;
		uint64		n;

		if (sift_kll_sec_find(sec.data, sec.len, spec->kll->col[i], SIFT_KLL_WHOLE_PART, &blob, &len, &n) && n > d->nrows)
			SIFT_CORRUPT(rel, "Part %llu: the quantile sketch of leaf %u holds %llu values and the part has %llu rows.", (unsigned long long) d->part_gen, (unsigned) spec->kll->col[i],
						 (unsigned long long) n, (unsigned long long) d->nrows);
	}
}

/* The spec of the part: the sort order its SEC_SCHEMA records, which is the one its rows keep whatever order the index asks for now (a key_order set or a primary key added after the rows were written) */
static SiftPartSpec
part_spec(Relation rel, const SiftPartSpec *spec, const SiftPart *part, uint16 *sort_cols)
{
	SiftPartSpec own = *spec;
	uint64		len;
	char	   *schema = sift_part_read_section(rel, part, SEC_SCHEMA, &len);
	uint16		hdr[2];
	Size		off;

	if (len < sizeof(hdr))
		SIFT_CORRUPT(rel, "Part %llu: its schema is %llu bytes.", (unsigned long long) part->hdr.part_gen, (unsigned long long) len);
	memcpy(hdr, schema, sizeof(hdr));
	own.nsort = hdr[1] & 0x00FF;
	off = (4 + (Size) hdr[0] + 1) & ~(Size) 1;
	if (own.nsort > SIFT_KEY_ORDER_MAX || off + (Size) own.nsort * (sizeof(uint16) + 1) > len)
		SIFT_CORRUPT(rel, "Part %llu: its schema names %u sort columns, which it does not hold or the spec does not take.", (unsigned long long) part->hdr.part_gen, (unsigned) own.nsort);
	memcpy(sort_cols, schema + off, own.nsort * sizeof(uint16));
	memcpy(own.sort_flags, schema + off + own.nsort * sizeof(uint16), own.nsort);
	own.sort_cols = sort_cols;
	pfree(schema);
	return own;
}

/* The uint32s [from, from + n) of a section, as many as the buffer holds at a time */
#define SEC_WORDS	4096

static void
read_words(Relation rel, const SiftPart *part, const SiftSecDirEntry *sec, uint64 off, uint32 n, uint32 *dst)
{
	sift_part_read_section_bytes(rel, part, sec, off, (uint64) n * sizeof(uint32), dst);
}

/* The pre-aggregates: the header and cubes the reader validates, then for each cube first[ngranules + 1], the number of the first cell of each granule from 0 and ascending, and the cells they count */
static void
check_preagg(Relation rel, const SiftPart *part)
{
	const SiftSecDirEntry *sec = sift_part_section(part, SEC_PREAGG);
	const SiftPreaggPart *pp = sift_preagg_part_open(rel, part);
	const uint64 ng = part->hdr.ngranules;
	uint32		w[SEC_WORDS];

	for (int k = 0; k < pp->hdr.ncubes; k++)
	{
		const SiftPreaggCubeDesc *cd = &pp->cube[k];
		uint32		prev = 0;

		for (uint64 at = 0; at < ng + 1; at += SEC_WORDS)
		{
			uint32		n = Min((uint64) SEC_WORDS, ng + 1 - at);

			read_words(rel, part, sec, cd->gran_dir_off + at * sizeof(uint32), n, w);
			for (uint32 i = 0; i < n; i++)
			{
				if ((at + i == 0 && w[i] != 0) || w[i] < prev)
					SIFT_CORRUPT(rel, "Part %llu: the first cell of granule %llu in cube %d of the pre-aggregates is %u after %u.", (unsigned long long) part->hdr.part_gen, (unsigned long long) (at + i), k, w[i], prev);
				prev = w[i];
			}
		}
		if (cd->cells_off + (uint64) prev * sift_pag_cell_size(cd) > sec->len)
			SIFT_CORRUPT(rel, "Part %llu: cube %d of the pre-aggregates counts %u cells and the section holds %llu bytes.", (unsigned long long) part->hdr.part_gen, k, prev, (unsigned long long) sec->len);
	}
}

/* The point directory: for each key, the bucket ends ascend from 0 to its entries, and every entry is a row of a granule of the part */
static void
check_point(Relation rel, const SiftPart *part)
{
	const SiftPointDir *pd = sift_point_dir_open(rel, part, CurrentMemoryContext);
	uint32		w[SEC_WORDS];

	for (int k = 0; pd != NULL && k < pd->hdr.ncols; k++)
	{
		const SiftPointCol *c = &pd->cols[k];
		const uint64 nb = (UINT64CONST(1) << c->log2_buckets) + 1;
		uint32		prev = 0;

		for (uint64 at = 0; at < nb; at += SEC_WORDS)
		{
			uint32		n = Min((uint64) SEC_WORDS, nb - at);

			read_words(rel, part, &pd->sec, c->off_buckets + at * sizeof(uint32), n, w);
			for (uint32 i = 0; i < n; i++)
			{
				if ((at + i == 0 && w[i] != 0) || w[i] < prev)
					SIFT_CORRUPT(rel, "Part %llu: the buckets of key %d of its point directory do not ascend from 0.", (unsigned long long) part->hdr.part_gen, k);
				prev = w[i];
			}
		}
		if (prev != c->nentries)
			SIFT_CORRUPT(rel, "Part %llu: the buckets of key %d of its point directory end at %u and it has %u entries.", (unsigned long long) part->hdr.part_gen, k, prev, c->nentries);
		for (uint64 at = 0; at < c->nentries; at += SEC_WORDS)
		{
			uint32		n = Min((uint64) SEC_WORDS, c->nentries - at);

			read_words(rel, part, &pd->sec, c->off_entries + at * sizeof(uint32), n, w);
			for (uint32 i = 0; i < n; i++)
				if ((w[i] >> SIFT_POINT_ROW_BITS) >= part->hdr.ngranules)
					SIFT_CORRUPT(rel, "Part %llu: an entry of key %d of its point directory names granule %u of %u.", (unsigned long long) part->hdr.part_gen, k, w[i] >> SIFT_POINT_ROW_BITS,
								 part->hdr.ngranules);
		}
	}
}

static void
check_sections(Relation rel, const SiftPartDesc *d, const SiftPart *part, uint32 part_id, const uint16 *granule_rows)
{
	if (sift_part_section(part, SEC_PREAGG) != NULL)
		check_preagg(rel, part);
	if (sift_part_section(part, SEC_POINT) != NULL)
		check_point(rel, part);
	if (sift_part_section(part, SEC_SKIP_INDEX) != NULL)
		(void) sift_skip_open(rel, part);
	if (d->dv_blk != InvalidBlockNumber)
		(void) sift_dv_read(rel, d, part_id, granule_rows);
	if (sift_part_section(part, SEC_TID_HASH) != NULL)
		sift_tid_hash_check(rel, part, part->hdr.nrows);
	if (sift_part_section(part, SEC_ROW_PACK) != NULL)
		sift_row_pack_check(rel, part, part->hdr.nrows);
}

static void
check_part(Relation rel, const SiftPartSpec *current, const SiftPartitionEntry *e, const SiftPartDesc *d, uint64 *granules, uint64 *rows)
{
	SiftPart   *part = sift_part_open_desc(rel, d, (uint32) e->part_id);
	uint16		sort_cols[SIFT_KEY_ORDER_MAX];
	SiftPartSpec own = part_spec(rel, current, part, sort_cols);
	const SiftPartSpec *spec = &own;
	SiftCell   *cells = palloc0(Max(spec->ncols, 1) * sizeof(SiftCell));
	MemoryContext granule_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 check granule");
	int			nint = sift_spec_int_keys(spec);
	uint8		prev[SIFT_BOUND_BYTES],
				least[SIFT_BOUND_BYTES],
				greatest[SIFT_BOUND_BYTES] = {0},
				key[SIFT_BOUND_BYTES] = {0},
				prefix[SIFT_BOUND_BYTES];
	bool		have_prev = false;
	/* check-memory: allow one count for each granule of the part */
	uint16	   *granule_rows = palloc(Max(part->hdr.ngranules, 1) * sizeof(uint16));

	memset(least, 0xFF, sizeof(least));
	const SiftSecDirEntry *zs = sift_part_section(part, SEC_ZONES);

	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		MemoryContext old = MemoryContextSwitchTo(granule_cx);
		bool	   *want = palloc0(Max(spec->ncols, 1));
		SiftGranuleDesc gd;
		char	   *rec;
		SiftGranuleView v;
		uint64	   *tids;
		int64	   *ints[SIFT_KEY_ORDER_MAX];
		bool	   *nulls[SIFT_KEY_ORDER_MAX];
		int64		zmin = PG_INT64_MAX,
					zmax = PG_INT64_MIN;

		CHECK_FOR_INTERRUPTS();
		for (int k = 0; k < nint; k++)
			want[spec->sort_cols[k]] = true;
		rec = sift_part_read_granule_cols(rel, part, g, want, &gd);
		v = sift_granule_open_part(rel, part, rec, gd.data_len, &gd);
		/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
		tids = palloc(gd.nrows * sizeof(uint64));
		sift_granule_tids(&v, tids);
		for (int k = 0; k < nint; k++)
		{
			/* check-memory: allow the rows of one granule, at most SIFT_GRANULE_ROWS */
			ints[k] = palloc(gd.nrows * sizeof(int64));
			nulls[k] = palloc(gd.nrows);
			sift_granule_ints(&v, spec->sort_cols[k], ints[k], nulls[k]);
		}
		for (int r = 0; r < gd.nrows; r++)
		{
			cells_of_row(spec, nint, ints, nulls, r, cells);
			sift_row_key(spec, cells, tids[r], key);
			sift_row_key_prefix(spec, cells, prefix);
			if (have_prev && memcmp(prev, prefix, SIFT_BOUND_BYTES) > 0)
				SIFT_CORRUPT(rel, "Part %llu: the keys of its rows do not ascend at granule %u row %d.", (unsigned long long) d->part_gen, g, r);
			if (memcmp(key, least, SIFT_BOUND_BYTES) < 0)
				memcpy(least, key, SIFT_BOUND_BYTES);
			if (memcmp(key, greatest, SIFT_BOUND_BYTES) > 0)
				memcpy(greatest, key, SIFT_BOUND_BYTES);
			memcpy(prev, prefix, SIFT_BOUND_BYTES);
			have_prev = true;
			if (nint > 0 && !nulls[0][r])
			{
				zmin = Min(zmin, ints[0][r]);
				zmax = Max(zmax, ints[0][r]);
			}
			if (memcmp(key, d->key_lo, SIFT_BOUND_BYTES) < 0 || memcmp(key, d->key_hi, SIFT_BOUND_BYTES) > 0 || memcmp(key, e->key_lo, SIFT_BOUND_BYTES) < 0 ||
				(memcmp(e->key_hi, (uint8[SIFT_BOUND_BYTES]) {0}, SIFT_BOUND_BYTES) != 0 && memcmp(key, e->key_hi, SIFT_BOUND_BYTES) >= 0))
				SIFT_CORRUPT(rel, "Part %llu: a row of granule %u lies outside the key range of its part or of its partition.", (unsigned long long) d->part_gen, g);
		}
		if (nint > 0 && zs != NULL && zs->len == (uint64) part->hdr.ncols * part->hdr.ngranules * sizeof(SiftZone))
		{
			SiftZone	z;

			sift_part_read_section_bytes(rel, part, zs, ((uint64) spec->sort_cols[0] * part->hdr.ngranules + g) * sizeof(z), sizeof(z), &z);
			if (zmin <= zmax ? (z.flags & SIFT_ZONE_NONE) || z.min != zmin || z.max != zmax : !(z.flags & SIFT_ZONE_NONE))
				SIFT_CORRUPT(rel, "Part %llu: the zone of the lead key column of granule %u is not the range of its rows.", (unsigned long long) d->part_gen, g);
		}
		granule_rows[g] = gd.nrows;
		*rows += gd.nrows;
		(*granules)++;
		MemoryContextSwitchTo(old);
		MemoryContextReset(granule_cx);
	}
	MemoryContextDelete(granule_cx);
	if (have_prev && (memcmp(least, d->key_lo, SIFT_BOUND_BYTES) != 0 || memcmp(greatest, d->key_hi, SIFT_BOUND_BYTES) != 0))
		SIFT_CORRUPT(rel, "Part %llu: its key range in the manifest is not that of its least and greatest row.", (unsigned long long) d->part_gen);
	check_sketches(rel, spec, part, d);
	check_sections(rel, d, part, (uint32) e->part_id, granule_rows);
}

char *
sift_f2_check_keys(Relation rel)
{
	const SiftPartSpec *spec = rel->rd_rel->relkind == RELKIND_INDEX ? &sift_f2_rel(rel)->spec : &sift_f2_store_open(rel)->spec;
	SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
	MemoryContext part_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 check part");
	uint64		parts = 0,
				granules = 0,
				rows = 0;

	for (int i = 0; i < mr.n; i++)
	{
		int			np;
		SiftPartDesc *pd = sift_manifest_read_parts(rel, &mr.entries[i], &np);

		for (int p = 0; p < np; p++)
		{
			MemoryContext old = MemoryContextSwitchTo(part_cx);

			check_part(rel, spec, &mr.entries[i], &pd[p], &granules, &rows);
			MemoryContextSwitchTo(old);
			MemoryContextReset(part_cx);
		}
		parts += np;
		pfree(pd);
	}
	MemoryContextDelete(part_cx);
	return psprintf("parts=%llu granules=%llu rows=%llu exact", (unsigned long long) parts, (unsigned long long) granules, (unsigned long long) rows);
}

/* A pass costs a scan of the whole index, so a window never holds less than the heap blocks that a megabyte of bits covers */
#define HEAP_WINDOW_MIN_BYTES	(1024 * 1024)

/* The rows of a window of heap blocks the index holds, one bit for each address the window can have: what a pass of the check keeps, however large the table is */
typedef struct HeapWindow
{
	uint8	   *seen;
	BlockNumber from;
	uint32		nblocks;
	BlockNumber max_index_block;
	Relation	index;
} HeapWindow;

static bool
window_bit(const HeapWindow *w, const ItemPointerData *tid, uint32 *bit)
{
	BlockNumber blk = ItemPointerGetBlockNumber(tid);
	OffsetNumber off = ItemPointerGetOffsetNumber(tid);

	if (blk < w->from || blk - w->from >= w->nblocks || off < FirstOffsetNumber || off > SIFT_MAX_OFFSET)
		return false;
	*bit = (blk - w->from) * SIFT_MAX_OFFSET + off - FirstOffsetNumber;
	return true;
}

static void
heap_tid_cb(Relation index, ItemPointer tid, Datum *values, bool *isnull, bool alive, void *state)
{
	const HeapWindow *w = state;
	uint32		bit;

	if (alive && !(window_bit(w, tid, &bit) && (w->seen[bit / 8] & (1 << (bit % 8)))))
		SIFT_CORRUPT(w->index, "The heap tuple (%u,%u) is visible to the snapshot and is not in the index.", ItemPointerGetBlockNumber(tid), (unsigned) ItemPointerGetOffsetNumber(tid));
}

/* The heap tuples of the blocks [from, from + span) that the snapshot of the index scan sees and the window does not hold are not in the index */
static void
heap_window_check(Relation heap, Relation index, Snapshot snap, HeapWindow *w, BlockNumber span)
{
	IndexInfo  *info = BuildIndexInfo(index);
	ItemPointerData lo,
				hi;

	info->ii_Concurrent = true;
	ItemPointerSet(&lo, w->from, FirstOffsetNumber);
	ItemPointerSet(&hi, span == InvalidBlockNumber ? MaxBlockNumber : w->from + span - 1, MaxOffsetNumber);
	table_index_build_range_scan(heap, index, info, true, false, false, 0, InvalidBlockNumber, heap_tid_cb, w, table_beginscan_tidrange(heap, snap, &lo, &hi));
}

static void
mark_live_rows(void *arg, const SiftF2Batch *b)
{
	HeapWindow *w = arg;

	for (int r = 0; r < b->n; r++)
		if ((b->live[r >> 6] >> (r & 63)) & 1)
		{
			ItemPointerData tid;
			uint32		bit;

			sift_f2_tid_set(&tid, b->tid[r]);
			w->max_index_block = Max(w->max_index_block, ItemPointerGetBlockNumber(&tid));
			if (window_bit(w, &tid, &bit))
				w->seen[bit / 8] |= 1 << (bit % 8);
		}
}

static void
index_window_fill(Relation heap, Relation index, Snapshot snap, HeapWindow *w)
{
	IndexScanDesc scan = sift_index_beginscan(heap, index, snap, 0);

	index_rescan(scan, NULL, 0, NULL, 0);
	sift_f2_scan_sink(scan, mark_live_rows, w, NULL, 0);
	index_endscan(scan);
}

/* Every row the heap shows to a snapshot is a live row of the index: the index may hold more, the versions that UPDATE, DELETE and aborted INSERTs left until its next VACUUM */
void
sift_f2_check_heap(Relation index)
{
	Relation	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	Snapshot	snap = RegisterSnapshot(GetTransactionSnapshot());
	Size		bytes = Max(sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, 1, 2), HEAP_WINDOW_MIN_BYTES);
	/* check-memory: allow one window of the maintenance budget */
	HeapWindow	w = {.seen = palloc(bytes),.nblocks = (Size) bytes * 8 / SIFT_MAX_OFFSET,.index = index};

	do
	{
		memset(w.seen, 0, bytes);
		index_window_fill(heap, index, snap, &w);
		heap_window_check(heap, index, snap, &w, w.nblocks);
		w.from += w.nblocks;
	} while (w.from <= w.max_index_block);
	w.nblocks = 0;
	heap_window_check(heap, index, snap, &w, InvalidBlockNumber);
	pfree(w.seen);
	UnregisterSnapshot(snap);
	table_close(heap, AccessShareLock);
}
