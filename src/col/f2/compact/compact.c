/*
 * col/compact.c - partition-local compaction (col/compact.h).
 */
#include "core/host.h"

#include "col/f2/scope/scope.h"

#include "access/transam.h"
#include "commands/vacuum.h"
#include "miscadmin.h"
#include "storage/lmgr.h"
#include "storage/procarray.h"

#include "col/f2/compact/adopt.h"
#include "col/f2/compact/par.h"
#include "col/f2/part/idx.h"
#include "core/memtrack.h"
#include "col/f2/del/delete.h"
#include "col/f2/del/delvec_delta.h"
#include "col/f2/dv_join.h"
#include "col/f2/key_sig.h"
#include "col/f2/kll/kll.h"
#include "col/f2/store/store.h"
#include "core/compat.h"
#include "core/crash_point.h"
#include "core/f2/col_delta_anchor.h"
#include "core/f2/gate.h"
#include "core/f2/pace.h"
#include "core/page_io.h"
#include "core/visibility/visibility.h"
#include "core/wal_conflict.h"
#include "core/xid_cache.h"

const SiftCompactFold *sift_compact_fold = NULL;

/* The rows of the inputs are written as a part when the compaction holds this share of its budget (a half), and the merged dictionaries of the string columns take at most this share of it (an eighth) */
#define SIFT_COMPACT_FLUSH_SHARE	2
#define SIFT_COMPACT_PROBE_ROWS	1024	/* the rows a row of the inputs is first measured on */
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
void
sift_compact_release_part(const SiftPart *part, SiftExtent **rel_ext, int *nrel)
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

