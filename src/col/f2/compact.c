/*
 * col/compact.c - partition-local compaction (col/compact.h).
 */
#include "core/host.h"

#include "access/transam.h"
#include "commands/vacuum.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"

#include "col/f2/compact.h"
#include "col/f2/f2_part_idx.h"
#include "core/memtrack.h"
#include "col/f2/delete.h"
#include "col/f2/delvec_delta.h"
#include "col/f2/f2_dv_join.h"
#include "col/f2/f2_kll.h"
#include "col/f2/f2_store.h"
#include "core/compat.h"
#include "core/crash_point.h"
#include "core/f2/col_delta_anchor.h"
#include "core/f2/f2_gate.h"
#include "core/page_io.h"
#include "core/wal_conflict.h"
#include "core/xid_cache.h"

const SiftCompactFold *sift_compact_fold = NULL;

/* The rows of the inputs are written as a part when the compaction holds this share of its budget (a half), and the merged dictionaries of the string columns take at most this share of it (an eighth) */
#define SIFT_COMPACT_FLUSH_SHARE	2
#define SIFT_COMPACT_DICT_SHARE		8

/* What the horizon lets a row's inserter be: kept as it is, frozen (it committed before the horizon) or dropped (it aborted) */
typedef enum Inserter
{
	INS_KEEP,
	INS_FREEZE,
	INS_DROP
} Inserter;

static Inserter
inserter_of(uint32 xmin, SiftXidHorizonCache *hc, FullTransactionId *stripped)
{
	if (!TransactionIdIsNormal(xmin))
		return INS_FREEZE;
	if (!sift_xid_horizon_older(hc, xmin))
		return INS_KEEP;
	switch (sift_xid_horizon_status(hc, xmin))
	{
		case SIFT_H_COMMITTED_OLD:
			*stripped = sift_fxid_max(*stripped, sift_xid_fxid_at(hc->next, xmin));
			return INS_FREEZE;
		case SIFT_H_ABORTED:
			return INS_DROP;
		default:
			return INS_KEEP;
	}
}

/* The blocks of a part, added to those a commit releases: its deletion vector is not among them, the commit releases the one the manifest holds when it is made */
static void
release_part(const SiftPart *part, SiftExtent **rel_ext, int *nrel)
{
	int			nb;
	SiftExtent *blocks = sift_part_blocks(part, &nb);

	*rel_ext = repalloc(*rel_ext, sizeof(SiftExtent) * pg_nextpower2_32(Max(16, *nrel + nb + 1)));
	memcpy(*rel_ext + *nrel, blocks, nb * sizeof(SiftExtent));
	*nrel += nb;
}

/* An input is opened in a context of its own, which the caller deletes: a compaction holds the header, the extents and the dictionaries of the input it reads, not of all of them */
SiftPart *
sift_compact_open_input(Relation rel, const SiftPartDesc *d, uint32 part_id, MemoryContext *cx)
{
	MemoryContext old;
	SiftPart   *part;

	*cx = sift_mem_allocset_create(CurrentMemoryContext, "sift compact input");
	old = MemoryContextSwitchTo(*cx);
	part = sift_part_open(rel, d->header_blk, d->part_gen, part_id);
	MemoryContextSwitchTo(old);
	part->io_mode = SIFT_IOS_MAINT_SEQ;
	return part;
}

/* Which of the two buffers takes a row: 1, the cold one, when its first time column is older than the cut */
static int
row_side(const SiftPartSpec *spec, const SiftRowBuf *rb, int r, int64 hot_cut)
{
	int			c = spec->time_cols[0] - 1;

	return c >= 0 && hot_cut != PG_INT64_MIN && !rb->isnull[c][r] && rb->i[c][r] < hot_cut;
}

/* The dead rows of a deletion vector as a bit for each row of the granules that have any (none for the others): what a compaction asks of a vector, in a twelfth of its bytes */
typedef uint64 **DeadBits;

static inline bool
bits_dead(const DeadBits bits, uint32 g, uint16 row)
{
	return bits != NULL && bits[g] != NULL && ((bits[g][row >> 6] >> (row & 63)) & 1) != 0;
}

/* settle: the rows whose deleter committed before the horizon are dead plain (raising *stripped), those whose deleter aborted are alive, and a deleter that is not settled stops it (*unsettled) */
static DeadBits
bits_of(const SiftDv *dv, uint32 ngranules, const uint16 *grows, bool settle, SiftXidHorizonCache *hc, FullTransactionId *stripped, bool *unsettled)
{
	DeadBits	bits = palloc0(Max(ngranules, 1) * sizeof(uint64 *));

	for (int i = 0; i < dv->ndirty; i++)
	{
		const SiftDvGran *dg = &dv->gran[i];

		for (int r = 0; r < dg->ndead; r++)
		{
			TransactionId x = dg->xmax[r].xmax;

			if (settle && TransactionIdIsValid(x))
			{
				if (!sift_xid_horizon_older(hc, x))
				{
					*unsettled = true;
					return bits;
				}
				switch (sift_xid_horizon_status(hc, x))
				{
					case SIFT_H_COMMITTED_OLD:
						*stripped = sift_fxid_max(*stripped, sift_xid_fxid_at(hc->next, x));
						break;
					case SIFT_H_ABORTED:
						continue;
					default:
						*unsettled = true;
						return bits;
				}
			}
			if (bits[dg->granule_id] == NULL)	/* check-memory: allow one bit for each row of one granule, at most 8192 rows */
				bits[dg->granule_id] = palloc0(((grows[dg->granule_id] + 63) / 64) * sizeof(uint64));
			bits[dg->granule_id][dg->rows[r] >> 6] |= UINT64CONST(1) << (dg->rows[r] & 63);
		}
	}
	return bits;
}

