/*
 * agg/unit.c - answering one unit: late conjuncts, overflow checks, filters, process_unit (its pre-aggregates are agg/unit_preagg.c)
 */
#include "postgres.h"

#include <float.h>
#include <math.h>

#include "access/genam.h"
#include "access/heapam.h"
#include "access/htup_details.h"
#include "access/parallel.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/pg_aggregate.h"
#include "catalog/pg_operator.h"
#include "catalog/pg_proc.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "access/xact.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "common/int.h"
#include "executor/executor.h"
#include "executor/nodeAgg.h"
#include "executor/nodeHash.h"
#include "executor/nodeSubplan.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/clauses.h"
#include "optimizer/optimizer.h"
#include "portability/instr_time.h"
#include "parser/parse_agg.h"
#include "storage/predicate.h"
#include "storage/proc.h"
#include "storage/shm_mq.h"
#include "storage/shm_toc.h"
#include "utils/acl.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/date.h"
#include "utils/datum.h"
#include "utils/fmgroids.h"
#include "utils/fmgrprotos.h"
#include "utils/expandeddatum.h"
#include "utils/float.h"
#include "utils/guc.h"
#include "utils/numeric.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"
#include "utils/snapmgr.h"
#include "utils/syscache.h"
#include "utils/timestamp.h"
#include "utils/tuplesort.h"
#include "utils/wait_event.h"
#include "utils/typcache.h"

#include "agg/agg.h"
#include "agg/wide.h"
#include "agg/back.h"
#include "agg/kern.h"
#include "agg/part.h"
#include "agg/sel.h"
#include "agg/spec.h"
#include "col/kern.h"
#include "agg/colsrc.h"
#include "agg/search.h"
#include "agg/topn.h"
#include "core/visibility.h"
#include "core/popcount.h"
#include "agg/agg_int.h"

/* bits [lo, hi) of a bit vector cleared */
static void
clear_bit_range(uint64 *bits, uint32 lo, uint32 hi)
{
	for (; lo < hi && (lo & 63); lo++)
		bits[lo >> 6] &= ~(UINT64CONST(1) << (lo & 63));
	for (; lo + 64 <= hi; lo += 64)
		bits[lo >> 6] = 0;
	for (; lo < hi; lo++)
		bits[lo >> 6] &= ~(UINT64CONST(1) << (lo & 63));
}

/* the element count of the unit and the offsets of its rows; the owner of every element is made when something reads it (unit_parent) */
static void
build_parents(AggExec *ex)
{
	ex->offs = elem_offs(ex, ex->spec->group);
	ex->nelem = ex->offs[ex->unit.nrow];
	ex->parent = NULL;
}

/* the row that owns each element of the array or object leaf `group` (the unnested one is unit_parent's) */
static const uint32 *
group_parent(AggExec *ex, int group)
{
	const uint32 *offs;
	uint32	   *par;

	if (group == ex->spec->group)
	{
		build_parents(ex);
		return unit_parent(ex);
	}
	offs = elem_offs(ex, group);
	par = MemoryContextAlloc(ex->ucxt, (Size) offs[ex->unit.nrow] * sizeof(uint32) + SIFT_AGG_PAD);
	sift_colk->expand_parents(offs + 1, ex->unit.nrow, par);
	return par;
}

/*
 * The conjuncts that evaluate an expression PostgreSQL may fail on (SiftAggPred.late) are applied to the items that passed every other
 * conjunct and the snapshot check, in the order of the query, each to the survivors of the ones before it: the normal plan never
 * evaluates an expression for a row that an earlier condition (a guard against a zero divisor) or its snapshot leaves out.
 */
