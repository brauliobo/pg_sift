/*
 * agg/part.c - the partitions of a partitioned table as one column source (agg/part.h, docs/AGG.md "Partitioned tables").
 *
 * The wrapper opens the source of every partition (sift_colsrc_open_part) and is a provider of its own behind the seam of agg/colsrc.h. Its
 * units are those of the partitions one after the other: begin cuts the range of superblocks it is given into the ranges of the partitions it
 * overlaps (the superblocks of the partitions are numbered one after the other) and starts them one at a time, and every call about the
 * unit the source is at goes to the partition that returned it. Nothing is merged: the dictionaries of the units are their own already, and the
 * executor translates by value. The one thing the wrapper changes of what a partition says is the id of a dictionary (SiftSrcDict.id): a
 * partition numbers its dictionaries by itself, so two of them can hold the same id for different dictionaries.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/table.h"
#include "miscadmin.h"
#include "nodes/nodeFuncs.h"
#include "optimizer/optimizer.h"
#include "utils/acl.h"
#include "utils/lsyscache.h"
#include "utils/partcache.h"
#include "utils/rel.h"
#include "utils/rls.h"

#include "agg/part.h"

#define PART_DICT_ID_BASE	UINT64CONST(0x6000000000000000)	/* no id of a provider has these bits: the engine's counters and pointers are below, its flags above */

/* the id the wrapper gave the dictionary of a slot (a leaf, a histogram or a rollup dimension) last, with the partition and the id it came from */
typedef struct PartId
{
	int			child;
	const void *in;
	const void *out;
} PartId;

typedef struct PartSource
{
	int			n;
	Relation   *heap;
	Relation   *index;
	SiftSrcSource **src;
	uint32	   *off;			/* n + 1: the first superblock of each partition, the total last */
	bool		have_off;
	bool	   *skip;			/* sift_aggpart_prune: the partitions the scan does not read (no superblocks), NULL: all are read */
	int			cur;			/* the partition of the unit the source is at */
	int			next;			/* the first partition begin has not started */
	uint32		lo,
				hi;				/* the superblocks of the begin */
	const SiftSrcPrune *prune;
	int			nprune;
	SiftSrcUnit cunit;			/* the unit as the partition returned it */
	int			nleaves;
	PartId	   *id;
	SiftSrcDict *dict;
	uint64		idseq;
} PartSource;

/* slots of dictionaries: the leaves, the dimensions of a histogram, the dimensions of a rollup */
#define SLOT_HIST(ps, leaf)		((ps)->nleaves + (leaf))
#define SLOT_ROLLUP(ps, j)		((ps)->nleaves + SIFT_COLSRC_MAX_LEAVES + (j))
#define NSLOTS(nleaves)			((nleaves) + SIFT_COLSRC_MAX_LEAVES + SIFT_SRC_MAX_RDIMS)

static inline PartSource *
ps_of(void *impl)
{
	return impl;
}

static inline SiftSrcSource *
cur_of(void *impl)
{
	PartSource *ps = impl;

	return ps->src[ps->cur];
}

static inline const SiftSrcUnit *
unit_of(void *impl)
{
	return &ps_of(impl)->cunit;
}

/* what PostgreSQL checks for a query on a partitioned table: its privileges and row security, not the partitions' */
static void
check_access(Relation parent)
{
	Oid			relid = RelationGetRelid(parent);

	if (pg_class_aclcheck(relid, GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, OBJECT_TABLE, RelationGetRelationName(parent));
	if (check_enable_rls(relid, InvalidOid, true) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_col indexes cannot be used on table \"%s\", which has row-level security", RelationGetRelationName(parent))));
}

static const SiftSrcProvider part_provider;