/*
 * Where the rows of the inputs go: two buffers (the hot and the cold side of the cut; one without a cut) written as a part of the level above each time the compaction holds flush_bytes
 * (a granule of the inputs is the least that is read, so a part holds at least one), whatever the size of the inputs. The parts are all outputs of the one commit of the manifest.
 */
typedef struct Sink
{
	Relation	rel;
	uint32		part_id;
	uint16		level;
	const SiftPartSpec *spec;
	const SiftDictMerge *dm;
	const SiftPartDicts *adopt;
	SiftMemTracker *mem;
	Size		flush_bytes;
	bool		split;
	SiftRowBuf *rb[2];
	SiftPartDesc *out;
	int			nout;
	int			cap;
	SiftCompactResult *res;
	MemoryContext cx;
} Sink;

#ifdef SIFT_TESTING
int			sift_test_compact_delete = 0;
#endif

static SiftPartDesc write_side(Relation rel, uint32 part_id, uint16 level, SiftRowBuf *rb, const SiftPartSpec *spec, const SiftDictMerge *dm, uint32 tier, const SiftPartDicts *adopt,
							   SiftCompactResult *res);

/* whole: the buffer is written because the compaction holds its share of the budget, not because the rows ended; at the last level such a part is as large as the compaction can make it, and is not merged again by count (SIFT_PART_FLAG_BUILT) */
static void
sink_flush(Sink *s, int side, bool whole)
{
	MemoryContext scratch = sift_mem_allocset_create(s->cx, "sift compact output");
	MemoryContext old = MemoryContextSwitchTo(scratch);
	uint32		flags = (!s->split ? 0 : side ? SIFT_PART_FLAG_TIME_COLD : SIFT_PART_FLAG_TIME_HOT) | (whole && s->level + 1 >= SIFT_MAX_PART_LEVEL ? SIFT_PART_FLAG_BUILT : 0);
	SiftPartDesc d = write_side(s->rel, s->part_id, s->level, s->rb[side], s->spec, s->dm, flags, s->adopt, s->res);
	Size		freed = s->rb[side]->mem_bytes;

	MemoryContextSwitchTo(old);
	MemoryContextDelete(scratch);
	s->res->rows_out += s->rb[side]->n;
	if (s->nout == s->cap)
	{
		s->cap = Max(8, s->cap * 2);
		s->out = s->out != NULL ? repalloc(s->out, s->cap * sizeof(SiftPartDesc)) : MemoryContextAlloc(s->cx, s->cap * sizeof(SiftPartDesc));
	}
	s->out[s->nout++] = d;
	sift_rowbuf_free(s->rb[side]);
	old = MemoryContextSwitchTo(s->cx);
	s->rb[side] = sift_rowbuf_new(s->spec);
	MemoryContextSwitchTo(old);
	sift_mem_release_os(freed);
}

/* The side that holds more is written while the compaction holds flush_bytes or more: what the rows of the inputs are in memory is never more than that and what a granule adds */
static void
sink_flush_due(Sink *s)
{
	while (sift_mem_used(s->mem) >= s->flush_bytes)
	{
		int			side = s->split && s->rb[1]->mem_bytes > s->rb[0]->mem_bytes ? 1 : 0;

		if (s->rb[side]->n == 0)
			return;
		sink_flush(s, side, true);
	}
}

/* What the horizon and the deletion vector make of the rows of a granule read: the rows that are dead or whose inserter aborted go, the others are kept as the part will hold them (frozen when their inserter committed before the horizon) */
typedef struct GranuleRows
{
	DeadBits	dead;
	uint32		granule;
	SiftXidHorizonCache *hc;
	SiftCompactResult *res;
} GranuleRows;

static bool
row_goes(SiftRowBuf *one, int r, void *arg)
{
	GranuleRows *gr = arg;
	Inserter	ins = inserter_of(one->xmin[r], gr->hc, &gr->res->stripped);

	gr->res->rows_in++;
	if (bits_dead(gr->dead, gr->granule, (uint16) r))
	{
		gr->res->dead_dropped++;
		return true;
	}
	if (ins == INS_DROP)
	{
		gr->res->aborted_dropped++;
		return true;
	}
	if (ins == INS_FREEZE)
	{
		gr->res->frozen += TransactionIdIsNormal(one->xmin[r]);
		one->xmin[r] = FrozenTransactionId;
		one->cmin[r] = 0;
	}
	one->subxid[r] = 0;
	return false;
}

/* The rows of a granule go to the sink: an empty buffer of the one side takes the granule's buffer as it is, the others copy its rows */
static void
sink_take(Sink *sink, SiftRowBuf *one, int64 hot_cut)
{
	if (!sink->split && sink->rb[0]->n == 0)
	{
		sift_rowbuf_free(sink->rb[0]);
		sink->rb[0] = one;
		return;
	}
	for (int r = 0; r < one->n; r++)
		sift_rowbuf_copy_row(sink->rb[row_side(sink->spec, one, r, hot_cut)], one, r, one->xmin[r], one->cmin[r], 0);
	sift_rowbuf_free(one);
}