static void
apply_late(AggExec *ex, uint64 *mask, int total, int qlevel)
{
	for (int i = 0; i < ex->spec->npred; i++)
	{
		SiftAggPred *p = ex->spec->pred[i];

		if (!p->late || p->level > qlevel)
			continue;
		for (int st = 0; st < total; st += VEC)
		{
			int			n = Min(VEC, total - st);
			uint64		t[VECW + PADW];

			ex->alive = mask + st / 64;
			eval_pred(ex, p, qlevel, st, n, t);
			ex->alive = NULL;
			sift_colk->bits_and(mask + st / 64, t, (n + 63) / 64);
		}
	}
}

/* A unit whose zone of the first sort leaf has nothing that sorts before the n-th best so far is not read */
static bool
topn_viable(AggExec *ex)
{
	SiftSrcZone z;

	if (!sift_topn_wants_zones(ex->topn))
		return true;
	sift_colsrc_zone(ex->src, &ex->unit, sift_topn_leaf(ex->topn, 0), &z);
	if (sift_topn_zone_viable(ex->topn, &z))
		return true;
	sift_topn_skip(ex->topn);
	return false;
}

/* the rows that pass go to the heap of the best n, by the sort leaves (agg/topn.h) */
static const SiftSrcVec *
topn_leaf_vec(void *ex, int leaf)
{
	const SiftSrcVec *v = get_vec(ex, leaf);

	if (v->dict)
		sift_srcdict_whole(v->dict);	/* it compares the entries as bytes and keeps their order */
	return v;
}

static void
topn_unit(AggExec *ex, const uint64 *rmask, int nrow)
{
	sift_topn_unit(ex->topn, ex->src, &ex->unit, NULL, rmask, nrow, topn_leaf_vec, ex);
}

/* *rows gets the rows of the unit that hold a value the index cannot store in leaf (a numeric NaN aside: the readers evaluate it). An element of a child leaf counts for its row. */
void
leaf_overflow_rows(AggExec *ex, int leaf, uint64 **rows)
{
	const SiftSrcLeaf *l = &ex->lay->leaf[leaf];
	const SiftSrcVec *v;
	const uint32 *par;

	if (l->typid == NUMERICOID || !sift_colsrc_overflow(ex->src, &ex->unit, leaf) || !(v = get_vec(ex, leaf))->ovf)
		return;
	if (!*rows)
		*rows = MemoryContextAllocZero(ex->ucxt, ((ex->unit.nrow + 63) / 64 + PADW) * sizeof(uint64));
	par = l->parent >= 0 ? group_parent(ex, l->parent) : NULL;
	for (int64 e = 0; e < v->n; e++)
		if (sift_colsrc_bit(v->ovf, e))
		{
			uint32		r = par ? par[e] : e;

			(*rows)[r >> 6] |= UINT64CONST(1) << (r & 63);
		}
}

/* *rows gets the rows of the unit whose member of the key of vleaf j holds a value the index cannot store: a value of another key is no one's concern */
static void
member_overflow_rows(AggExec *ex, int j, uint64 **rows)
{
	int			val = ex->vl[j].val;
	const SiftSrcVec *v;
	const uint32 *offs,
			   *par;
	uint64	   *eq;

	if (!sift_colsrc_overflow(ex->src, &ex->unit, val) || !(v = get_vec(ex, val))->ovf)
		return;
	eq = member_elems(ex, j, &offs);
	par = group_parent(ex, ex->lay->leaf[val].parent);
	if (!*rows)
		*rows = MemoryContextAllocZero(ex->ucxt, ((ex->unit.nrow + 63) / 64 + PADW) * sizeof(uint64));
	for (int64 e = 0; e < v->n; e++)
		if (sift_colsrc_bit(v->ovf, e) && sift_colsrc_bit(eq, e))
			(*rows)[par[e] >> 6] |= UINT64CONST(1) << (par[e] & 63);
}

/* does the source read leaf again, so that the member of one key of it (a leaf after the source's) is read from the rows it gives? */
static bool
leaf_rechecked(const SiftAggSpec *s, int leaf)
{
	for (int i = 0; i < s->nleaves; i++)
		if (s->leaves[i] == leaf)
			return s->recheck[i] != SRK_NONE;
	return false;
}

