/*
 * agg/order/rank.c - a Sort of many rows whose keys have few distinct values, sorted by the ranks of the values
 *
 * The tail of an analytics statement (a window over the groups of two scans, an ORDER BY of a join of them) sorts tens of thousands of rows by keys that are a handful of
 * distinct values each: the text of a category, the id of an entity, a month, a flag. The Sort of PostgreSQL calls the comparator of every key for every compare,
 * n log n times. This node reads the rows of the child, gives each distinct value of a key (found by its bytes) an id, sorts the distinct values once with the comparator
 * of the key (the sort operator, collation and nulls order the Sort would have used), and sorts the rows by the packed ranks with a radix sort (agg/gorder.c): integer
 * work per row, whatever the types. The plan has the node where the finished plan of a statement that runs the scan had a Sort; the rows and their order are the Sort's,
 * rows that tie come in the order they were read.
 *
 * What the Sort does that the node does not do, it leaves to the Sort: a Sort under a Limit stays (the bounded heapsort), so does one under a Gather. A node that finds
 * its input too small, its keys too varied or its rows more than work_mem hands what it read to a tuplesort (the Sort's own) and goes on with that.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "catalog/pg_type_d.h"
#include "commands/explain.h"
#include "common/hashfn.h"
#include "executor/executor.h"
#include "miscadmin.h"
#include "nodes/extensible.h"
#include "nodes/makefuncs.h"
#include "nodes/nodeFuncs.h"
#include "nodes/plannodes.h"
#include "parser/parsetree.h"
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/ruleutils.h"
#include "utils/sortsupport.h"
#include "utils/tuplesort.h"

#include "agg/agg.h"
#include "agg/agg_int.h"

/* the Sort stays below this many rows expected */
#define RANK_PLAN_ROWS 2048
/* ... and a node that reads fewer rows hands them to the tuplesort */
#define RANK_MIN_ROWS 1024
/* a key with more than a fourth of the rows distinct, seen after this many rows or at the end, is not a key to rank */
#define RANK_SAMPLE 4096
/* ... and one that has read this many rows hands them to the tuplesort: the ranks are kept in memory */
#define RANK_MAX_ROWS 40000000
/* the rows come out in an order that is not the order in memory: the next ones are fetched ahead */
#define RANK_AHEAD 8

typedef struct RankKey
{
	AttrNumber	att;
	Oid			sortop;
	Oid			collation;
	bool		nulls_first;
	int16		typlen;
	bool		byval;
	SortSupportData ssup;
	uint32	   *slot;			/* open addressing: id + 1 of the value, 0 free */
	uint32		mask;
	Datum	   *val;			/* by id - 1 */
	uint32	   *hash;
	uint32		nd;
	uint32		cap;
} RankKey;

typedef struct RankExec
{
	CustomScanState css;
	int			nkeys;
	RankKey    *key;
	MemoryContext pcxt;			/* what lives as long as the node */
	MemoryContext cxt;			/* the rows and their keys of one pass */
	MemoryContext rowcxt;		/* one row */
	MinimalTuple *tup;
	uint32	   *ids;			/* nkeys ids a row */
	uint64		nrows;
	uint64		cap;
	Size		bytes;
	uint32	   *perm;
	bool		done;			/* the rows are sorted */
	Tuplesortstate *ts;			/* the tuplesort they went to */
	const char *why;			/* ... and why */
	bool		random;			/* backward, mark and restore: the plan needs a sort that can */
	uint64		current;
	bool		eof;
	uint64		mark;
	bool		mark_eof;
	Size		mem;
} RankExec;

/* ---- the keys: the distinct values of a column by their bytes ---- */

static bool
rank_key_ok(Oid typid)
{
	int16		len;
	bool		byval;

	get_typlenbyval(typid, &len, &byval);
	return len != -2 && (byval ? len >= 1 && len <= (int16) sizeof(Datum) : true);
}