static bool
add_part_rows(Relation rel, SiftPart *part, const SiftPartDesc *d, uint32 part_id, Sink *sink, int64 hot_cut, const SiftPartSpec *spec, const SiftCodeMap *code_map,
			  const SiftColRemap *remap, SiftXidHorizonCache *hc, void *fold, SiftCompactResult *res, SiftExtent **rel_ext, int *nrel)
{
	SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);
	uint16	   *grows = palloc(Max(part->hdr.ngranules, 1) * sizeof(uint16));
	DeadBits	dead = NULL;
	MemoryContext granule_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift compact granule");

	for (uint32 g = 0; g < part->hdr.ngranules; g++)
		grows[g] = dir[g].nrows;
	if (d->dv_blk != InvalidBlockNumber)
	{
		MemoryContext vector_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift compact vector");
		MemoryContext keep = MemoryContextSwitchTo(vector_cx);
		SiftDv	   *dv = sift_dv_read(rel, d, part_id, grows);
		bool		unsettled = false;

		(void) sift_mem_used(sink->mem);
		MemoryContextSwitchTo(keep);
		dead = bits_of(dv, part->hdr.ngranules, grows, true, hc, &res->stripped, &unsettled);
		MemoryContextDelete(vector_cx);
		if (unsettled)
		{
			MemoryContextDelete(granule_cx);
			return false;
		}
	}
	for (uint32 g = 0; g < part->hdr.ngranules; g++)
	{
		MemoryContext old = MemoryContextSwitchTo(granule_cx);
		SiftGranuleDesc gd;
		GranuleRows gr = {.dead = dead,.granule = g,.hc = hc,.res = res};

		sift_vacuum_delay_point();
		char	   *rec = sift_part_read_granule(rel, part, g, &gd);
		SiftGranuleView v = sift_granule_open_part(rel, part, rec, gd.data_len, &gd);
		SiftRowVer *vers = sift_part_read_vers(rel, part, dir, g);
		SiftRowBuf *one;

		MemoryContextSwitchTo(sink->cx);
		one = sift_rowbuf_new(spec);
		MemoryContextSwitchTo(granule_cx);
		sift_rowbuf_add_granule_mapped(one, &v, vers, spec, code_map, remap);
		if (fold != NULL)
			sift_compact_fold->apply(fold, d->part_gen, g, one, rel_ext, nrel);
		(void) sift_rowbuf_drop_if(one, row_goes, &gr);
		sink_take(sink, one, hot_cut);
		(void) sift_mem_used(sink->mem);
		MemoryContextSwitchTo(old);
		MemoryContextReset(granule_cx);
		sink_flush_due(sink);
	}
	MemoryContextDelete(granule_cx);
	release_part(part, rel_ext, nrel);
	return true;
}

static int
by_key(const void *a, const void *b)
{
	return memcmp((*(const SiftPartDesc *const *) a)->key_lo, (*(const SiftPartDesc *const *) b)->key_lo, SIFT_BOUND_BYTES);
}

/*
 * The parts as one part of the level above, their granules copied as they are: possible when no row is dead, the keys of the parts ascend from one to the next, and the parts
 * share a schema, their dictionaries and have all rows frozen (so that the horizon has nothing to drop or freeze). false: the rows have to be read.
 */
/* The side of the cut every row of the part is on: SIFT_PART_FLAG_TIME_HOT or _COLD; 0 when it straddles the cut, has no bounds or its bounds do not hold (a chain patches the time column). Without a time column or a cut every part is on the side 0 */
static uint32
part_side(const SiftPartDesc *p, int64 hot_cut, bool bounds_hold)
{
	if (!bounds_hold || !(p->flags & SIFT_PART_FLAG_HAS_TIME0))
		return 0;
	return p->time_min[0] >= hot_cut ? SIFT_PART_FLAG_TIME_HOT : p->time_max[0] < hot_cut ? SIFT_PART_FLAG_TIME_COLD : 0;
}

/* The vectors of the parts joined, their granule numbers shifted, written for the new part; the deleters older than the horizon are made plain (res->stripped) */
static void
carry_deletes(Relation rel, uint64 part_id, SiftPart *const *src, const SiftPartDesc *const *at, int n, FullTransactionId horizon, SiftCompactResult *res, SiftPartDesc *out)
{
	SiftDvJoinIn *in = palloc(n * sizeof(SiftDvJoinIn));
	FullTransactionId stripped = InvalidFullTransactionId;
	SiftDv	   *dv;

	for (int i = 0; i < n; i++)
	{
		SiftGranuleDesc *dir = sift_part_granule_dir(rel, src[i]);
		uint16	   *grows = palloc(Max(src[i]->hdr.ngranules, 1) * sizeof(uint16));

		for (uint32 g = 0; g < src[i]->hdr.ngranules; g++)
			grows[g] = dir[g].nrows;
		in[i] = (SiftDvJoinIn) {.dv = at[i]->dv_blk != InvalidBlockNumber ? sift_dv_read(rel, at[i], (uint32) part_id, grows) : NULL,.ngranules = src[i]->hdr.ngranules};
	}
	dv = sift_dv_concat_shifted(in, n, out->part_gen, sift_manifest_new_gen(rel, 1), horizon, &stripped);
	if (dv != NULL)
		(void) sift_dv_write(rel, dv, (uint32) part_id, out);
	res->stripped = sift_fxid_max(res->stripped, stripped);
}