/* the rows of own that the FILTER of an aggregate of gate (bit j: agg[j]) keeps: the rows whose cast the aggregates evaluate */
static void
gate_rows(AggExec *ex, uint64 gate, uint64 *own)
{
	int			nrw = (ex->unit.nrow + 63) / 64;
	uint64	   *keep = MemoryContextAllocZero(ex->ucxt, (nrw + PADW) * sizeof(uint64));

	for (int j = 0; j < ex->spec->naggs; j++)
		if (gate >> j & 1)
			sift_colk->bits_or(keep, row_bitmap(ex, ex->spec->agg[j].filter), nrw);
	sift_colk->bits_and(own, keep, nrw);
}

/*
 * The rows of the unit that hold a value the index cannot store in a leaf the scan reads, NULL when there is none. The leaves after the source's
 * read their values from one of its leaves (the value column of a member of an object, the array of a length): theirs count too.
 */
static uint64 *
overflow_rows(AggExec *ex, bool rechecked, bool lists_only)
{
	SiftAggSpec *s = ex->spec;
	uint64	   *rows = NULL;

	for (int i = 0; i < s->nleaves; i++)
		if (s->leaves[i] < ex->nbase && s->recheck[i] != SRK_RECORDSET_LAZY && (s->recheck[i] != SRK_NONE) == rechecked &&
			(s->recheck[i] == SRK_RECORDSET ? lists_only || !ex->recheck_after_late : !lists_only))
		{
			uint64	   *own = NULL;

			if (!rechecked || !s->gate[i])
				leaf_overflow_rows(ex, s->leaves[i], &rows);
			else
			{
				leaf_overflow_rows(ex, s->leaves[i], &own);
				if (own)
				{
					gate_rows(ex, s->gate[i], own);
					if (!rows)
						rows = own;
					else
						sift_colk->bits_or(rows, own, (ex->unit.nrow + 63) / 64);
				}
			}
		}
	for (int j = 0; j < s->nvleaf && !lists_only; j++)
	{
		if (s->vleaf[j].types)
			continue;
		if (s->vleaf[j].count)
		{
			if (!rechecked)
				leaf_overflow_rows(ex, s->vleaf[j].obj, &rows);
		}
		else if ((s->vleaf[j].recheck != SRK_NONE) == rechecked && (rechecked || !leaf_rechecked(s, ex->vl[j].val)))
			member_overflow_rows(ex, j, &rows);
	}
	return rows;
}

/*
 * A unit may hold values the index cannot store (the writer's committed after the plan was made, in flight or rolled back, as the plan could
 * not know); the rows of another snapshot or none are not the scan's, a row it sees is: the normal plan reads it from the heap.
 * rechecked: the leaves the source reads again (a row of the stored unit that has such a value is not in its live rows: the unit that follows
 * has it, its values as SQL gives them) and what is left is the value SQL raises for. The source has the error of the row; it is raised when the
 * row reaches the expression, after the conjuncts of the rows, as SQL raises it.
 */
static pg_noinline void
check_overflow(AggExec *ex, const uint64 *live, bool rechecked)
{
	int			nrw = (ex->unit.nrow + 63) / 64;
	uint64	   *rows = overflow_rows(ex, rechecked, false);

	if (!rows)
		return;
	sift_colk->bits_and(rows, live, nrw);
	sift_colsrc_vet(ex->src, &ex->unit, rows);
	if (sift_colk->bits_count(rows, nrw) == 0)
		return;
	if (rechecked)
		sift_colsrc_overflow_raise(ex->src, &ex->unit, rows);
	sift_aggsel_overflow_error(RelationGetRelid(ex->heap));
}

/*
 * check_overflow of the lists a row reads again, for the rows the conjuncts that may raise (SiftAggPred.late) keep: PostgreSQL evaluates them
 * with the scan of the rows, before the call that reads the list, so a row they drop or raise for never raises the error of its list.
 */