static void
rank_key_init(RankExec *ex, RankKey *k, TupleDesc desc, AttrNumber att, Oid sortop, Oid collation, bool nulls_first)
{
	Oid			typid = TupleDescAttr(desc, att - 1)->atttypid;

	k->att = att;
	k->sortop = sortop;
	k->collation = collation;
	k->nulls_first = nulls_first;
	get_typlenbyval(typid, &k->typlen, &k->byval);
	k->ssup.ssup_cxt = ex->pcxt;
	k->ssup.ssup_collation = collation;
	k->ssup.ssup_nulls_first = nulls_first;
	PrepareSortSupportFromOrderingOp(sortop, &k->ssup);
}

static void
rank_key_reset(RankKey *k)
{
	k->slot = NULL;
	k->val = NULL;
	k->hash = NULL;
	k->nd = 0;
	k->cap = 0;
	k->mask = 0;
}

static inline Datum
rank_mask(const RankKey *k, Datum d)
{
	switch (k->typlen)
	{
		case 1:
			return (Datum) (uint8) d;
		case 2:
			return (Datum) (uint16) d;
		case 4:
			return (Datum) (uint32) d;
		default:
			return d;
	}
}

static bool
rank_same(const RankKey *k, Datum a, Datum b)
{
	if (k->byval)
		return a == b;
	if (k->typlen == -1)
	{
		Size		la = VARSIZE_ANY_EXHDR(DatumGetPointer(a));

		return la == VARSIZE_ANY_EXHDR(DatumGetPointer(b)) && memcmp(VARDATA_ANY(DatumGetPointer(a)), VARDATA_ANY(DatumGetPointer(b)), la) == 0;
	}
	return memcmp(DatumGetPointer(a), DatumGetPointer(b), k->typlen) == 0;
}

static uint32
rank_hash(const RankKey *k, Datum d)
{
	if (k->byval)
		return hash_bytes_uint32((uint32) d ^ (uint32) (d >> 32));
	if (k->typlen == -1)
		return hash_bytes((const unsigned char *) VARDATA_ANY(DatumGetPointer(d)), (int) VARSIZE_ANY_EXHDR(DatumGetPointer(d)));
	return hash_bytes((const unsigned char *) DatumGetPointer(d), k->typlen);
}

static void
rank_key_grow(RankExec *ex, RankKey *k)
{
	uint32		ncap = k->cap ? k->cap * 2 : 64;
	uint32		nmask = ncap * 2 - 1;
	MemoryContext old = MemoryContextSwitchTo(ex->cxt);
	uint32	   *nslot = palloc0((Size) (nmask + 1) * sizeof(uint32));

	SIFT_REGROW(k->val, ncap * sizeof(Datum));
	SIFT_REGROW(k->hash, ncap * sizeof(uint32));
	for (uint32 i = 0; i < k->nd; i++)
	{
		uint32		at = k->hash[i] & nmask;

		while (nslot[at] != 0)
			at = (at + 1) & nmask;
		nslot[at] = i + 1;
	}
	if (k->slot)
		pfree(k->slot);
	k->slot = nslot;
	k->mask = nmask;
	k->cap = ncap;
	MemoryContextSwitchTo(old);
}

/* the id (from 1) of a value; a value met for the first time is kept */
static uint32
rank_intern(RankExec *ex, RankKey *k, Datum d)
{
	uint32		h,
				at;

	if (k->byval)
		d = rank_mask(k, d);
	else if (k->typlen == -1 && VARATT_IS_EXTENDED(DatumGetPointer(d)))
		d = PointerGetDatum(pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d)));
	h = rank_hash(k, d);
	if (k->nd * 2 >= k->cap)
		rank_key_grow(ex, k);
	for (at = h & k->mask; k->slot[at] != 0; at = (at + 1) & k->mask)
	{
		uint32		i = k->slot[at] - 1;

		if (k->hash[i] == h && rank_same(k, k->val[i], d))
			return i + 1;
	}
	if (!k->byval)
	{
		Size		len = k->typlen == -1 ? VARSIZE_ANY(DatumGetPointer(d)) : k->typlen;
		void	   *copy = MemoryContextAlloc(ex->cxt, len);

		memcpy(copy, DatumGetPointer(d), len);
		d = PointerGetDatum(copy);
	}
	k->val[k->nd] = d;
	k->hash[k->nd] = h;
	k->slot[at] = ++k->nd;
	return k->nd;
}