static bool
join_parts(Relation rel, uint64 part_id, const SiftPartDesc **at, int n, const SiftPartSpec *spec, int64 hot_cut, bool bounds_hold, FullTransactionId horizon, SiftExtent **rel_ext, int *nrel,
		   SiftCompactResult *res, SiftPartDesc *out)
{
	bool		split = spec->time_cols[0] > 0 && hot_cut != PG_INT64_MIN;
	uint32		side = split ? part_side(at[0], hot_cut, bounds_hold) : 0;

	SiftPart  **src = palloc(n * sizeof(SiftPart *));
	SiftPartDesc *sorted = palloc(n * sizeof(SiftPartDesc));
	SiftF2KllBuild *kll = NULL;
	uint16		level = 0;
	uint64		ngranules,
				writer_bytes,
			   *nrows = palloc(n * sizeof(uint64)),
			   *ndead = palloc(n * sizeof(uint64));

	if (split && side == 0)
		return false;
	qsort(at, n, sizeof(SiftPartDesc *), by_key);
	for (int i = 0; i < n; i++)
	{
		uint32		tier = at[i]->flags & (SIFT_PART_FLAG_TIME_HOT | SIFT_PART_FLAG_TIME_COLD);

		if ((i > 0 && memcmp(at[i - 1]->key_hi, at[i]->key_lo, SIFT_BOUND_BYTES) >= 0) ||
			(split && (part_side(at[i], hot_cut, bounds_hold) != side || (tier != 0 && tier != side))) || (!split && tier != (at[0]->flags & (SIFT_PART_FLAG_TIME_HOT | SIFT_PART_FLAG_TIME_COLD))))
			return false;
		sorted[i] = *at[i];
		sorted[i].flags = (sorted[i].flags & ~(SIFT_PART_FLAG_TIME_HOT | SIFT_PART_FLAG_TIME_COLD)) | side;
		src[i] = sift_part_open(rel, at[i]->header_blk, at[i]->part_gen, (uint32) part_id);
		src[i]->io_mode = SIFT_IOS_MAINT_SEQ;
		level = Max(level, at[i]->level);
		nrows[i] = at[i]->nrows;
		ndead[i] = at[i]->ndead;
		sorted[i].dv_blk = InvalidBlockNumber;
	}
	if (!sift_parts_joinable(rel, src, n) || (spec->preagg && sift_part_section(src[0], SEC_PREAGG) == NULL) || !sift_dv_join_sparse(nrows, ndead, n, sift_dead_rewrite_pct))
		return false;
	res->new_gen = sift_manifest_new_gen(rel, 1);
	if (spec->kll != NULL && sift_part_section(src[0], SEC_KLL) != NULL && sift_dv_join_keeps_sketch(nrows, ndead, n))
	{
		kll = sift_f2_kll_begin(CurrentMemoryContext, spec->kll, res->new_gen);
		for (int i = 0; i < n; i++)
			if (!sift_f2_kll_merge_part(kll, rel, src[i], at[i]))
			{
				sift_f2_kll_free(kll);
				return false;
			}
	}
	*out = sift_part_write_joined(rel, (uint32) part_id, res->new_gen, Min(level + 1, SIFT_MAX_PART_LEVEL), src, sorted, n, kll, &ngranules, &writer_bytes);
	for (int i = 0; i < n; i++)
	{
		res->rows_in += at[i]->nrows;
		release_part(src[i], rel_ext, nrel);
	}
	carry_deletes(rel, part_id, src, at, n, horizon, res, out);
	res->rows_out = res->rows_in;
	res->granules_copied = res->granules_written = ngranules;
	res->writer_bytes = writer_bytes;
	res->bytes_out = out->nbytes;
	return true;
}

/* The granules of a part that was just written: its header is read in a scratch context, so no SiftPart stays behind in the caller's */
static uint32
part_granules(Relation rel, const SiftPartDesc *d, uint32 part_id)
{
	MemoryContext scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift compact written part");
	MemoryContext old = MemoryContextSwitchTo(scratch);
	uint32		n = sift_part_open(rel, d->header_blk, d->part_gen, part_id)->hdr.ngranules;

	MemoryContextSwitchTo(old);
	MemoryContextDelete(scratch);
	return n;
}

/* MEM[MEM-COL-10]: f2 compact write_side scratch (avoid leaking SiftPart in caller ctx) */
/* The rows of one side of the cut written as a part of the level above: sorted, their merged dictionaries put back, the tier flagged */
static SiftPartDesc
write_side(Relation rel, uint32 part_id, uint16 level, SiftRowBuf *rb, const SiftPartSpec *spec, const SiftDictMerge *dm, uint32 tier, const SiftPartDicts *adopt, SiftCompactResult *res)
{
	SiftColIn  *cols = palloc(Max(spec->ncols, 1) * sizeof(SiftColIn));
	SiftRows	r;
	SiftPartDesc d;
	uint64		gen = sift_manifest_new_gen(rel, 1);

	SiftCompressTier out_tier = level + 1 >= SIFT_MAX_PART_LEVEL ? SIFT_TIER_COLD_L2 : SIFT_TIER_WARM_L1;

	sift_rowbuf_sort(rb, spec);
	sift_rowbuf_cluster(rb, spec, out_tier);
	r = sift_rowbuf_rows(rb, cols, spec);
	sift_dict_merge_finish(dm, rb, cols, res);
	r.part_flags = tier;
	r.adopt_dicts = adopt;
	r.bulk_tail = true;
	d = sift_part_write_rows(rel, part_id, gen, Min(level + 1, SIFT_MAX_PART_LEVEL), out_tier, &r);
	res->new_gen = res->new_gen == 0 ? gen : res->new_gen;
	res->granules_written += part_granules(rel, &d, part_id);
	res->bytes_out += d.nbytes;
	return d;
}


/* What a compaction keeps of a part it reads, which is opened for the one step that needs it: its descriptor, the rows of its granules, its deletion vector as it was then and how its columns map to the current ones */
typedef struct CompactIn
{
	SiftPartDesc d;
	uint32		ngranules;
	uint16	   *grows;
	DeadBits	dead;			/* the rows its vector held dead at the read */
	uint32		dv_len;			/* the bytes of the vector at the read: a delta tail changes them and not dv_gen */
	const SiftColRemap *remap;
} CompactIn;