static pg_noinline void
check_overflow_after_late(AggExec *ex, const uint64 *live)
{
	int			nrow = ex->unit.nrow,
				nrw = (nrow + 63) / 64;
	uint64	   *rows = overflow_rows(ex, true, true);

	if (!rows)
		return;
	sift_colk->bits_and(rows, live, nrw);
	sift_colsrc_vet(ex->src, &ex->unit, rows);
	if (sift_colk->bits_count(rows, nrw) == 0)
		return;
	apply_late(ex, rows, nrow, 0);
	if (sift_colk->bits_count(rows, nrw) == 0)
		return;
	sift_colsrc_overflow_raise(ex->src, &ex->unit, rows);
	sift_aggsel_overflow_error(RelationGetRelid(ex->heap));
}

/* check_overflow for the rows that keep an element of the unit, at the point the aggregates read them (SRK_CAST) */
static pg_noinline void
check_overflow_elems(AggExec *ex, const uint64 *em)
{
	int			nrw = (ex->unit.nrow + 63) / 64;
	uint64	   *rv = MemoryContextAllocZero(ex->ucxt, (nrw + PADW) * sizeof(uint64));

	rows_of_elems(ex->offs, ex->unit.nrow, em, rv);
	check_overflow(ex, rv, true);
}

/* the leaves are read for the rows that still count when they are fewer than a SPARSE_RATIOth of the unit: above about 3% the gather costs more than the decode it saves (1M rows, 6% kept: 66% more instructions at an eighth) */
#define SPARSE_RATIO	32
/* ... and the unit goes on as them alone only when they are fewer than a NARROW_RATIOth: above about 3% (ten keys of 300 parents) it costs what it saves */
#define NARROW_RATIO	32

/* the rows whose value has one of the JSON types of each SAP_TYPE conjunct; false when none is left */
static bool
type_filters(AggExec *ex, uint64 *rmask)
{
	for (int i = 0; i < ex->spec->npred; i++)
	{
		SiftAggPred *p = ex->spec->pred[i];

		if (p->kind == SAP_TYPE && !sift_colsrc_filter_types(ex->src, &ex->unit, p->leaf, p->op, rmask))
			return false;
	}
	return true;
}

/* the rows that pass each SAP_WIDE conjunct, in the order wide_init gave them to the source; false when none is left */
static bool
wide_filters(AggExec *ex, uint64 *rmask)
{
	int			k = 0;

	for (int i = 0; i < ex->spec->npred; i++)
		if (ex->spec->pred[i]->kind == SAP_WIDE && !sift_colsrc_filter_wide(ex->src, &ex->unit, k++, rmask))
			return false;
	return true;
}

/* what the scan derived from the rows of the unit */
static void
unit_forget(AggExec *ex)
{
	SiftAggSpec *s = ex->spec;

	memset(ex->uvec_ok, 0, ex->lay->nleaves * sizeof(bool));
	memset(ex->nvec_ok, 0, ex->lay->nleaves * sizeof(bool));
	memset(ex->gofs, 0, ex->lay->nleaves * sizeof(uint32 *));
	memset(ex->gpar, 0, ex->lay->nleaves * sizeof(uint32 *));
	for (int i = 0; i < s->npreds_total; i++)
	{
		ex->ps[i].rowmask_ok = false;
		ex->ps[i].poison_ok = false;
	}
}

static void
unit_begin(AggExec *ex)
{
	MemoryContextReset(ex->ucxt);
	unit_forget(ex);
	ex->n_units++;
}

/*
 * The source's filters, or the rows a point directory lists, left a few rows of the unit (a key that is spread over every unit of the table, in a lookup of one key): the unit goes on
 * as those rows alone, so that the rest of the work (the leaves, the offsets and owners of the elements of a list) costs what they do, not
 * what the unit does. rmask becomes all of them, nrow their number.
 */