SiftSrcSource *
sift_aggpart_open(Relation parent, int n, const Oid *heaps, const Oid *indexes, Snapshot snapshot, MemoryContext mcxt)
{
	PartSource *ps = MemoryContextAllocZero(mcxt, sizeof(PartSource));
	SiftSrcSource *out = MemoryContextAlloc(mcxt, sizeof(SiftSrcSource));

	if (n <= 0 || n > SIFT_AGG_MAX_PARTS)
		elog(ERROR, "sift: %d partitions in a scan, from 1 to %d", n, SIFT_AGG_MAX_PARTS);
	check_access(parent);
	ps->n = n;
	ps->heap = MemoryContextAlloc(mcxt, n * sizeof(Relation));
	ps->index = MemoryContextAlloc(mcxt, n * sizeof(Relation));
	ps->src = MemoryContextAlloc(mcxt, n * sizeof(SiftSrcSource *));
	ps->off = MemoryContextAllocZero(mcxt, (n + 1) * sizeof(uint32));
	for (int i = 0; i < n; i++)
	{
		ps->heap[i] = table_open(heaps[i], AccessShareLock);
		ps->index[i] = index_open(indexes[i], AccessShareLock);
		ps->src[i] = sift_colsrc_open_part(ps->heap[i], ps->index[i], snapshot, mcxt);
	}
	ps->nleaves = sift_colsrc_source_layout(ps->src[0])->nleaves;
	ps->id = MemoryContextAllocZero(mcxt, NSLOTS(ps->nleaves) * sizeof(PartId));
	ps->dict = MemoryContextAllocZero(mcxt, NSLOTS(ps->nleaves) * sizeof(SiftSrcDict));
	ps->cur = 0;
	out->prov = &part_provider;
	out->impl = ps;
	return out;
}

static void
part_close(void *impl)
{
	PartSource *ps = impl;

	for (int i = 0; i < ps->n; i++)
	{
		sift_colsrc_close(ps->src[i]);
		index_close(ps->index[i], NoLock);
		table_close(ps->heap[i], NoLock);
	}
}

static const SiftSrcLayout *
part_source_layout(const void *impl)
{
	const PartSource *ps = impl;

	return sift_colsrc_source_layout(ps->src[0]);
}

/* the superblocks the partitions have now, counted once: the units are numbered by them for as long as the source is open */
static void
count_sbs(PartSource *ps)
{
	uint64		total = 0;

	if (ps->have_off)
		return;
	for (int i = 0; i < ps->n; i++)
	{
		ps->off[i] = (uint32) total;
		if (ps->skip == NULL || !ps->skip[i])
			total += sift_colsrc_nsb(ps->src[i]);
	}
	if (total > PG_UINT32_MAX)
		ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("sift: the partitions of a scan hold more superblocks than the scan can number")));
	ps->off[ps->n] = (uint32) total;
	ps->have_off = true;
}

static uint32
part_nsb(void *impl)
{
	PartSource *ps = impl;

	count_sbs(ps);
	return ps->off[ps->n];
}

void
sift_aggpart_offsets(SiftSrcSource *src, uint32 *off)
{
	PartSource *ps = src->impl;

	count_sbs(ps);
	memcpy(off, ps->off, (ps->n + 1) * sizeof(uint32));
}

void
sift_aggpart_set_offsets(SiftSrcSource *src, const uint32 *off)
{
	PartSource *ps = src->impl;

	memcpy(ps->off, off, (ps->n + 1) * sizeof(uint32));
	ps->have_off = true;
}

typedef struct QualVars
{
	Oid			part;
	Oid			parent;
	bool		ok;
} QualVars;

/* a column of the partition's bound as the column of the partitioned table (the attribute numbers differ after a dropped column) */
static Node *
qual_vars_mutator(Node *n, QualVars *qv)
{
	Var		   *v;
	AttrNumber	att;

	if (n == NULL)
		return NULL;
	if (!IsA(n, Var))
		return expression_tree_mutator(n, qual_vars_mutator, qv);
	v = copyObject((Var *) n);
	att = get_attnum(qv->parent, get_attname(qv->part, v->varattno, false));
	if (att == InvalidAttrNumber)
		qv->ok = false;
	v->varattno = att;
	return (Node *) v;
}

/*
 * The partitions whose bounds the conditions refute (they hold the partitioned table's columns, with varno 1 and constants for the operands the
 * query evaluated when it started) are not read: they have no superblocks. Before the first sift_colsrc_nsb. PostgreSQL's own test of a
 * partition against the conditions, so a partition it cannot refute (a default partition, a hash remainder) stays. Returns how many are dropped.
 */
int
sift_aggpart_prune(SiftSrcSource *src, Oid parent, List *conds)
{
	PartSource *ps = src->impl;
	int			dropped = 0;

	Assert(!ps->have_off);
	if (conds == NIL)
		return 0;
	ps->skip = palloc0(ps->n * sizeof(bool));
	for (int i = 0; i < ps->n; i++)
	{
		QualVars	qv = {RelationGetRelid(ps->heap[i]), parent, true};
		List	   *bound = (List *) qual_vars_mutator((Node *) RelationGetPartitionQual(ps->heap[i]), &qv);

		if (qv.ok && bound != NIL && predicate_refuted_by(bound, conds, false))
		{
			ps->skip[i] = true;
			dropped++;
		}
	}
	return dropped;
}