static CompactIn *
read_inputs(Relation rel, uint64 part_id, const SiftPartDesc *parts, int nparts, const uint64 *gens, int ngens)
{
	CompactIn  *in = palloc0(Max(ngens, 1) * sizeof(CompactIn));
	const SiftF2Store *store = sift_f2_store_open(rel);
	SiftPartGenIndex by_gen;

	sift_part_gen_idx_init(&by_gen, parts, nparts);
	for (int i = 0; i < ngens; i++)
	{
		const SiftPartDesc *found = sift_part_gen_idx_find(&by_gen, gens[i]);
		MemoryContext icx;
		SiftPart   *part;
		SiftGranuleDesc *dir;

		if (found == NULL)
			ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu has no part of generation %llu", (unsigned long long) part_id, (unsigned long long) gens[i])));
		part = sift_compact_open_input(rel, found, (uint32) part_id, &icx);
		dir = sift_part_granule_dir(rel, part);
		in[i].d = *found;
		in[i].ngranules = part->hdr.ngranules;
		in[i].grows = palloc(Max(in[i].ngranules, 1) * sizeof(uint16));
		for (uint32 g = 0; g < in[i].ngranules; g++)
			in[i].grows[g] = dir[g].nrows;
		pfree(dir);
		in[i].dv_len = found->dv_blk != InvalidBlockNumber ? sift_dv_stream_len(rel, found->dv_blk) : 0;
		if (store->host->build_col_remap != NULL)
			in[i].remap = store->host->build_col_remap(store, rel, part, CurrentMemoryContext);
		MemoryContextDelete(icx);
		if (found->dv_blk != InvalidBlockNumber)
		{
			MemoryContext vector_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift compact vector");
			MemoryContext keep = MemoryContextSwitchTo(vector_cx);
			SiftDv	   *dv = sift_dv_read(rel, found, (uint32) part_id, in[i].grows);

			MemoryContextSwitchTo(keep);
			in[i].dead = bits_of(dv, in[i].ngranules, in[i].grows, false, NULL, NULL, NULL);
			MemoryContextDelete(vector_cx);
		}
	}
	sift_part_gen_idx_free(&by_gen);
	return in;
}

#ifdef SIFT_TESTING
/* The first rows of the first input, deleted as settled deletes through the bulk path */
static void
test_delete_meanwhile(Relation rel, uint64 part_id, const uint8 *key_lo, const CompactIn *in, const SiftPartSpec *spec)
{
	MemoryContext icx;
	SiftPart   *part = sift_compact_open_input(rel, &in->d, (uint32) part_id, &icx);
	SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);
	uint64	   *tids = sift_part_read_tids(rel, part, dir, 0);
	int			n = Min(sift_test_compact_delete, dir[0].nrows);
	SiftPendDelRec *recs = palloc(Max(n, 1) * sizeof(SiftPendDelRec));

	MemoryContextDelete(icx);
	for (int i = 0; i < n; i++)
		recs[i] = (SiftPendDelRec) {.tid = tids[i],.xmax = InvalidTransactionId,.cmax = 0};
	sift_test_compact_delete = 0;
	(void) sift_del_bulk(rel, part_id, key_lo, recs, n, false, spec);
}
#endif

static bool
folds_any(void *fold, const SiftPartDesc **at, int n)
{
	for (int i = 0; fold != NULL && i < n; i++)
		if (sift_compact_fold->folds_part(fold, at[i]->part_gen))
			return true;
	return false;
}

/* The vector an output carried over is replaced: the extents it had were never in a manifest */
static void
release_outputs_dv(Relation rel, const SiftPartDesc *out)
{
	if (out->dv_blk != InvalidBlockNumber)
		sift_fsm_release(rel, &(SiftExtent) {out->dv_blk, out->dv_nblocks}, 1, InvalidFullTransactionId);
}

/* The output parts given back at once: no manifest ever held them */
static void
release_outputs(Relation rel, uint64 part_id, const SiftPartDesc *out, int nout)
{
	for (int k = 0; k < nout; k++)
	{
		int			nb;
		SiftExtent *blocks = sift_part_blocks(sift_part_open(rel, out[k].header_blk, out[k].part_gen, (uint32) part_id), &nb);

		sift_fsm_release(rel, blocks, nb, InvalidFullTransactionId);
		release_outputs_dv(rel, &out[k]);
	}
}