static void
unit_narrow(AggExec *ex, uint64 **rmask, int *nrow)
{
	SiftSrcUnit *u = &ex->unit;

	if (!sift_colsrc_narrow(ex->src, u, *rmask))
		return;
	*nrow = u->nrow;
	*rmask = MemoryContextAllocZero(ex->ucxt, ((*nrow + 63) / 64 + PADW) * sizeof(uint64));
	fill_ones(*rmask, *nrow);
	unit_forget(ex);
}

/*
 * A row the source will read again from the heap (cand) stays unless a conjunct rejects it for a value the stored unit holds: a row that holds no
 * value of the conjunct's leaf (it cannot store it) is not tested by it. The conjuncts of other shapes leave the row to the heap.
 */
static void
cand_conjunct(AggExec *ex, uint64 *cand, SiftAggPred *p, const uint64 *pass, int nrw)
{
	uint64	   *ovf = NULL;

	if ((p->kind != SAP_CMP && p->kind != SAP_IN && p->kind != SAP_NULL) || p->leaf >= ex->nbase)
		return;
	leaf_overflow_rows(ex, p->leaf, &ovf);
	for (int w = 0; w < nrw; w++)
		cand[w] &= pass[w] | (ovf ? ovf[w] : 0);
}

/* The rows the source's filters left, narrowed by the row level conjuncts the source did not apply; cand: the rows the source reads again, narrowed the same way */
static void
unit_conjuncts(AggExec *ex, uint64 *rmask, uint64 *cand, int nrow)
{
	SiftAggSpec *s = ex->spec;
	SiftSrcUnit *u = &ex->unit;
	int			nrw = (nrow + 63) / 64;

	/* a few rows left: the leaves are decoded for them only (not with rows to read again: they are tested on whole leaves, and a selected read of an overflowed slot is an error) */
	if (!cand && (ex->nfilter > 0 || ex->ntype > 0 || ex->nwide > 0) && sift_colsrc_can_read_sel(ex->src) && sift_colk->bits_count(rmask, nrw) * SPARSE_RATIO < nrow)
		ex->rsel = rmask;
	/* row level conjuncts */
	for (int i = 0; i < s->npred; i++)
	{
		SiftAggPred *p = s->pred[i];
		SiftSrcStrTest *str = ex->ps[p->id].str;

		if (p->level != 0 || p->late || (ex->pushed && ex->pushed[i]))
			continue;
		if (str && sift_colsrc_filter_str(ex->src, u, str, rmask))
		{
			if (cand)
				sift_colsrc_filter_str(ex->src, u, str, cand);
			continue;
		}
		sift_colk->bits_and(rmask, row_bitmap(ex, p), nrw);
		if (cand)
			cand_conjunct(ex, cand, p, row_bitmap(ex, p), nrw);
	}
	ex->qlevel = s->group >= 0;
	if (ex->recheck_after_late && sift_colk->bits_count(rmask, nrw) > 0)
		check_overflow_after_late(ex, rmask);
	if (ex->recheck && !ex->recheck_late && sift_colk->bits_count(rmask, nrw) > 0)
		check_overflow(ex, rmask, true);
}

/* A unit of rows: the late conjuncts, then the chunks */
static void
unit_rows(AggExec *ex, uint64 *rmask, int nrow)
{
	sift_colsrc_vet(ex->src, &ex->unit, rmask);
	apply_late(ex, rmask, nrow, ex->qlevel);
	if (ex->recheck_late)
		check_overflow(ex, rmask, true);
	/* the aggregates of a top-N first: it reads the other sort keys of a few rows into the buffers of their leaves, which an aggregate over such a leaf reads whole */
	if (!ex->topn || ex->spec->naggs > 0)
		for (int st = 0; st < nrow; st += VEC)
			agg_chunk(ex, st, Min(VEC, nrow - st), rmask + st / 64);
	if (ex->topn)
		topn_unit(ex, rmask, nrow);
}