/* Starts the next partition that has superblocks in [lo, hi); false when there is none */
static bool
start_next(PartSource *ps)
{
	for (int i = ps->next; i < ps->n && ps->off[i] < ps->hi; i++)
	{
		uint32		from = Max(ps->lo, ps->off[i]),
					to = Min(ps->hi, ps->off[i + 1]);

		if (from >= to)
			continue;
		ps->cur = i;
		ps->next = i + 1;
		sift_colsrc_begin(ps->src[i], from - ps->off[i], to - ps->off[i], ps->prune, ps->nprune);
		return true;
	}
	ps->next = ps->n;
	return false;
}

static void
part_begin(void *impl, uint32 sb_lo, uint32 sb_hi, const SiftSrcPrune *prune, int nprune)
{
	PartSource *ps = impl;

	count_sbs(ps);
	ps->lo = sb_lo;
	ps->hi = sb_hi;
	ps->prune = prune;
	ps->nprune = nprune;
	ps->next = 0;
	while (ps->next < ps->n && ps->off[ps->next + 1] <= sb_lo)
		ps->next++;
	ps->cur = -1;
}

static bool
part_next(void *impl, SiftSrcUnit *unit)
{
	PartSource *ps = impl;

	for (;;)
	{
		if (ps->cur >= 0 && sift_colsrc_next(ps->src[ps->cur], &ps->cunit))
		{
			*unit = (SiftSrcUnit) {ps->off[ps->cur] + ps->cunit.sb, ps->cunit.nrow, ps};
			return true;
		}
		if (!start_next(ps))
			return false;
	}
}

/* the dictionary of a read as the wrapper presents it: the id is the partition's own, made unique among the partitions (a new dictionary gets a new id) */
static const SiftSrcDict *
presented(PartSource *ps, int slot, const SiftSrcDict *d)
{
	PartId	   *m = &ps->id[slot];

	if (d == NULL)
		return NULL;
	if (m->child != ps->cur + 1 || m->in != d->id)
	{
		m->child = ps->cur + 1;
		m->in = d->id;
		m->out = (const void *) (uintptr_t) (PART_DICT_ID_BASE + ++ps->idseq);
	}
	ps->dict[slot] = *d;
	ps->dict[slot].id = m->out;
	return &ps->dict[slot];
}

static void
part_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec)
{
	sift_colsrc_read(cur_of(impl), unit_of(impl), leaf, vec);
	vec->dict = presented(impl, leaf, vec->dict);
}

static void
part_read_into(void *impl, const SiftSrcUnit *unit, int leaf, const SiftSrcBuf *buf, SiftSrcVec *vec)
{
	sift_colsrc_read_into(cur_of(impl), unit_of(impl), leaf, buf, vec);
	vec->dict = presented(impl, leaf, vec->dict);
}

static void
part_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec)
{
	sift_colsrc_read_sel(cur_of(impl), unit_of(impl), leaf, bits, vec);
	vec->dict = presented(impl, leaf, vec->dict);
}

static int64
part_leaf_n(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_colsrc_leaf_n(cur_of(impl), unit_of(impl), leaf);
}

static void
part_live(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	sift_colsrc_live(cur_of(impl), unit_of(impl), bits);
}

static void
part_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	sift_colsrc_vet(cur_of(impl), unit_of(impl), bits);
}

static bool
part_vet_needed(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_vet_needed(cur_of(impl), unit_of(impl));
}

static void
part_counters(const void *impl, SiftSrcCounters *c)
{
	const PartSource *ps = impl;

	memset(c, 0, sizeof(*c));
	for (int i = 0; i < ps->n; i++)
	{
		SiftSrcCounters pc;

		sift_colsrc_counters(ps->src[i], &pc);
		c->units += pc.units;
		c->units_pruned += pc.units_pruned;
		c->rows += pc.rows;
		c->bytes += pc.bytes;
		c->rows_vetted += pc.rows_vetted;
		c->heap_pages += pc.heap_pages;
		c->rechecked += pc.rechecked;
		c->recheck_units += pc.recheck_units;
	}
}

static bool
part_overflow(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_colsrc_overflow(cur_of(impl), unit_of(impl), leaf);
}

/* ---- the tests the partitions apply on what they store: every partition gets them ---- */

static void
part_prune_str(void *impl, const SiftSrcStrTest *t, int nt)
{
	PartSource *ps = impl;

	for (int i = 0; i < ps->n; i++)
		sift_colsrc_prune_str(ps->src[i], t, nt);
}

static bool
part_filter_str(void *impl, const SiftSrcUnit *unit, const SiftSrcStrTest *t, uint64 *rows)
{
	return sift_colsrc_filter_str(cur_of(impl), unit_of(impl), t, rows);
}