/* Phase 1: the rows of the inputs read and the output parts written, with no lock held: nothing of the output is reachable until the manifest takes it in. false: a deleter of a row is not settled, and no output is left */
static bool
write_outputs(Relation rel, uint64 part_id, CompactIn *in, int ngens, const SiftPartSpec *spec, int64 hot_cut, bool bounds_hold, SiftXidHorizonCache *hc, void *fold, SiftCompactResult *res,
			  SiftExtent **rel_ext, int *nrel, SiftMemTracker *mem, SiftPartDesc **out, int *nout)
{
	const SiftPartDesc **at = palloc(Max(ngens, 1) * sizeof(SiftPartDesc *));
	SiftDictMerge *dm;
	SiftPartSpec mspec;
	Sink		sink;
	SiftPartDesc joined;
	bool		split = spec->time_cols[0] > 0 && hot_cut != PG_INT64_MIN;
	uint16		level = 0;
	uint64		total_rows = 0;
	const SiftPartDicts *adopt = NULL;
	MemoryContext adopt_cx = NULL;
	const SiftColRemap **remap = palloc0(Max(ngens, 1) * sizeof(SiftColRemap *));
	bool		mapped = false,
				retier = false,
				read = true;

	for (int i = 0; i < ngens; i++)
	{
		at[i] = &in[i].d;
		remap[i] = in[i].remap;
		level = Max(level, at[i]->level);
		mapped |= remap[i] != NULL;
		retier |= sift_part_needs_retier(at[i]);
	}
	res->nparts_in = ngens;
	if (!mapped && !retier && !folds_any(fold, at, ngens) && join_parts(rel, part_id, memcpy(palloc(Max(ngens, 1) * sizeof(*at)), at, ngens * sizeof(*at)), ngens, spec, hot_cut, bounds_hold, hc->horizon, rel_ext, nrel, res, &joined))
	{
		*out = memcpy(palloc(sizeof(SiftPartDesc)), &joined, sizeof(SiftPartDesc));
		*nout = 1;
		return true;
	}
	dm = sift_dict_merge_plan(rel, (uint32) part_id, at, ngens, spec, fold != NULL ? sift_compact_fold->folded_leaves(fold) : NULL, remap, mem->budget / SIFT_COMPACT_DICT_SHARE);
	mspec = sift_dict_merge_spec(dm, spec);
	for (int i = 0; i < ngens; i++)
		total_rows += at[i]->nrows;
	for (int i = 0; i < ngens; i++)
		if (remap[i] == NULL && at[i]->nrows * 10 >= total_rows * 6)
			adopt = sift_part_dicts(rel, sift_compact_open_input(rel, at[i], (uint32) part_id, &adopt_cx));
	sink = (Sink) {.rel = rel,.part_id = (uint32) part_id,.level = level,.spec = &mspec,.dm = dm,.adopt = adopt,.mem = mem,.flush_bytes = mem->budget / SIFT_COMPACT_FLUSH_SHARE,.split = split,
		.res = res,.cx = CurrentMemoryContext};
	sink.rb[0] = sift_rowbuf_new(&mspec);
	sink.rb[1] = split ? sift_rowbuf_new(&mspec) : NULL;
	for (int i = 0; i < ngens && read; i++)
	{
		MemoryContext icx;
		SiftPart   *part = sift_compact_open_input(rel, at[i], (uint32) part_id, &icx);

		read = add_part_rows(rel, part, at[i], (uint32) part_id, &sink, hot_cut, &mspec, &dm->map[i * dm->ncols], remap[i], hc, fold, res, rel_ext, nrel);
		MemoryContextDelete(icx);
	}
	for (int side = 0; read && side < (split ? 2 : 1); side++)
	{
		if (sink.rb[side]->n > 0)
			sink_flush(&sink, side, false);
		sift_rowbuf_free(sink.rb[side]);
	}
	if (adopt_cx != NULL)
		MemoryContextDelete(adopt_cx);
	if (!read)
		release_outputs(rel, part_id, sink.out, sink.nout);
	*out = read ? sink.out : NULL;
	*nout = read ? sink.nout : 0;
	return read;
}

/* The rows an input holds dead now that were not dead in the vector the compaction read it with: their addresses and deleters */
static void
dv_delta(Relation rel, uint32 part_id, const CompactIn *in, const SiftDv *now, SiftPendDelRec **recs, int *n, int *cap)
{
	MemoryContext icx = NULL;
	SiftPart   *part = NULL;
	SiftGranuleDesc *dir = NULL;

	for (int i = 0; i < now->ndirty; i++)
	{
		const SiftDvGran *dg = &now->gran[i];
		uint64	   *tids = NULL;

		for (int k = 0; k < dg->ndead; k++)
		{
			if (bits_dead(in->dead, dg->granule_id, dg->rows[k]))
				continue;
			if (part == NULL)
			{
				part = sift_compact_open_input(rel, &in->d, part_id, &icx);
				dir = sift_part_granule_dir(rel, part);
			}
			if (tids == NULL)
				tids = sift_part_read_tids(rel, part, dir, dg->granule_id);
			if (*n == *cap)
			{
				*cap = Max(16, *cap * 2);
				*recs = *recs ? repalloc(*recs, *cap * sizeof(SiftPendDelRec)) : palloc(*cap * sizeof(SiftPendDelRec));
			}
			(*recs)[(*n)++] = (SiftPendDelRec) {.tid = tids[dg->rows[k]],.xmax = dg->xmax ? dg->xmax[k].xmax : InvalidTransactionId,.cmax = dg->xmax ? dg->xmax[k].cmax : 0};
		}
		if (tids != NULL)
			pfree(tids);
	}
	if (icx != NULL)
		MemoryContextDelete(icx);
}