/* The elements of the kept rows that the conjuncts on the elements keep, as a bitmap over the elements of the unit */
static uint64 *
elems_kept(AggExec *ex, const uint64 *rmask, int nrow)
{
	SiftAggSpec *s = ex->spec;
	int			nrw = (nrow + 63) / 64,
				m = ex->nelem;
	uint64		cm[VECW + PADW];
	/* no row is out: so is no element, and the owners of the elements are not needed to tell */
	bool		allrows = sift_colk->bits_count(rmask, nrw) == nrow;
	uint64	   *em = MemoryContextAlloc(ex->ucxt, ((m + 63) / 64 + PADW) * sizeof(uint64));

	for (int st = 0; st < m; st += VEC)
	{
		int			n = Min(VEC, m - st),
					nw = (n + 63) / 64;

		if (allrows)
			fill_ones(cm, n);
		else
			sift_aggk->gather_bits(cm, rmask, unit_parent(ex) + st, n);
		for (int i = 0; i < s->npred; i++)
			if (s->pred[i]->level == 1 && !s->pred[i]->late)
			{
				uint64		t[VECW + PADW];

				eval_pred(ex, s->pred[i], 1, st, n, t);
				sift_colk->bits_and(cm, t, nw);
			}
		memcpy(em + st / 64, cm, nw * sizeof(uint64));
	}
	return em;
}

/* The rows that keep an element, checked against the snapshot: nothing to check in a unit whose pages are all visible */
static void
elems_vet(AggExec *ex, uint64 *em, int nrow)
{
	int			nrw = (nrow + 63) / 64;
	uint64	   *rv,
			   *seen;

	if (!sift_colsrc_vet_needed(ex->src, &ex->unit))
		return;
	rv = MemoryContextAllocZero(ex->ucxt, (nrw + PADW) * sizeof(uint64));
	seen = MemoryContextAlloc(ex->ucxt, (nrw + PADW) * sizeof(uint64));
	rows_of_elems(ex->offs, nrow, em, rv);
	memcpy(seen, rv, nrw * sizeof(uint64));
	sift_colsrc_vet(ex->src, &ex->unit, rv);
	/* the elements of a row the snapshot does not see go */
	for (int w = 0; w < nrw; w++)
		for (uint64 b = seen[w] & ~rv[w]; b; b &= b - 1)
		{
			int			r = w * 64 + pg_rightmost_one_pos64(b);

			clear_bit_range(em, ex->offs[r], ex->offs[r + 1]);
		}
}

/*
 * LEFT JOIN LATERAL .. ON true: a kept row none of whose elements passes the lateral's own conditions is one tuple with NULL element columns.
 * The rows are those of the row mask without a kept element, found before the snapshot check takes elements away: a row the snapshot does not
 * see is no row at all.
 */
static void
null_rows(AggExec *ex, const uint64 *rmask, const uint64 *em, int nrow)
{
	int			nrw = (nrow + 63) / 64;
	uint64	   *has = MemoryContextAlloc(ex->ucxt, (nrw + PADW) * sizeof(uint64));

	rows_of_elems(ex->offs, nrow, em, has);
	ex->nullrows = MemoryContextAllocZero(ex->ucxt, (nrw + PADW) * sizeof(uint64));
	for (int w = 0; w < nrw; w++)
		ex->nullrows[w] = rmask[w] & ~has[w];
	if (sift_colk->bits_count(ex->nullrows, nrw) == 0)
		ex->nullrows = NULL;
}

/* The tuples of the rows with no element: the rows themselves, their element columns NULL */
static void
unit_null_tuples(AggExec *ex, int nrow)
{
	uint64	   *nm = ex->nullrows;

	ex->nullrows = NULL;
	sift_colsrc_vet(ex->src, &ex->unit, nm);
	ex->nullpass = true;
	ex->qlevel = 0;
	apply_late(ex, nm, nrow, 0);
	for (int st = 0; st < nrow; st += VEC)
		agg_chunk(ex, st, Min(VEC, nrow - st), nm + st / 64);
	ex->nullpass = false;
	ex->qlevel = 1;
}