/* the rank of each id: the distinct values sorted by the comparator of the key, values it calls equal one rank, NULL before them or after */
static uint32 *
rank_of(RankKey *k)
{
	IdVal	   *vals = palloc((Size) k->nd * sizeof(IdVal) + sizeof(IdVal));
	uint32	   *rank = palloc0(((Size) k->nd + 1) * sizeof(uint32));
	uint32		r = 0;

	for (uint32 i = 0; i < k->nd; i++)
	{
		vals[i].id = i + 1;
		vals[i].val = k->val[i];
	}
	sift_idval_sort(vals, k->nd, &k->ssup);
	for (uint32 i = 0; i < k->nd; i++)
	{
		if (i == 0 || ApplySortComparator(vals[i - 1].val, false, vals[i].val, false, &k->ssup) != 0)
			r++;
		rank[vals[i].id] = r;
	}
	rank[0] = k->nulls_first ? 0 : r + 1;
	pfree(vals);
	return rank;
}

/* ---- the rows ---- */

static void
rank_free(RankExec *ex)
{
	MemoryContextReset(ex->cxt);
	ex->tup = NULL;
	ex->ids = NULL;
	ex->perm = NULL;
	ex->nrows = 0;
	ex->cap = 0;
	ex->bytes = 0;
	for (int i = 0; i < ex->nkeys; i++)
		rank_key_reset(&ex->key[i]);
}

static void
rank_to_tuplesort(RankExec *ex, const char *why)
{
	TupleTableSlot *slot = ex->css.ss.ss_ScanTupleSlot;
	AttrNumber *att = palloc(ex->nkeys * sizeof(AttrNumber));
	Oid		   *op = palloc(ex->nkeys * sizeof(Oid));
	Oid		   *coll = palloc(ex->nkeys * sizeof(Oid));
	bool	   *nf = palloc(ex->nkeys * sizeof(bool));
	MemoryContext old = MemoryContextSwitchTo(ex->pcxt);

	for (int i = 0; i < ex->nkeys; i++)
	{
		att[i] = ex->key[i].att;
		op[i] = ex->key[i].sortop;
		coll[i] = ex->key[i].collation;
		nf[i] = ex->key[i].nulls_first;
	}
	ex->ts = tuplesort_begin_heap(slot->tts_tupleDescriptor, ex->nkeys, att, op, coll, nf, work_mem, NULL, ex->random ? TUPLESORT_RANDOMACCESS : TUPLESORT_NONE);
	MemoryContextSwitchTo(old);
	ex->why = why;
	for (uint64 i = 0; i < ex->nrows; i++)
	{
		ExecStoreMinimalTuple(ex->tup[i], slot, false);
		tuplesort_puttupleslot(ex->ts, slot);
	}
	ExecClearTuple(slot);
	rank_free(ex);
}

static bool
rank_varied(const RankExec *ex)
{
	for (int i = 0; i < ex->nkeys; i++)
		if ((uint64) ex->key[i].nd * 4 > ex->nrows)
			return true;
	return false;
}