/* The deletes that reached the inputs while the outputs were written, put into the deletion vectors of the outputs, which the commit makes the vectors of the parts that replace the inputs */
static void
adopt_deletes(Relation rel, uint64 part_id, const SiftPendDelRec *recs, int n, SiftPartDesc *out, int nout)
{
	bool	   *done = palloc0(n * sizeof(bool));
	MemoryContext scratch = sift_mem_allocset_create(CurrentMemoryContext, "sift compact adopt");

	for (int k = 0; k < nout; k++)
	{
		MemoryContext old = MemoryContextSwitchTo(scratch);
		SiftPart   *part = sift_part_open(rel, out[k].header_blk, out[k].part_gen, (uint32) part_id);
		SiftGranuleDesc *dir = sift_part_granule_dir(rel, part);
		SiftDvMark *marks = palloc(n * sizeof(SiftDvMark));
		uint16	   *grows = palloc(Max(part->hdr.ngranules, 1) * sizeof(uint16));
		SiftDv	   *base;
		int			nm;

		for (uint32 g = 0; g < part->hdr.ngranules; g++)
			grows[g] = dir[g].nrows;
		base = out[k].dv_blk != InvalidBlockNumber ? sift_dv_read(rel, &out[k], (uint32) part_id, grows) : NULL;
		nm = sift_del_locate(rel, part, dir, recs, n, done, marks, base, NULL);
		if (nm > 0)
		{
			release_outputs_dv(rel, &out[k]);
			(void) sift_dv_write(rel, sift_dv_merge(base, marks, nm, out[k].part_gen, sift_manifest_new_gen(rel, 1), part->hdr.ngranules, grows), (uint32) part_id, &out[k]);
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(scratch);
	}
	MemoryContextDelete(scratch);
	pfree(done);
}

/* A packed page is shared by the slots of parts of other partitions: whether a page is still referenced is decided, and its release committed, under the relation-wide lock of the splits */
static bool
any_packed_slot(const CompactIn *in, int ngens)
{
	for (int i = 0; i < ngens; i++)
		if (in[i].d.flags & SIFT_PART_FLAG_PACKED_SLOT)
			return true;
	return false;
}

static int
ref_by_gen(const void *a, const void *b)
{
	uint64		x = ((const SiftPartRef *) a)->part_gen,
				y = ((const SiftPartRef *) b)->part_gen;

	return x < y ? -1 : x > y;
}

static int
blk_cmp(const void *a, const void *b)
{
	BlockNumber x = *(const BlockNumber *) a,
				y = *(const BlockNumber *) b;

	return x < y ? -1 : x > y;
}

/* The packed pages that no part is a slot of once the inputs are gone, written to out as the extents the commit releases; how many: the manifest is read once for all of them */
static int
release_packed_pages(Relation rel, uint64 part_id, const CompactIn *in, int ngens, SiftExtent *out)
{
	SiftPartRef *leaving = palloc(Max(ngens, 1) * sizeof(SiftPartRef));
	BlockNumber *pages = palloc(Max(ngens, 1) * sizeof(BlockNumber));
	int			nleaving = 0,
				npages = 0,
				n = 0;
	SiftLivePackedSet live;

	for (int i = 0; i < ngens; i++)
		if (in[i].d.flags & SIFT_PART_FLAG_PACKED_SLOT)
		{
			leaving[nleaving++] = (SiftPartRef) {(uint32) part_id, in[i].d.part_gen};
			pages[npages++] = in[i].d.header_blk;
		}
	if (npages > 0)
	{
		qsort(leaving, nleaving, sizeof(SiftPartRef), ref_by_gen);
		qsort(pages, npages, sizeof(BlockNumber), blk_cmp);
		sift_live_packed_set_build(rel, &live);
		for (int i = 0; i < npages; i++)
			if ((i == 0 || pages[i] != pages[i - 1]) && sift_packed_unreferenced_in(rel, pages[i], leaving, nleaving, &live))
				out[n++] = (SiftExtent) {pages[i], 1};
		sift_live_packed_set_free(&live);
	}
	pfree(pages);
	pfree(leaving);
	return n;
}

/*
 * Compacts the parts of one partition in three steps. The rows are read and the output written with no lock held (readers and writers go on, new parts and deletes included), under
 * the compaction lock of the partition that only another compaction of it waits on; then, under the partition lock for the length of one commit, the manifest is read again: the
 * inputs must still be there, and the deletes that reached them meanwhile are put into the vectors of the outputs. One change of the manifest swaps the parts. Returns false, with
 * nothing changed, for an input that is gone, a deleter that is not settled or a compaction of the partition that is under way.
 */
static bool
compact_online(Relation rel, uint64 part_id, const uint8 *key_lo, const uint64 *gens, int ngens, const SiftPartSpec *spec, int64 hot_cut, FullTransactionId horizon,
			   SiftCompactResult *res, SiftMemTracker *mem)
{
	SiftManifestRead mr;
	SiftPartDesc *parts;
	CompactIn  *in;
	int			nparts,
				nrel = 0,
				nout = 0,
				ndelta = 0,
				capdelta = 0;
	SiftExtent *rel_ext = palloc(sizeof(SiftExtent));
	SiftManifestEdit e = {.part_id = part_id,.key_lo = key_lo,.drop_gens = gens,.ndrop = ngens};
	SiftPartDesc *out = NULL;
	SiftPendDelRec *delta = NULL;
	SiftXidHorizonCache hc;
	SiftPartGenIndex by_gen = {0};
	void	   *fold;
	bool		packed,
				bounds_hold;

	memset(res, 0, sizeof(*res));
	sift_xid_horizon_init(&hc, horizon);
	if (!ConditionalLockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock))
		return false;
	mr = sift_manifest_read_range(rel, key_lo, NULL);
	if (mr.n == 0 || mr.entries[0].part_id != part_id)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
	bounds_hold = (mr.entries[0].flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0 || (sift_manifest_read_col_delta(rel, &mr.entries[0]).flags & SIFT_CDLA_DIRTY_TIME0) == 0;
	parts = sift_manifest_read_parts(rel, &mr.entries[0], &nparts);
	in = read_inputs(rel, part_id, parts, nparts, gens, ngens);
	fold = sift_compact_fold != NULL ? sift_compact_fold->open(rel, &mr.entries[0], parts, nparts, gens, ngens, horizon) : NULL;
	if (!write_outputs(rel, part_id, in, ngens, spec, hot_cut, bounds_hold, &hc, fold, res, &rel_ext, &nrel, mem, &out, &nout))
	{
		UnlockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
		return false;
	}
	SIFT_F2_GATE(SIFT_F2_GATE_COMPACT);
#ifdef SIFT_TESTING
	if (sift_test_compact_delete > 0)
		test_delete_meanwhile(rel, part_id, key_lo, &in[0], spec);
#endif
	packed = any_packed_slot(in, ngens);
	if (packed)
		LockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock);
	LockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	mr = sift_manifest_read_range(rel, key_lo, NULL);
	parts = mr.n > 0 && mr.entries[0].part_id == part_id ? sift_manifest_read_parts(rel, &mr.entries[0], &nparts) : NULL;
	if (parts != NULL)
		sift_part_gen_idx_init(&by_gen, parts, nparts);
	for (int i = 0; i < ngens && parts != NULL; i++)
	{
		const SiftPartDesc *now_d = sift_part_gen_idx_find(&by_gen, gens[i]);

		if (now_d == NULL)
			parts = NULL;
		else if (now_d->header_blk != in[i].d.header_blk)
			parts = NULL;
		else if (now_d->dv_gen != in[i].d.dv_gen || now_d->dv_blk != in[i].d.dv_blk ||
				 (now_d->dv_blk != InvalidBlockNumber && sift_dv_stream_len(rel, now_d->dv_blk) != in[i].dv_len))
		{
			MemoryContext vector_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift compact vector");
			MemoryContext keep = MemoryContextSwitchTo(vector_cx);
			SiftDv	   *now = sift_dv_read(rel, now_d, (uint32) part_id, in[i].grows);

			MemoryContextSwitchTo(keep);
			dv_delta(rel, (uint32) part_id, &in[i], now, &delta, &ndelta, &capdelta);
			MemoryContextDelete(vector_cx);
			in[i].d = *now_d;
		}
		else
			in[i].d = *now_d;
	}
	sift_part_gen_idx_free(&by_gen);
	if (parts == NULL)
	{
		release_outputs(rel, part_id, out, nout);
		UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
		if (packed)
			UnlockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock);
		UnlockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
		return false;
	}
	if (ndelta > 0)
		adopt_deletes(rel, part_id, delta, ndelta, out, nout);
	rel_ext = repalloc(rel_ext, (nrel + 2 * ngens + 1) * sizeof(SiftExtent));
	for (int i = 0; i < ngens; i++)
		if (in[i].d.dv_blk != InvalidBlockNumber)
			rel_ext[nrel++] = (SiftExtent) {in[i].d.dv_blk, in[i].d.dv_nblocks};
	nrel += release_packed_pages(rel, part_id, in, ngens, rel_ext + nrel);
	if (fold != NULL)
		sift_compact_fold->commit(fold, &mr.entries[0], parts, nparts, out, nout, &e, &rel_ext, &nrel, &res->stripped);
	e.nadd = nout;
	e.add_parts = out;
	e.release = rel_ext;
	e.nrelease = nrel;
	(void) sift_wal_snapshot_conflict(rel, InvalidBlockNumber, res->stripped);
	sift_manifest_commit(rel, &e);
	SIFT_CRASH_POINT_IF(fold != NULL, f2_col_delta_fold_post_p2);
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	if (packed)
		UnlockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock);
	UnlockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
	(void) sift_mem_used(mem);
	return true;
}