/* A unit of elements: the elements of the kept rows, vetted, the late conjuncts, then the chunks */
static void
unit_elems(AggExec *ex, uint64 *rmask, int nrow)
{
	uint64	   *em;
	int			m;

	build_parents(ex);
	m = ex->nelem;
	em = elems_kept(ex, rmask, nrow);
	if (ex->spec->left_lateral)
		null_rows(ex, rmask, em, nrow);
	elems_vet(ex, em, nrow);
	apply_late(ex, em, m, ex->qlevel);
	if (ex->recheck_late)
		check_overflow_elems(ex, em);
	for (int st = 0; st < m; st += VEC)
		agg_chunk(ex, st, Min(VEC, m - st), em + st / 64);
	if (ex->nullrows)
		unit_null_tuples(ex, nrow);
}

void
process_unit(AggExec *ex)
{
	SiftSrcUnit *u = &ex->unit;
	int			nrow = u->nrow;
	uint64	   *rmask,
			   *cand;

	unit_begin(ex);
	if (nrow > 0 && ex->pa_on && !ex->spec->left_lateral && preagg_unit(ex))
	{
		/* a superunit stands for its units: they count as the units they are */
		ex->n_units += Max(u->nsu, 1) - 1;
		ex->n_super += u->nsu > 0;
		ex->n_pre_units += Max(u->nsu, 1);
		ex->row_base += nrow;
		CHECK_FOR_INTERRUPTS();
		return;
	}
	if (u->nsu > 0)
	{
		/* its summary cannot answer here (a predicate that cuts through the group, a dim or a measure it lacks): the units of the group come one by one */
		sift_colsrc_split(ex->src, u);
		ex->n_units--;
		if (ex->n_super == 0 && ++ex->n_split >= 4)
			sift_colsrc_super(ex->src, false);
		return;
	}
	ex->n_rows += nrow;
	if (nrow == 0)
		return;
	if (!ex->spec->left_lateral && sift_aggbe_run_unit(ex->be, ex->be_state, &ex->unit))
	{
		ex->row_base += nrow;
		CHECK_FOR_INTERRUPTS();
		return;
	}
	if (ex->topn && !topn_viable(ex))
		return;
	rmask = MemoryContextAlloc(ex->ucxt, ((nrow + 63) / 64 + PADW) * sizeof(uint64));
	ex->rsel = NULL;
	sift_colsrc_live(ex->src, u, rmask);
	check_overflow(ex, rmask, false);
	cand = ex->recheck ? sift_colsrc_recheck_rows(ex->src, u) : NULL;
	/* a row the source will read again is tested by the filters on the columns of the stored unit, as the rows that stay; the type tests leave it to the heap */
	if (cand && ((ex->nwide > 0 && !wide_filters(ex, cand)) || (ex->nfilter > 0 && !sift_colsrc_filter(ex->src, u, cand))))
		cand = NULL;
	if ((ex->ntype > 0 && !type_filters(ex, rmask)) || (ex->nwide > 0 && !wide_filters(ex, rmask)) ||
		(ex->nfilter > 0 && !sift_colsrc_filter(ex->src, u, rmask)))
	{
		if (cand)
			unit_conjuncts(ex, rmask, cand, nrow);
		ex->row_base += nrow;
		return;
	}
	if (!cand && (ex->nfilter > 0 || ex->ntype > 0 || ex->nwide > 0 || ex->spec->npred > 0) && sift_colsrc_can_narrow(ex->src) &&
		sift_colk->bits_count(rmask, (nrow + 63) / 64) * NARROW_RATIO < nrow)
		unit_narrow(ex, &rmask, &nrow);
	unit_conjuncts(ex, rmask, cand, nrow);
	if (ex->spec->group >= 0)
		unit_elems(ex, rmask, nrow);
	else
		unit_rows(ex, rmask, nrow);
	ex->row_base += nrow;
	CHECK_FOR_INTERRUPTS();
}