/* Which of the two buffers takes a row: 1, the cold one, when its time column of the cut is older than the cut */
static int
row_side(const SiftPartSpec *spec, const SiftRowBuf *rb, int r, const SiftCompactCut *cut)
{
	int			c = spec->time_cols[cut->slot] - 1;

	return c >= 0 && cut->at != PG_INT64_MIN && !rb->isnull[c][r] && rb->i[c][r] < cut->at;
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
	Size		row_bytes;		/* the bytes a row of the inputs takes in a row buffer, as the last slice (or measure_row) measured it; 0 before the first */
	bool		split;
	const SiftCompactCut *cut;
	bool		bounds_hold;
	FullTransactionId horizon;
	bool		folding;		/* the compaction folds column-delta records: its outputs stay as written, joining them left the chain unfolded */
	int			open[2];		/* the output of each side that is still under the floor (its place in out), or -1 */
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

/*
 * The part just written joins the last one of its side, when both stay under the floor together and follow one another (rows with no key do): a compaction under a small budget cuts its
 * output where the memory filled, not where the rows ended, and rows that compress well make a part of a few pages each time. They were never in a manifest: the blocks of the two are
 * given back at once, so at most two small parts of a side are held at any time.
 */
static bool
join_open(Sink *s, int side, const SiftPartDesc *d)
{
	const SiftPartDesc *at[2] = {&s->out[Max(s->open[side], 0)], d};
	MemoryContext scratch;
	MemoryContext old;
	SiftExtent *given;
	SiftCompactResult part = {0};
	SiftPartDesc joined;
	int			ngiven = 0;
	bool		done;

	if (s->folding || s->open[side] < 0 || at[0]->nbytes + d->nbytes >= SIFT_COMPACT_FLOOR_BYTES)
		return false;
	scratch = sift_mem_allocset_create(s->cx, "sift compact join");
	old = MemoryContextSwitchTo(scratch);
	given = palloc(sizeof(SiftExtent));
	done = sift_compact_join_parts(s->rel, s->part_id, at, 2, s->spec, s->cut, s->bounds_hold, s->horizon, &given, &ngiven, &part, &joined);
	if (done)
		sift_fsm_release(s->rel, given, ngiven, InvalidFullTransactionId);
	MemoryContextSwitchTo(old);
	MemoryContextDelete(scratch);
	if (done)
	{
		s->res->bytes_out += joined.nbytes;
		s->res->granules_written += part.granules_written;
		s->out[s->open[side]] = joined;
	}
	return done;
}

/* whole: the buffer is written because the compaction holds its share of the budget, not because the rows ended; at the last level such a part is as large as the compaction can make it, and is not merged again by count (SIFT_PART_FLAG_BUILT) */
static void
sink_flush(Sink *s, int side, bool whole)
{
	MemoryContext scratch = sift_mem_allocset_create(s->cx, "sift compact output");
	MemoryContext old = MemoryContextSwitchTo(scratch);
	uint32		flags = (!s->split || !s->cut->tier ? 0 : side ? SIFT_PART_FLAG_TIME_COLD : SIFT_PART_FLAG_TIME_HOT) | (whole && s->level + 1 >= SIFT_MAX_PART_LEVEL ? SIFT_PART_FLAG_BUILT : 0);
	SiftPartDesc d = write_side(s->rel, s->part_id, s->level, s->rb[side], s->spec, s->dm, flags, s->adopt, s->res);
	Size		freed = s->rb[side]->mem_bytes;

	MemoryContextSwitchTo(old);
	MemoryContextDelete(scratch);
	s->res->rows_out += s->rb[side]->n;
	if (!join_open(s, side, &d))
	{
		if (s->nout == s->cap)
		{
			s->cap = Max(8, s->cap * 2);
			s->out = s->out != NULL ? repalloc(s->out, s->cap * sizeof(SiftPartDesc)) : MemoryContextAlloc(s->cx, s->cap * sizeof(SiftPartDesc));
		}
		s->out[s->nout++] = d;
		s->open[side] = d.nbytes < SIFT_COMPACT_FLOOR_BYTES ? s->nout - 1 : -1;
	}
	sift_rowbuf_free(s->rb[side]);
	old = MemoryContextSwitchTo(s->cx);
	s->rb[side] = sift_rowbuf_new(s->spec);
	MemoryContextSwitchTo(old);
	sift_mem_release_os(freed);
}

/* The bytes of the rows the sink holds */
static Size
sink_held(const Sink *s)
{
	return s->rb[0]->mem_bytes + (s->split ? s->rb[1]->mem_bytes : 0);
}

/*
 * The side that holds more is written while the compaction holds flush_bytes or more and its rows least_held: what the rows of the inputs are in memory is never more than that and what a
 * granule (or a slice of one) adds; the rows of a sliced granule wait for half the share, so that what else the compaction holds (dictionaries, folds) does not cut them into parts of a few rows
 */
static void
sink_flush_due(Sink *s, Size least_held)
{
	while (sift_mem_used(s->mem) >= s->flush_bytes && sink_held(s) >= least_held)
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
	uint32		row0;			/* the row of the granule the first row of the slice is */
	SiftXidHorizonCache *hc;
	SiftCompactResult *res;
} GranuleRows;

static bool
row_goes(SiftRowBuf *one, int r, void *arg)
{
	GranuleRows *gr = arg;
	Inserter	ins = inserter_of(one->xmin[r], gr->hc, &gr->res->stripped);

	gr->res->rows_in++;
	if (bits_dead(gr->dead, gr->granule, (uint16) (gr->row0 + r)))
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

/* The rows of a slice go to the sink: an empty buffer of the one side takes the slice's buffer as it is, the others copy its rows */
static void
sink_take(Sink *sink, SiftRowBuf *one)
{
	if (!sink->split && sink->rb[0]->n == 0)
	{
		sift_rowbuf_free(sink->rb[0]);
		sink->rb[0] = one;
		return;
	}
	for (int r = 0; r < one->n; r++)
		sift_rowbuf_copy_row(sink->rb[row_side(sink->spec, one, r, sink->cut)], one, r, one->xmin[r], one->cmin[r], 0);
	sift_rowbuf_free(one);
}

/*
 * A granule whose rows take no more than the flush share (as the last slice measured a row) is read whole, so that the parts are written in whole granules of the inputs; a larger one is
 * read in slices that each fill the room sink_flush_due leaves before it writes, so that it is never in memory whole
 */
static bool
granule_sliced(const Sink *s, int nrows)
{
	return (Size) nrows * s->row_bytes > s->flush_bytes;
}

/* The bytes a row of the inputs takes in a row buffer, measured on the first rows in a buffer of their own: the first granule is read whole when it fits, as every other one */
static void
measure_row(Sink *s, const SiftGranuleView *v, const SiftRowVer *vers, const SiftCodeMap *code_map, const SiftColRemap *remap)
{
	SiftRowBuf *probe = sift_rowbuf_new(s->spec);

	sift_rowbuf_add_granule_mapped(probe, v, vers, s->spec, code_map, remap, 0, Min(v->hdr->nrows, SIFT_COMPACT_PROBE_ROWS));
	s->row_bytes = Max(probe->mem_bytes / (Size) probe->n, 1);
	sift_rowbuf_free(probe);
}

/* The rows of the next slice of a granule of nrows rows that has left rows */
static int
slice_rows(Sink *s, int nrows, int left)
{
	Size		used,
				held,
				room;

	if (!granule_sliced(s, nrows))
		return left;
	used = sift_mem_used(s->mem);
	held = sink_held(s);
	room = Max(used < s->flush_bytes ? s->flush_bytes - used : 0, held < s->flush_bytes / 2 ? s->flush_bytes / 2 - held : 0);
	return (int) Min((Size) left, Max(room / s->row_bytes, 1));
}

static void
add_slice(Sink *sink, const SiftGranuleView *v, const SiftRowVer *vers, int r0, int r1, const SiftCodeMap *code_map, const SiftColRemap *remap, uint64 part_gen, void *fold,
		  GranuleRows *gr, SiftExtent **rel_ext, int *nrel)
{
	MemoryContext old = MemoryContextSwitchTo(sink->cx);
	SiftRowBuf *one = sift_rowbuf_new(sink->spec);

	MemoryContextSwitchTo(old);
	sift_rowbuf_add_granule_mapped(one, v, vers, sink->spec, code_map, remap, r0, r1);
	if (fold != NULL)
		sift_compact_fold->apply(fold, part_gen, gr->granule, (uint32) r0, one, rel_ext, nrel);
	sink->row_bytes = Max(one->mem_bytes / (Size) Max(one->n, 1), 1);
	gr->row0 = (uint32) r0;
	(void) sift_rowbuf_drop_if(one, row_goes, gr);
	sink_take(sink, one);
}

static bool
add_part_rows(Relation rel, SiftPart *part, const SiftPartDesc *d, uint32 part_id, uint32 g_lo, uint32 g_hi, Sink *sink, const SiftCodeMap *code_map,
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
	for (uint32 g = g_lo; g < g_hi; g++)
	{
		MemoryContext old = MemoryContextSwitchTo(granule_cx);
		SiftGranuleDesc gd;
		GranuleRows gr = {.dead = dead,.granule = g,.hc = hc,.res = res};
		Size		least_held = 0;

		sift_vacuum_delay_point();
		sift_f2_pace_pay();
		char	   *rec = sift_part_read_granule(rel, part, g, &gd);
		SiftGranuleView v = sift_granule_open_part(rel, part, rec, gd.data_len, &gd);
		SiftRowVer *vers = sift_part_read_vers(rel, part, dir, g);

		for (int r0 = 0, r1; r0 < v.hdr->nrows; r0 = r1)
		{
			if (sink->row_bytes == 0)
				measure_row(sink, &v, vers, code_map, remap);
			r1 = r0 + slice_rows(sink, v.hdr->nrows, v.hdr->nrows - r0);
			add_slice(sink, &v, vers, r0, r1, code_map, remap, d->part_gen, fold, &gr, rel_ext, nrel);
			least_held = granule_sliced(sink, v.hdr->nrows) ? sink->flush_bytes / 2 : 0;
			if (r1 < v.hdr->nrows && least_held > 0)
				sink_flush_due(sink, least_held);
		}
		MemoryContextSwitchTo(old);
		MemoryContextReset(granule_cx);
		sink_flush_due(sink, least_held);
	}
	MemoryContextDelete(granule_cx);
	sift_compact_release_part(part, rel_ext, nrel);
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
	uint32		g_lo;			/* the granules [g_lo, g_hi) are the ones this compaction reads of the part: all, but for a slice of a compaction of several participants */
	uint32		g_hi;
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
		in[i].ngranules = in[i].g_hi = part->hdr.ngranules;
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

static bool
folds_any(void *fold, const SiftPartDesc **at, int n)
{
	for (int i = 0; fold != NULL && i < n; i++)
		if (sift_compact_fold->folds_part(fold, at[i]->part_gen))
			return true;
	return false;
}

/* The rows read and the outputs written by the participants of a VACUUM (PARALLEL n), each a share of the granules of the inputs (compact/par.c); the blocks of the inputs are given back as those of a serial compaction are */
static SiftCompactParResult
par_written(Relation rel, uint64 part_id, const CompactIn *in, int ngens, const SiftCompactCut *cut, bool bounds_hold, FullTransactionId horizon, SiftCompactResult *res, SiftExtent **rel_ext, int *nrel,
			SiftPartDesc **out, int *nout)
{
	SiftPartDesc *descs = palloc(Max(ngens, 1) * sizeof(SiftPartDesc));
	uint32	   *granules = palloc(Max(ngens, 1) * sizeof(uint32));
	SiftCompactParResult par;

	for (int i = 0; i < ngens; i++)
	{
		descs[i] = in[i].d;
		granules[i] = in[i].ngranules;
	}
	par = sift_compact_par_write(rel, part_id, descs, granules, ngens, cut, bounds_hold, horizon, res, out, nout);
	if (par == SIFT_COMPACT_PAR_UNSETTLED)
	{
		sift_compact_release_outputs(rel, part_id, *out, *nout);
		*out = NULL;
		*nout = 0;
	}
	for (int i = 0; par == SIFT_COMPACT_PAR_DONE && i < ngens; i++)
	{
		MemoryContext icx;

		sift_compact_release_part(sift_compact_open_input(rel, &in[i].d, (uint32) part_id, &icx), rel_ext, nrel);
		MemoryContextDelete(icx);
	}
	pfree(descs);
	pfree(granules);
	return par;
}

/*
 * Phase 1: the rows of the inputs read and the output parts written, with no lock held: nothing of the output is reachable until the manifest takes it in. false: a deleter of a row is not
 * settled, and no output is left. join_only: the parts are joined or nothing is written (*nout 0).
 */
static bool
write_outputs(Relation rel, uint64 part_id, CompactIn *in, int ngens, const SiftPartSpec *spec, const SiftCompactCut *cut, bool bounds_hold, bool join_only, bool slice, SiftXidHorizonCache *hc, void *fold,
			  SiftCompactResult *res, SiftExtent **rel_ext, int *nrel, SiftMemTracker *mem, SiftPartDesc **out, int *nout)
{
	const SiftPartDesc **at = palloc(Max(ngens, 1) * sizeof(SiftPartDesc *));
	SiftDictMerge *dm;
	SiftPartSpec mspec;
	Sink		sink;
	SiftPartDesc joined;
	bool		split = spec->time_cols[cut->slot] > 0 && cut->at != PG_INT64_MIN;
	uint16		level = 0;
	uint64		total_rows = 0;
	const SiftPartDicts *adopt = NULL;
	MemoryContext adopt_cx = NULL;
	SiftPart   *dominant_part = NULL;
	int			dominant = -1;
	const SiftColRemap **remap = palloc0(Max(ngens, 1) * sizeof(SiftColRemap *));
	bool		mapped = false,
				read = true;
	SiftCompactParResult par;

	for (int i = 0; i < ngens; i++)
	{
		at[i] = &in[i].d;
		remap[i] = in[i].remap;
		level = Max(level, at[i]->level);
		mapped |= remap[i] != NULL;
	}
	res->nparts_in = ngens;
	if (!slice && !mapped && !folds_any(fold, at, ngens) && sift_compact_join_parts(rel, part_id, memcpy(palloc(Max(ngens, 1) * sizeof(*at)), at, ngens * sizeof(*at)), ngens, spec, cut, bounds_hold, hc->horizon, rel_ext, nrel, res, &joined))
	{
		*out = memcpy(palloc(sizeof(SiftPartDesc)), &joined, sizeof(SiftPartDesc));
		*nout = 1;
		return true;
	}
	if (join_only)
	{
		*nout = 0;
		return true;
	}
	if (!slice && !mapped && !folds_any(fold, at, ngens) && (par = par_written(rel, part_id, in, ngens, cut, bounds_hold, hc->horizon, res, rel_ext, nrel, out, nout)) != SIFT_COMPACT_PAR_OFF)
		return par == SIFT_COMPACT_PAR_DONE;
	dm = sift_dict_merge_plan(rel, (uint32) part_id, at, ngens, spec, fold != NULL ? sift_compact_fold->folded_leaves(fold) : NULL, remap, mem->budget / SIFT_COMPACT_DICT_SHARE);
	mspec = sift_dict_merge_spec(dm, spec);
	for (int i = 0; i < ngens; i++)
		if (remap[i] != NULL && remap[i]->incomplete)
			mspec.col_ids = sift_f2_unnamed_path_ids(sift_f2_store_open(rel));
	for (int i = 0; i < ngens; i++)
		total_rows += at[i]->nrows;
	for (int i = 0; i < ngens; i++)
		if (remap[i] == NULL && at[i]->nrows * 10 >= total_rows * 6)
			dominant = i;
	if (dominant >= 0)
		adopt = sift_part_dicts(rel, dominant_part = sift_compact_open_input(rel, at[dominant], (uint32) part_id, &adopt_cx));
	sink = (Sink) {.rel = rel,.part_id = (uint32) part_id,.level = level,.spec = &mspec,.dm = dm,.adopt = adopt,.mem = mem,.flush_bytes = mem->budget / SIFT_COMPACT_FLUSH_SHARE,.split = split,.cut = cut,.bounds_hold = bounds_hold,.horizon = hc->horizon,.folding = folds_any(fold, at, ngens),.open = {-1, -1},
		.res = res,.cx = CurrentMemoryContext};
	sink.rb[0] = sift_rowbuf_new(&mspec);
	sink.rb[1] = split ? sift_rowbuf_new(&mspec) : NULL;
	for (int i = 0; i < ngens && read; i++)
	{
		MemoryContext icx = NULL;
		SiftPart   *part = i == dominant ? dominant_part : sift_compact_open_input(rel, at[i], (uint32) part_id, &icx);

		read = add_part_rows(rel, part, at[i], (uint32) part_id, in[i].g_lo, in[i].g_hi, &sink, &dm->map[i * dm->ncols], remap[i], hc, fold, res, rel_ext, nrel);
		if (icx != NULL)
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
		sift_compact_release_outputs(rel, part_id, sink.out, sink.nout);
	*out = read ? sink.out : NULL;
	*nout = read ? sink.nout : 0;
	return read;
}

/* What one participant of a compaction of several writes: the rows of the granules [lo[i], hi[i]) of the part in[i] as parts of its own, which the leader of the compaction commits with the rest; false: a deletion in them is not settled */
bool
sift_compact_write_slice(Relation rel, uint64 part_id, const SiftPartSpec *spec, const SiftPartDesc *in, int nin, const uint32 *lo, const uint32 *hi, const SiftCompactCut *cut, bool bounds_hold,
						 FullTransactionId horizon, int nparticipants, SiftCompactResult *res, SiftPartDesc *out, int out_cap, int *nout)
{
	SiftMemTracker mem;
	MemoryContext old;
	SiftXidHorizonCache hc;
	SiftExtent *rel_ext;
	SiftPartDesc *written = NULL;
	uint64	   *gens = palloc(nin * sizeof(uint64));
	CompactIn  *ci;
	int			nrel = 0;
	bool		read;

	for (int i = 0; i < nin; i++)
		gens[i] = in[i].part_gen;
	sift_mem_init(&mem, "f2_compact", CurrentMemoryContext, true, SIFT_BUDGET_MAINT_MEM, 2 * nparticipants, 1);
	old = MemoryContextSwitchTo(mem.root_cx);
	memset(res, 0, sizeof(*res));
	sift_xid_horizon_init(&hc, horizon);
	rel_ext = palloc(sizeof(SiftExtent));
	ci = read_inputs(rel, part_id, in, nin, gens, nin);
	for (int i = 0; i < nin; i++)
	{
		ci[i].g_lo = lo[i];
		ci[i].g_hi = hi[i];
	}
	read = write_outputs(rel, part_id, ci, nin, spec, cut, bounds_hold, false, true, &hc, NULL, res, &rel_ext, &nrel, &mem, &written, nout);
	if (read && *nout > out_cap)
	{
		sift_compact_release_outputs(rel, part_id, written, *nout);
		ereport(ERROR,
				(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
				 errmsg("sift: a share of a parallel compaction wrote %d parts, its slot holds %d", *nout, out_cap),
				 errhint("Run VACUUM with PARALLEL 0 to compact this partition without parallel workers.")));
	}
	if (read && *nout > 0)
		memcpy(out, written, *nout * sizeof(SiftPartDesc));
	MemoryContextSwitchTo(old);
	sift_mem_end(&mem);
	pfree(gens);
	return read;
}

/* The rows of now that are not dead in dead */
static int64
new_dead(const DeadBits dead, const SiftDv *now)
{
	int64		n = 0;

	for (int i = 0; i < now->ndirty; i++)
	{
		CHECK_FOR_INTERRUPTS();
		for (int k = 0; k < now->gran[i].ndead; k++)
			n += !bits_dead(dead, now->gran[i].granule_id, now->gran[i].rows[k]);
	}
	return n;
}

/* The rows an input holds dead now that were not dead in the vector the compaction read it with: the addresses they have in the outputs and their deleters */
static void
dv_delta(Relation rel, uint32 part_id, const CompactIn *in, const SiftDv *now, void *fold, SiftCompactDeletes *del)
{
	MemoryContext icx = NULL;
	SiftPart   *part = NULL;
	SiftGranuleDesc *dir = NULL;

	sift_compact_deletes_reserve(del, new_dead(in->dead, now));
	for (int i = 0; i < now->ndirty; i++)
	{
		const SiftDvGran *dg = &now->gran[i];
		uint64	   *tids = NULL;

		for (int k = 0; k < dg->ndead; k++)
		{
			CHECK_FOR_INTERRUPTS();
			if (bits_dead(in->dead, dg->granule_id, dg->rows[k]))
				continue;
			if (part == NULL)
			{
				part = sift_compact_open_input(rel, &in->d, part_id, &icx);
				dir = sift_part_granule_dir(rel, part);
			}
			if (tids == NULL)
			{
				tids = sift_part_read_tids(rel, part, dir, dg->granule_id);
				if (fold != NULL)
					sift_compact_fold->row_tids(fold, in->d.part_gen, dg->granule_id, tids, dir[dg->granule_id].nrows);
			}
			del->recs[del->n++] = (SiftPendDelRec) {.tid = tids[dg->rows[k]],.xmax = dg->xmax ? dg->xmax[k].xmax : InvalidTransactionId,.cmax = dg->xmax ? dg->xmax[k].cmax : 0};
		}
		if (tids != NULL)
			pfree(tids);
	}
	if (icx != NULL)
		MemoryContextDelete(icx);
}

/* The compaction gives up under its locks: the outputs and what the fold wrote go back, nothing was committed */
static bool
abandon(Relation rel, uint64 part_id, const SiftPartDesc *out, int nout, bool packed, void *fold)
{
	if (fold != NULL)
		sift_compact_fold->forget(fold);
	sift_compact_release_outputs(rel, part_id, out, nout);
	UnlockPage(rel, SIFT_PART_LOCK_BLKNO(part_id), ExclusiveLock);
	if (packed)
		UnlockPage(rel, SIFT_F2_SPLIT_LOCK_BLKNO, ExclusiveLock);
	UnlockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
	return false;
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
compact_online(Relation rel, uint64 part_id, const uint8 *key_lo, const uint64 *gens, int ngens, const SiftPartSpec *spec, const SiftCompactCut *cut, FullTransactionId horizon,
			   bool join_only, SiftCompactResult *res, SiftMemTracker *mem)
{
	SiftManifestRead mr;
	SiftPartDesc *parts;
	CompactIn  *in;
	int			nparts,
				nrel = 0,
				nout = 0;
	SiftExtent *rel_ext = palloc(sizeof(SiftExtent));
	SiftManifestEdit e = {.part_id = part_id,.key_lo = key_lo,.drop_gens = gens,.ndrop = ngens,.key_sig = sift_f2_spec_key_sig(spec)};
	SiftPartDesc *out = NULL;
	SiftCompactDeletes delta = {0};
	SiftXidHorizonCache hc;
	SiftPartGenIndex by_gen = {0};
	void	   *fold;
	bool		packed,
				bounds_hold;

	memset(res, 0, sizeof(*res));
	sift_xid_horizon_init(&hc, horizon);
	if (!ConditionalLockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock))
		return false;
	sift_fsm_meta_pool_drain(rel);
	mr = sift_manifest_read_range(rel, key_lo, NULL);
	if (mr.n == 0 || mr.entries[0].part_id != part_id)
		ereport(ERROR, (errcode(ERRCODE_NO_DATA_FOUND), errmsg("sift: partition %llu is not in the manifest", (unsigned long long) part_id)));
	bounds_hold = (mr.entries[0].flags & SIFT_PARTENTRY_HAS_COL_DELTA) == 0 || (sift_manifest_read_col_delta(rel, &mr.entries[0]).flags & (cut->slot == 0 ? SIFT_CDLA_DIRTY_TIME0 : SIFT_CDLA_DIRTY_TIME1)) == 0;
	parts = sift_manifest_read_parts(rel, &mr.entries[0], &nparts);
	in = read_inputs(rel, part_id, parts, nparts, gens, ngens);
	fold = sift_compact_fold != NULL ? sift_compact_fold->open(rel, &mr.entries[0], parts, nparts, gens, ngens, horizon) : NULL;
	if (!write_outputs(rel, part_id, in, ngens, spec, cut, bounds_hold, join_only, false, &hc, fold, res, &rel_ext, &nrel, mem, &out, &nout))
	{
		UnlockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
		return false;
	}
	if (join_only && nout == 0)
	{
		UnlockPage(rel, SIFT_COMPACT_LOCK_BLKNO(part_id), ExclusiveLock);
		return true;
	}
	if (fold != NULL)
		sift_compact_fold->prepare(fold, part_id, key_lo, out, nout);
	SIFT_F2_GATE(SIFT_F2_GATE_COMPACT);
#ifdef SIFT_TESTING
	if (sift_test_compact_delete > 0)
		sift_compact_test_delete_meanwhile(rel, part_id, key_lo, &in[0].d, spec);
#endif
	packed = any_packed_slot(in, ngens);
	SIFT_TEST_GATE(sift_test_release_gate, 6);
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
			dv_delta(rel, (uint32) part_id, &in[i], now, fold, &delta);
			MemoryContextDelete(vector_cx);
			in[i].d = *now_d;
		}
		else
			in[i].d = *now_d;
	}
	sift_part_gen_idx_free(&by_gen);
	if (parts == NULL || (delta.n > 0 && sift_compact_adoption_ambiguous(rel, part_id, &delta, out, nout)))
		return abandon(rel, part_id, out, nout, packed, fold);
	if (delta.n > 0)
		sift_compact_adopt_deletes(rel, part_id, &delta, out, nout);
	rel_ext = repalloc(rel_ext, (nrel + 2 * ngens + 1) * sizeof(SiftExtent));
	for (int i = 0; i < ngens; i++)
		if (in[i].d.dv_blk != InvalidBlockNumber)
			rel_ext[nrel++] = (SiftExtent) {in[i].d.dv_blk, in[i].d.dv_nblocks};
	nrel += release_packed_pages(rel, part_id, in, ngens, rel_ext + nrel);
	if (fold != NULL && !sift_compact_fold->commit(fold, &mr.entries[0], parts, nparts, out, nout, &e, &rel_ext, &nrel, &res->stripped))
		return abandon(rel, part_id, out, nout, packed, fold);
	e.nadd = nout;
	e.add_parts = out;
	e.release = rel_ext;
	e.nrelease = nrel;
	(void) sift_wal_snapshot_conflict(rel, InvalidBlockNumber, res->stripped);
	if (sift_f2_scoped(rel))
		sift_f2_scope_refresh_for(rel, &e);
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
static bool
compact_budgeted(Relation rel, uint64 part_id, const uint8 *key_lo, const uint64 *gens, int ngens, const SiftPartSpec *spec, SiftCompactCut cut, FullTransactionId horizon, bool join_only,
				 SiftCompactResult *res)
{
	SiftMemTracker mem;
	MemoryContext old;
	bool		done;

	sift_mem_init(&mem, "f2_compact", CurrentMemoryContext, true, SIFT_BUDGET_MAINT_MEM, 2, 1);
	old = MemoryContextSwitchTo(mem.root_cx);
	done = compact_online(rel, part_id, key_lo, gens, ngens, spec, &cut, horizon, join_only, res, &mem);
	MemoryContextSwitchTo(old);
	sift_mem_end(&mem);
	return done;
}

bool
sift_partition_compact_online(Relation rel, uint64 part_id, const uint8 *key_lo, const uint64 *gens, int ngens, const SiftPartSpec *spec, int64 hot_cut, FullTransactionId horizon,
							  SiftCompactResult *res)
{
	return compact_budgeted(rel, part_id, key_lo, gens, ngens, spec, SIFT_COMPACT_HOT_CUT(hot_cut), horizon, false, res);
}

bool
sift_partition_join_online(Relation rel, uint64 part_id, const uint8 *key_lo, const uint64 *gens, int ngens, const SiftPartSpec *spec, int64 hot_cut, FullTransactionId horizon,
						   SiftCompactResult *res)
{
	return compact_budgeted(rel, part_id, key_lo, gens, ngens, spec, SIFT_COMPACT_HOT_CUT(hot_cut), horizon, true, res);
}

bool
sift_partition_cut_online(Relation rel, uint64 part_id, const uint8 *key_lo, const uint64 *gens, int ngens, const SiftPartSpec *spec, SiftCompactCut cut, FullTransactionId horizon,
						  SiftCompactResult *res)
{
	return compact_budgeted(rel, part_id, key_lo, gens, ngens, spec, cut, horizon, false, res);
}