/* read the child, sort what it gave */
static void
rank_fill(RankExec *ex)
{
	PlanState  *child = linitial(ex->css.custom_ps);
	int			maxatt = 0;

	for (int i = 0; i < ex->nkeys; i++)
		maxatt = Max(maxatt, ex->key[i].att);
	for (;;)
	{
		TupleTableSlot *in;
		MemoryContext old;

		in = ExecProcNode(child);
		if (TupIsNull(in))
			break;
		if (ex->ts)
		{
			tuplesort_puttupleslot(ex->ts, in);
			continue;
		}
		slot_getsomeattrs(in, maxatt);
		old = MemoryContextSwitchTo(ex->cxt);
		if (ex->nrows == ex->cap)
		{
			uint64		ncap = ex->cap ? ex->cap * 2 : 1024;

			SIFT_REGROW(ex->tup, ncap * sizeof(MinimalTuple));
			SIFT_REGROW(ex->ids, ncap * ex->nkeys * sizeof(uint32));
			ex->cap = ncap;
		}
		ex->tup[ex->nrows] = ExecCopySlotMinimalTuple(in);
		ex->bytes += MAXALIGN(ex->tup[ex->nrows]->t_len) + sizeof(MinimalTuple) + ex->nkeys * (3 * sizeof(uint32));
		MemoryContextSwitchTo(ex->rowcxt);
		for (int i = 0; i < ex->nkeys; i++)
		{
			RankKey    *k = &ex->key[i];

			ex->ids[ex->nrows * ex->nkeys + i] = in->tts_isnull[k->att - 1] ? 0 : rank_intern(ex, k, in->tts_values[k->att - 1]);
		}
		ex->nrows++;
		MemoryContextSwitchTo(old);
		MemoryContextReset(ex->rowcxt);
		if (ex->bytes > (Size) work_mem * 1024L || ex->nrows >= RANK_MAX_ROWS)
			rank_to_tuplesort(ex, "memory");
		else if (ex->nrows == RANK_SAMPLE && rank_varied(ex))
			rank_to_tuplesort(ex, "distinct values");
	}
	if (ex->ts == NULL && ex->nrows < RANK_MIN_ROWS)
		rank_to_tuplesort(ex, "rows");
	else if (ex->ts == NULL && rank_varied(ex))
		rank_to_tuplesort(ex, "distinct values");
	if (ex->ts)
	{
		tuplesort_performsort(ex->ts);
		ex->done = true;
		return;
	}
	{
		MemoryContext old = MemoryContextSwitchTo(ex->cxt);
		uint32	   *tup = palloc((Size) ex->nrows * ex->nkeys * sizeof(uint32));

		for (int i = 0; i < ex->nkeys; i++)
		{
			uint32	   *rank = rank_of(&ex->key[i]);

			for (uint64 r = 0; r < ex->nrows; r++)
				tup[r * ex->nkeys + i] = rank[ex->ids[r * ex->nkeys + i]];
			pfree(rank);
		}
		ex->perm = sift_rank_perm(tup, (uint32) ex->nrows, ex->nkeys);
		pfree(tup);
		pfree(ex->ids);
		ex->ids = NULL;
		MemoryContextSwitchTo(old);
	}
	for (int i = 0; i < ex->nkeys; i++)
		ex->mem += (Size) ex->key[i].cap * (sizeof(Datum) + sizeof(uint32)) + (Size) (ex->key[i].mask + 1) * sizeof(uint32);
	ex->mem += ex->bytes + ex->nrows * sizeof(uint32);
	ex->done = true;
	ex->current = 0;
	ex->eof = false;
}

static TupleTableSlot *
rank_exec(CustomScanState *node)
{
	RankExec   *ex = (RankExec *) node;
	TupleTableSlot *slot = node->ss.ss_ScanTupleSlot;
	bool		forward = ScanDirectionIsForward(node->ss.ps.state->es_direction);

	if (!ex->done)
		rank_fill(ex);
	if (ex->ts)
	{
		(void) tuplesort_gettupleslot(ex->ts, forward, false, slot, NULL);
		return slot;
	}
	ExecClearTuple(slot);
	/* what tuplesort does at the ends: backward after the last row gives the last, else the one before the last given */
	if (forward)
	{
		if (ex->current >= ex->nrows)
		{
			ex->eof = true;
			return slot;
		}
		if (ex->current + RANK_AHEAD < ex->nrows)
			__builtin_prefetch(ex->tup[ex->perm[ex->current + RANK_AHEAD]]);
		ExecStoreMinimalTuple(ex->tup[ex->perm[ex->current++]], slot, false);
		return slot;
	}
	if (ex->current == 0)
		return slot;
	if (ex->eof)
		ex->eof = false;
	else
	{
		ex->current--;
		if (ex->current == 0)
			return slot;
	}
	ExecStoreMinimalTuple(ex->tup[ex->perm[ex->current - 1]], slot, false);
	return slot;
}