/*
 * The whole compaction runs in a context of its own, held to the maintenance budget (the half of it: the other half is what the part writer and the commit need), and what it
 * read and wrote is given back, to the system too, when it is done.
 */
bool
sift_partition_compact_online(Relation rel, uint64 part_id, const uint8 *key_lo, const uint64 *gens, int ngens, const SiftPartSpec *spec, int64 hot_cut, FullTransactionId horizon,
							  SiftCompactResult *res)
{
	SiftMemTracker mem;
	MemoryContext old;
	bool		done;

	sift_mem_init(&mem, "f2_compact", CurrentMemoryContext, true, SIFT_BUDGET_MAINT_MEM, 2, 1);
	old = MemoryContextSwitchTo(mem.root_cx);
	done = compact_online(rel, part_id, key_lo, gens, ngens, spec, hot_cut, horizon, res, &mem);
	MemoryContextSwitchTo(old);
	sift_mem_end(&mem);
	return done;
}

static bool
partition_idle(const SiftPartitionEntry *e)
{
	return e->l0_slab_blk == InvalidBlockNumber && e->pend_del_blk == InvalidBlockNumber && (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0;
}

/* The entry of a partition as it is now, in place of the one a read of the manifest gave: only the fields a flush changes differ, the partitions themselves cannot move under the lock of the splits */
static void
refresh_entry(Relation rel, SiftPartitionEntry *e)
{
	if (!sift_manifest_find_entry(rel, e->part_id, e->key_lo, e))
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) e->part_id)));
}

/*
 * A transaction that holds a partition for its rows (table/f2/f2_lock_esc.h) holds the split lock shared: the merge yields to it and comes again with the next pass. The manifest is read
 * once, under the lock that no split or merge passes: the neighbours are walked in one pass, the pair at the cursor judged by its entries as they are now, and a merged partition is judged
 * again against the one after it (the one before it was judged against its smaller self and did not join, so it does not join the larger).
 */
int
sift_compact_merge_neighbours(Relation rel)
{
	int			merged = 0;

	if (!ConditionalLockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock))
		return 0;
	{
		SiftManifestRead mr = sift_manifest_read_range(rel, NULL, NULL);
		int		   *next = palloc(Max(mr.n, 1) * sizeof(int));
		int			i = 0;

		for (int k = 0; k < mr.n; k++)
			next[k] = k + 1 < mr.n ? k + 1 : -1;
		while (i >= 0 && next[i] >= 0)
		{
			SiftPartitionEntry *a = &mr.entries[i],
					   *b = &mr.entries[next[i]];

			sift_vacuum_delay_point();
			refresh_entry(rel, a);
			refresh_entry(rel, b);
			if (partition_idle(a) && partition_idle(b) && a->total_bytes + b->total_bytes < SIFT_PARTITION_MERGE_BYTES)
			{
				sift_manifest_merge(rel, a->part_id, a->key_lo, b->part_id, b->key_lo);
				merged++;
				a->total_bytes += b->total_bytes;
				memcpy(a->key_hi, b->key_hi, SIFT_BOUND_BYTES);
				next[i] = next[next[i]];
			}
			else
				i = next[i];
		}
		pfree(next);
	}
	UnlockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock);
	return merged;
}