/* the partitions share one layout, so they take the same tests: the first's answer is everyone's */
static int
part_set_wide(void *impl, const SiftSrcWideTest *t, int nt)
{
	PartSource *ps = impl;
	int			n = 0;

	for (int i = 0; i < ps->n; i++)
		n = sift_colsrc_set_wide(ps->src[i], t, nt);
	return n;
}

static bool
part_filter_wide(void *impl, const SiftSrcUnit *unit, int i, uint64 *rows)
{
	return sift_colsrc_filter_wide(cur_of(impl), unit_of(impl), i, rows);
}

static uint64
part_set_filters(void *impl, const SiftSrcFilter *f, int nf)
{
	PartSource *ps = impl;
	uint64		applied = 0;

	for (int i = 0; i < ps->n; i++)
		applied = sift_colsrc_set_filters(ps->src[i], f, nf);
	return applied;
}

static bool
part_filter(void *impl, const SiftSrcUnit *unit, uint64 *bits)
{
	return sift_colsrc_filter(cur_of(impl), unit_of(impl), bits);
}

static bool
part_filter_types(void *impl, const SiftSrcUnit *unit, int leaf, uint32 types, uint64 *bits)
{
	return sift_colsrc_filter_types(cur_of(impl), unit_of(impl), leaf, types, bits);
}

static void
part_tolerate_ovf(void *impl, int leaf)
{
	PartSource *ps = impl;

	for (int i = 0; i < ps->n; i++)
		sift_colsrc_tolerate_ovf(ps->src[i], leaf);
}

static void
part_set_recheck(void *impl, int leaf, int mode)
{
	PartSource *ps = impl;

	for (int i = 0; i < ps->n; i++)
		sift_colsrc_set_recheck(ps->src[i], leaf, mode);
}

static void
part_overflow_raise(void *impl, const SiftSrcUnit *unit, const uint64 *rows)
{
	sift_colsrc_overflow_raise(cur_of(impl), unit_of(impl), rows);
}

static void
part_read_types(void *impl, const SiftSrcUnit *unit, int leaf, uint8 *tags)
{
	sift_colsrc_read_types(cur_of(impl), unit_of(impl), leaf, tags);
}

static void
part_read_jnull(void *impl, const SiftSrcUnit *unit, int leaf, uint64 *bits)
{
	sift_colsrc_read_jnull(cur_of(impl), unit_of(impl), leaf, bits);
}

/* ---- pre-aggregates: each unit answers from its partition ---- */

static bool
part_unit_exact(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_unit_exact(cur_of(impl), unit_of(impl));
}

static bool
part_unit_agg(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcUnitAgg *agg)
{
	return sift_colsrc_unit_agg(cur_of(impl), unit_of(impl), leaf, agg);
}

static bool
part_unit_hist(void *impl, const SiftSrcUnit *unit, int dim, SiftSrcHist *h)
{
	if (!sift_colsrc_unit_hist(cur_of(impl), unit_of(impl), dim, h))
		return false;
	h->dict = presented(impl, SLOT_HIST(ps_of(impl), dim), h->dict);
	return true;
}

static uint32
part_unit_rollup_tuples(void *impl, const SiftSrcUnit *unit, int list)
{
	return sift_colsrc_unit_rollup_tuples(cur_of(impl), unit_of(impl), list);
}

static bool
part_unit_rollup(void *impl, const SiftSrcUnit *unit, int list, const SiftSrcRollupNeed *need, SiftSrcRollup *r)
{
	if (!sift_colsrc_unit_rollup(cur_of(impl), unit_of(impl), list, need, r))
		return false;
	for (int j = 0; j < r->ndims; j++)
		r->dict[j] = presented(impl, SLOT_ROLLUP(ps_of(impl), j), r->dict[j]);
	return true;
}

static bool
part_unit_rollup_miss(void *impl, const SiftSrcUnit *unit)
{
	return sift_colsrc_unit_rollup_miss(cur_of(impl), unit_of(impl));
}

/* ---- the Vulkan path ---- */

static bool
part_gpu_unit(void *impl, const SiftSrcUnit *unit, SiftSrcGpuUnit *g)
{
	return sift_colsrc_gpu_unit(cur_of(impl), unit_of(impl), g);
}

static bool
part_gpu_dense(void *impl, const SiftSrcUnit *unit, int leaf)
{
	return sift_colsrc_gpu_dense(cur_of(impl), unit_of(impl), leaf);
}