static void
rank_begin(CustomScanState *node, EState *estate, int eflags)
{
	RankExec   *ex = (RankExec *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	TupleDesc	desc = node->ss.ss_ScanTupleSlot->tts_tupleDescriptor;
	List	   *att = linitial(cs->custom_private),
			   *op = lsecond(cs->custom_private),
			   *coll = lthird(cs->custom_private),
			   *nf = lfourth(cs->custom_private);

	ex->pcxt = CurrentMemoryContext;
	ex->cxt = AllocSetContextCreate(ex->pcxt, "sift rank sort", ALLOCSET_DEFAULT_SIZES);
	ex->rowcxt = AllocSetContextCreate(ex->pcxt, "sift rank sort row", ALLOCSET_SMALL_SIZES);
	ex->nkeys = list_length(att);
	ex->key = palloc0(ex->nkeys * sizeof(RankKey));
	ex->random = (eflags & (EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)) != 0;
	/* the child is read once, forward, whatever the parent asks of the node */
	foreach_ptr(Plan, plan, cs->custom_plans)
		node->custom_ps = lappend(node->custom_ps, ExecInitNode(plan, estate, eflags & ~(EXEC_FLAG_REWIND | EXEC_FLAG_BACKWARD | EXEC_FLAG_MARK)));
	for (int i = 0; i < ex->nkeys; i++)
		rank_key_init(ex, &ex->key[i], desc, (AttrNumber) list_nth_int(att, i), list_nth_oid(op, i), list_nth_oid(coll, i), list_nth_int(nf, i) != 0);
}

static void
rank_end(CustomScanState *node)
{
	RankExec   *ex = (RankExec *) node;

	if (ex->ts)
		tuplesort_end(ex->ts);
	foreach_ptr(PlanState, child, node->custom_ps)
		ExecEndNode(child);
	if (ex->cxt)
		MemoryContextDelete(ex->cxt);
	if (ex->rowcxt)
		MemoryContextDelete(ex->rowcxt);
}

static void
rank_rescan(CustomScanState *node)
{
	RankExec   *ex = (RankExec *) node;
	PlanState  *child = linitial(node->custom_ps);

	/* the same rows, again: only a sort that can go back to the start can, and only when the child's rows are the same */
	if (ex->done && child->chgParam == NULL && ex->random)
	{
		if (ex->ts)
			tuplesort_rescan(ex->ts);
		else
		{
			ex->current = 0;
			ex->eof = false;
		}
		return;
	}
	if (ex->ts)
	{
		tuplesort_end(ex->ts);
		ex->ts = NULL;
	}
	rank_free(ex);
	ex->done = false;
	ex->why = NULL;
	ex->mem = 0;
	if (child->chgParam == NULL || node->ss.ps.chgParam != NULL)
		UpdateChangedParamSet(child, node->ss.ps.chgParam);
	if (child->chgParam == NULL)
		ExecReScan(child);
}

static void
rank_mark(CustomScanState *node)
{
	RankExec   *ex = (RankExec *) node;

	if (ex->ts)
		tuplesort_markpos(ex->ts);
	else
	{
		ex->mark = ex->current;
		ex->mark_eof = ex->eof;
	}
}

static void
rank_restore(CustomScanState *node)
{
	RankExec   *ex = (RankExec *) node;

	if (ex->ts)
		tuplesort_restorepos(ex->ts);
	else
	{
		ex->current = ex->mark;
		ex->eof = ex->mark_eof;
	}
}

static void
rank_explain(CustomScanState *node, List *ancestors, ExplainState *es)
{
	RankExec   *ex = (RankExec *) node;
	CustomScan *cs = (CustomScan *) node->ss.ps.plan;
	Plan	   *child = linitial(cs->custom_plans);
	List	   *att = linitial(cs->custom_private),
			   *coll = lthird(cs->custom_private),
			   *nf = lfourth(cs->custom_private);
	List	   *context = set_deparse_context_plan(es->deparse_cxt, child, lcons(cs, ancestors));
	bool		useprefix = list_length(es->rtable) > 1 || es->verbose;
	List	   *keys = NIL;
	StringInfoData buf;

	for (int i = 0; i < list_length(att); i++)
	{
		TargetEntry *tle = get_tle_by_resno(child->targetlist, list_nth_int(att, i));
		char	   *name = deparse_expression((Node *) tle->expr, context, useprefix, true);

		initStringInfo(&buf);
		appendStringInfoString(&buf, name);
		if (OidIsValid(list_nth_oid(coll, i)) && list_nth_oid(coll, i) != exprCollation((Node *) tle->expr))
			appendStringInfo(&buf, " COLLATE %s", generate_collation_name(list_nth_oid(coll, i)));
		if (ex->key[i].ssup.ssup_reverse)
			appendStringInfoString(&buf, " DESC");
		if (list_nth_int(nf, i) != 0)
			appendStringInfoString(&buf, " NULLS FIRST");
		keys = lappend(keys, buf.data);
	}
	ExplainPropertyList("Sort Key", keys, es);
	if (es->analyze && ex->done)
	{
		if (ex->ts)
			ExplainPropertyText("Sort Method", psprintf("tuplesort (%s)", ex->why), es);
		else
		{
			initStringInfo(&buf);
			for (int i = 0; i < ex->nkeys; i++)
				appendStringInfo(&buf, "%s%u", i ? ", " : "", ex->key[i].nd);
			ExplainPropertyText("Sort Method", "rank", es);
			ExplainPropertyText("Distinct Values", buf.data, es);
			ExplainPropertyInteger("Sort Memory", "kB", (int64) ((ex->mem + 1023) / 1024), es);
		}
	}
}

static const CustomExecMethods rank_exec_methods = {
	.CustomName = "SiftSort",
	.BeginCustomScan = rank_begin,
	.ExecCustomScan = rank_exec,
	.EndCustomScan = rank_end,
	.ReScanCustomScan = rank_rescan,
	.MarkPosCustomScan = rank_mark,
	.RestrPosCustomScan = rank_restore,
	.ExplainCustomScan = rank_explain,
};

static Node *
rank_create_state(CustomScan *cscan)
{
	RankExec   *ex = (RankExec *) newNode(sizeof(RankExec), T_CustomScanState);

	ex->css.methods = &rank_exec_methods;
	ex->css.flags = cscan->flags;
	ex->css.slotOps = &TTSOpsMinimalTuple;
	return (Node *) ex;
}

const CustomScanMethods sift_agg_rank_plan_methods = {"SiftSort", rank_create_state};

/* ---- the plan ---- */

/* the sort support of the key compares the datum as an integer: one such key is sorted by the Sort at about the price of the node's own work */
static bool
int_compare(Oid sortop, Oid collation)
{
	SortSupportData ssup = {0};

	ssup.ssup_cxt = CurrentMemoryContext;
	ssup.ssup_collation = collation;
	PrepareSortSupportFromOrderingOp(sortop, &ssup);
	return ssup.comparator == ssup_datum_signed_cmp || ssup.comparator == ssup_datum_int32_cmp || ssup.comparator == ssup_datum_unsigned_cmp;
}

static bool
sort_keys_ok(Sort *srt, Plan *child)
{
	if (srt->numCols < 1 || srt->numCols > SIFT_AGG_MAX_KEYS)
		return false;
	for (int i = 0; i < srt->numCols; i++)
	{
		TargetEntry *tle = get_tle_by_resno(child->targetlist, srt->sortColIdx[i]);

		if (tle == NULL || !rank_key_ok(exprType((Node *) tle->expr)) || !OidIsValid(srt->sortOperators[i]))
			return false;
	}
	return srt->numCols > 1 || !int_compare(srt->sortOperators[0], srt->collations[0]);
}

static Plan *
rank_node(Sort *srt)
{
	Plan	   *child = srt->plan.lefttree;
	CustomScan *cs = makeNode(CustomScan);
	List	   *att = NIL,
			   *op = NIL,
			   *coll = NIL,
			   *nf = NIL,
			   *tlist = NIL,
			   *scan_tlist = NIL;
	int			resno = 1;

	for (int i = 0; i < srt->numCols; i++)
	{
		att = lappend_int(att, srt->sortColIdx[i]);
		op = lappend_oid(op, srt->sortOperators[i]);
		coll = lappend_oid(coll, srt->collations[i]);
		nf = lappend_int(nf, srt->nullsFirst[i]);
	}
	cs->scan.plan.startup_cost = srt->plan.startup_cost;
	cs->scan.plan.total_cost = srt->plan.total_cost;
	cs->scan.plan.plan_rows = srt->plan.plan_rows;
	cs->scan.plan.plan_width = srt->plan.plan_width;
	cs->scan.plan.parallel_safe = srt->plan.parallel_safe;
	cs->scan.plan.plan_node_id = srt->plan.plan_node_id;
	cs->scan.plan.initPlan = srt->plan.initPlan;
	cs->scan.plan.extParam = srt->plan.extParam;
	cs->scan.plan.allParam = srt->plan.allParam;
	cs->scan.scanrelid = 0;
	cs->flags = CUSTOMPATH_SUPPORT_BACKWARD_SCAN | CUSTOMPATH_SUPPORT_MARK_RESTORE;
	cs->custom_plans = list_make1(child);
	/*
	 * The columns of the child, as a Sort has them (OUTER_VAR): the child is also the node's lefttree, which only EXPLAIN reads (the deparse of the
	 * columns of the plans above goes through the node to the child), and the executor starts the custom plan alone
	 */
	foreach_node(TargetEntry, ctle, child->targetlist)
	{
		Node	   *e = (Node *) ctle->expr;
		Oid			typ = exprType(e);
		int32		mod = exprTypmod(e);
		Oid			col = exprCollation(e);
		/* the labels (name, junk, sort group) are the Sort's: the planner sets them on the node it hands over, and the entries of the child do not always carry them (PostgreSQL 17 returned the two sort columns of a UNION ALL as results) */
		TargetEntry *tle = get_tle_by_resno(srt->plan.targetlist, resno);
		TargetEntry *t;

		if (tle == NULL)
			tle = ctle;
		t = makeTargetEntry((Expr *) makeVar(INDEX_VAR, resno, typ, mod, col, 0), resno, tle->resname, tle->resjunk);
		t->ressortgroupref = tle->ressortgroupref;
		tlist = lappend(tlist, t);
		scan_tlist = lappend(scan_tlist, makeTargetEntry((Expr *) makeVar(OUTER_VAR, resno, typ, mod, col, 0), resno, tle->resname, tle->resjunk));
		resno++;
	}
	cs->custom_scan_tlist = scan_tlist;
	cs->scan.plan.lefttree = child;
	cs->scan.plan.targetlist = tlist;
	cs->custom_private = list_make4(att, op, coll, nf);
	cs->methods = &sift_agg_rank_plan_methods;
	return &cs->scan.plan;
}

/* the nodes ExecSetTupleBound passes a Limit's bound through: a Sort below them sorts for the first rows only */
static bool
passes_bound(Plan *p)
{
	return IsA(p, SubqueryScan) || IsA(p, Result) || IsA(p, Gather) || IsA(p, GatherMerge);
}

static Plan *rank_walk(Plan *p, bool bound, bool parallel);

static void
rank_walk_list(List *plans, bool bound, bool parallel)
{
	ListCell   *lc;

	foreach(lc, plans)
		lfirst(lc) = rank_walk(lfirst(lc), bound, parallel);
}

static Plan *
rank_walk(Plan *p, bool bound, bool parallel)
{
	bool		below;

	if (p == NULL)
		return NULL;
	below = IsA(p, Limit) || (bound && passes_bound(p));
	parallel = parallel || IsA(p, Gather) || IsA(p, GatherMerge);
	p->lefttree = rank_walk(p->lefttree, below, parallel);
	p->righttree = rank_walk(p->righttree, false, parallel);
	switch (nodeTag(p))
	{
		case T_SubqueryScan:
			((SubqueryScan *) p)->subplan = rank_walk(((SubqueryScan *) p)->subplan, below, parallel);
			break;
		case T_Append:
			rank_walk_list(((Append *) p)->appendplans, false, parallel);
			break;
		case T_MergeAppend:
			rank_walk_list(((MergeAppend *) p)->mergeplans, false, parallel);
			break;
		case T_CustomScan:
			rank_walk_list(((CustomScan *) p)->custom_plans, false, parallel);
			break;
		case T_Sort:
			if (!bound && !parallel && p->plan_rows >= RANK_PLAN_ROWS && p->lefttree != NULL && sort_keys_ok((Sort *) p, p->lefttree))
				return rank_node((Sort *) p);
			break;
		default:
			break;
	}
	return p;
}

/* a Sort of many rows in the finished plan of a statement that runs the scan, by keys of few distinct values, is this node */
void
sift_agg_rank_sorts(PlannedStmt *stmt)
{
	stmt->planTree = rank_walk(stmt->planTree, false, false);
	rank_walk_list(stmt->subplans, false, false);
}