static bool
part_gpu_chunk(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcGpuChunk *c)
{
	return sift_colsrc_gpu_chunk(cur_of(impl), unit_of(impl), leaf, c);
}

static void
part_gpu_dict(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcDict *dict)
{
	sift_colsrc_gpu_dict(cur_of(impl), unit_of(impl), leaf, dict);
	*dict = *presented(impl, leaf, dict);
}

/*
 * No match set (select), no top-N (tids, zone, zone_range, read_rows): the planner never gives a partitioned table a search or a ranked scan.
 * The others are the partitions' own; a provider that lacks one of them has no partitions (sift_colsrc_can_part).
 */
static const SiftSrcProvider part_provider = {
	.close = part_close,
	.source_layout = part_source_layout,
	.nsb = part_nsb,
	.begin = part_begin,
	.next = part_next,
	.read = part_read,
	.live = part_live,
	.vet = part_vet,
	.vet_needed = part_vet_needed,
	.counters = part_counters,
	.leaf_n = part_leaf_n,
	.read_into = part_read_into,
	.overflow = part_overflow,
	.set_filters = part_set_filters,
	.filter = part_filter,
	.read_sel = part_read_sel,
	.filter_types = part_filter_types,
	.tolerate_ovf = part_tolerate_ovf,
	.set_recheck = part_set_recheck,
	.overflow_raise = part_overflow_raise,
	.read_types = part_read_types,
	.read_jnull = part_read_jnull,
	.prune_str = part_prune_str,
	.filter_str = part_filter_str,
	.set_wide = part_set_wide,
	.filter_wide = part_filter_wide,
	.unit_exact = part_unit_exact,
	.unit_agg = part_unit_agg,
	.unit_hist = part_unit_hist,
	.unit_rollup_tuples = part_unit_rollup_tuples,
	.unit_rollup = part_unit_rollup,
	.unit_rollup_miss = part_unit_rollup_miss,
	.gpu_unit = part_gpu_unit,
	.gpu_dense = part_gpu_dense,
	.gpu_chunk = part_gpu_chunk,
	.gpu_dict = part_gpu_dict,
};

/* ---- planner side ---- */

bool
sift_aggpart_same_layout(const SiftSrcLayout *a, const SiftSrcLayout *b)
{
	if (a->is_table != b->is_table || a->natts != b->natts || a->nleaves != b->nleaves || !equal(a->attexprs, b->attexprs))
		return false;
	for (int i = 0; i < a->nleaves; i++)
	{
		const SiftSrcLeaf *x = &a->leaf[i],
				   *y = &b->leaf[i];

		if (x->id != y->id || x->attno != y->attno || x->parent != y->parent || x->kind != y->kind || x->vtype != y->vtype || x->typid != y->typid ||
			x->wtypid != y->wtypid || x->wide != y->wide || x->scale != y->scale || x->lossy != y->lossy || x->astext != y->astext ||
			!equal(x->path, y->path) || strcmp(x->name, y->name) != 0)
			return false;
	}
	return true;
}

/* the number of a leaf's values is a lower bound over several sources (the distinct values of a leaf may repeat among them), its range the union */
void
sift_aggpart_add_stats(SiftSrcStats *tot, const SiftSrcStats *s)
{
	double		w = tot->nrows + s->nrows;

	if (w > 0)
	{
		tot->allvisible_frac = (tot->allvisible_frac * tot->nrows + s->allvisible_frac * s->nrows) / w;
		tot->exact_frac = (tot->exact_frac * tot->nrows + s->exact_frac * s->nrows) / w;
	}
	for (int i = 0; i < SIFT_COLSRC_MAX_LEAVES; i++)
	{
		if (w > 0)
			tot->elems_per_row[i] = (tot->elems_per_row[i] * tot->nrows + s->elems_per_row[i] * s->nrows) / w;
		if (tot->min[i] == 0 && tot->max[i] == 0)
		{
			tot->min[i] = s->min[i];
			tot->max[i] = s->max[i];
		}
		else if (s->min[i] != 0 || s->max[i] != 0)
		{
			tot->min[i] = Min(tot->min[i], s->min[i]);
			tot->max[i] = Max(tot->max[i], s->max[i]);
		}
		tot->ndistinct[i] = Max(tot->ndistinct[i], s->ndistinct[i]);
		tot->ovf[i] |= s->ovf[i];
		tot->nokinds[i] |= s->nokinds[i];
		tot->jnull[i] |= s->jnull[i];
	}
	tot->nrows += s->nrows;
	tot->nunits += s->nunits;
	tot->bytes += s->bytes;
}
