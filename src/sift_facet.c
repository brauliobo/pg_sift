/*
 * sift_facet.c - sift.facets(): the number of visible matches of a query per value of a keyword
 * index.
 *
 * The matches of the query on the search index are collected once, as count(*) sees them (visibility
 * map, heap checks of the other pages, phrase recheck), into superblock groups with a 64-byte offset
 * row per page. Every value of the keyword index is a term of its dictionary, so its count is the
 * popcount of the AND of its posting groups with those: the heap is not read for the values. Values
 * are tried in descending order of an upper bound of their count (their df plus the mutable
 * matches), which ends a top-k early; the NULL row (matches without a value) is the matches minus
 * the counted ones, so asking for it counts every value.
 */
#include "postgres.h"

#include "access/genam.h"
#include "access/relation.h"
#include "access/table.h"
#include "access/tableam.h"
#include "catalog/index.h"
#include "catalog/objectaddress.h"
#include "catalog/pg_class.h"
#include "catalog/pg_index.h"
#include "executor/executor.h"
#include "funcapi.h"
#include "miscadmin.h"
#include "port/pg_bitutils.h"
#include "storage/predicate.h"
#include "utils/acl.h"
#include "utils/builtins.h"
#include "utils/hsearch.h"
#include "utils/lsyscache.h"
#include "utils/memutils.h"
#include "utils/rel.h"
#include "utils/rls.h"
#include "utils/snapmgr.h"

#include "sift_eval.h"
#include "sift_mutable.h"
#include "sift_segment.h"
#include "sift_sql.h"

typedef uint64 Row[SIFT_OFFMASK_WORDS];

#define ROW_BYTES	sizeof(Row)

/* The matches of the query in one superblock */
typedef struct QSb
{
	uint32		sb;
	int64		row;			/* first row in Facets.rows */
	uint64		pages[SIFT_PAGEMASK_WORDS];
} QSb;

/* A value in one immutable segment, or (seg -1) in the mutable ones with info.df = its matches there */
typedef struct Entry
{
	uint64		off;			/* term bytes in Facets.terms */
	uint32		len;
	int			seg;
	SiftTermInfo info;
} Entry;

typedef struct Value
{
	const Entry *e;
	int			n;
	uint64		bound;			/* at least its count */
} Value;

typedef struct Res
{
	const char *term;			/* NULL: the matches without a value */
	int			len;
	int64		n;
} Res;

typedef struct MutCount
{
	char		term[SIFT_MAX_TERM_BYTES + 1];	/* zero padded */
	int64		n;
} MutCount;

/* An immutable segment of the keyword index and the query's groups as it holds them */
typedef struct FacetSeg
{
	SiftSegment *seg;
	bool		dirty;			/* a doc was vacuumed: groups are filtered by liveness on demand */
	const sift_grp **view;		/* per QSb; NULL: no live doc there */
	bool	   *viewed;
} FacetSeg;

typedef struct Facets
{
	MemoryContext mcxt;
	MemoryContext tmpcx;		/* one value's cursor */
	Relation	heap;
	Relation	index;
	Relation	findex;
	QSb		   *qsb;
	int			nqsb;
	int			qsbcap;
	Row		   *rows;
	int64		nrows;
	int64		rowcap;
	int64		total;			/* visible matches */
	sift_grp   *grp;			/* per QSb */
	FacetSeg   *fseg;
	int			nseg;
	char	   *terms;
	size_t		nterms;
	size_t		termcap;
	Entry	   *ent;
	int64		nent;
	int64		entcap;
	Res		   *res;
	int64		nres;
	int64		rescap;
	int64		lim;			/* results wanted, < 0 all */
	bool		bounded;		/* results beyond lim are dropped while collecting */
	/* phrase recheck */
	SiftVis		vis;
	IndexInfo  *iinfo;
	TupleTableSlot *slot;
	EState	   *estate;
} Facets;

/* ---------- the query's matches ---------- */

static void
add_page(void *arg, BlockNumber blk, const uint64 *offmask)
{
	Facets	   *f = arg;
	uint32		sb = sift_block_sb(blk),
				page = blk & (SIFT_SB_PAGES - 1);
	QSb		   *q;

	if (f->nqsb == 0 || f->qsb[f->nqsb - 1].sb != sb)
	{
		if (f->nqsb == f->qsbcap)
		{
			f->qsbcap = Max(64, 2 * f->qsbcap);
			f->qsb = f->qsb ? repalloc_huge(f->qsb, f->qsbcap * sizeof(QSb)) :
				MemoryContextAllocHuge(f->mcxt, f->qsbcap * sizeof(QSb));
		}
		f->qsb[f->nqsb++] = (QSb) {.sb = sb,.row = f->nrows};
	}
	q = &f->qsb[f->nqsb - 1];
	q->pages[page / 64] |= UINT64CONST(1) << (page % 64);
	if (f->nrows == f->rowcap)
	{
		f->rowcap = Max(256, 2 * f->rowcap);
		f->rows = f->rows ? repalloc_huge(f->rows, f->rowcap * ROW_BYTES) :
			MemoryContextAllocHuge(f->mcxt, f->rowcap * ROW_BYTES);
	}
	memcpy(f->rows[f->nrows++], offmask, ROW_BYTES);
}

/* Visibility and the indexed text of a candidate, for the plan's phrase recheck */
static bool
facet_verify(void *arg, ItemPointer tid, bool want_doc, text **doc)
{
	Facets	   *f = arg;
	Datum		value;
	bool		isnull;

	if (!sift_vis_fetch(&f->vis, tid, f->slot))
		return false;
	if (!want_doc)
		return true;
	ResetPerTupleExprContext(f->estate);
	GetPerTupleExprContext(f->estate)->ecxt_scantuple = f->slot;
	FormIndexDatum(f->iinfo, f->slot, f->estate, &value, &isnull);
	if (isnull)
		return false;
	*doc = DatumGetTextPP(value);
	return true;
}

static void
collect_matches(Facets *f, const char *query, int len)
{
	SiftIndexState *st = sift_state_open(f->index, f->mcxt);
	SiftQuery  *q = sift_state_query(st, query, len);
	SiftPlan   *plan = sift_plan_bind(st, &q, 1, 0);
	Snapshot	snapshot = GetActiveSnapshot();

	PredicateLockRelation(f->heap, snapshot);
	f->estate = CreateExecutorState();
	f->iinfo = BuildIndexInfo(f->index);
	f->slot = table_slot_create(f->heap, NULL);
	sift_vis_begin(&f->vis, f->heap, snapshot);
	f->total = sift_eval_pages(plan, &f->vis, facet_verify, f, add_page, f);
	sift_vis_end(&f->vis);
	sift_state_close(st);
	ExecDropSingleTupleTableSlot(f->slot);
	FreeExecutorState(f->estate);

	f->grp = MemoryContextAlloc(f->mcxt, Max(f->nqsb, 1) * sizeof(sift_grp));
	for (int i = 0; i < f->nqsb; i++)
	{
		f->grp[i] = (sift_grp) {.gid = f->qsb[i].sb,.w = ROW_BYTES,.off = (const uint8 *) f->rows[f->qsb[i].row]};
		memcpy(f->grp[i].pages, f->qsb[i].pages, sizeof(f->grp[i].pages));
	}
}

/* First QSb at or after lo whose superblock is at least sb */
static int
q_lower(const Facets *f, int lo, uint32 sb)
{
	int			hi = f->nqsb;

	while (lo < hi)
	{
		int			mid = lo + (hi - lo) / 2;

		if (f->qsb[mid].sb < sb)
			lo = mid + 1;
		else
			hi = mid;
	}
	return lo;
}

/* Is the document at key in the group (offset rows of ROW_BYTES) */
static bool
grp_has(const sift_grp *g, SiftKey key)
{
	uint32		page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1,
				row = 0;
	const uint64 *r;

	if (!(g->pages[page / 64] >> (page % 64) & 1))
		return false;
	for (int w = 0; w < page / 64; w++)
		row += sift_pc64(g->pages[w]);
	row += sift_pc64(g->pages[page / 64] & ((UINT64CONST(1) << (page % 64)) - 1));
	r = (const uint64 *) (g->off + (size_t) row * ROW_BYTES);
	return r[off / 64] >> (off % 64) & 1;
}

/* ---------- the keyword index ---------- */

/* The query's group i restricted to the docs of seg that are live; NULL when it has none there */
static const sift_grp *
live_group(Facets *f, SiftSegment *seg, int i)
{
	const QSb  *q = &f->qsb[i];
	SiftDocChunk dc;
	Row		   *rows;
	sift_grp   *g;
	uint64		pages[SIFT_PAGEMASK_WORDS] = {0};
	int			n = 0,
				idx = 0;

	if (!sift_seg_docs(seg, q->sb, &dc))
		return NULL;
	if (!dc.dirty)
		return &f->grp[i];
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		n += sift_pc64(q->pages[wd]);
	rows = MemoryContextAlloc(f->mcxt, n * ROW_BYTES);
	n = 0;
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 m = q->pages[wd]; m; m &= m - 1, idx++)
		{
			int			page = wd * 64 + pg_rightmost_one_pos64(m);
			uint64		live[SIFT_OFFMASK_WORDS];
			uint64		any = 0;

			sift_seg_page_live(seg, &dc, page, live);
			for (int k = 0; k < SIFT_OFFMASK_WORDS; k++)
				any |= rows[n][k] = f->rows[q->row + idx][k] & live[k];
			if (any)
			{
				pages[wd] |= UINT64CONST(1) << (page % 64);
				n++;
			}
		}
	sift_sr_release(&seg->sr);
	if (n == 0)
		return NULL;
	g = MemoryContextAlloc(f->mcxt, sizeof(sift_grp));
	*g = (sift_grp) {.gid = q->sb,.w = ROW_BYTES,.off = (const uint8 *) rows};
	memcpy(g->pages, pages, sizeof(pages));
	return g;
}

static const sift_grp *
seg_view(Facets *f, FacetSeg *fs, int i)
{
	if (!fs->dirty)
		return &f->grp[i];
	if (!fs->viewed[i])
	{
		fs->view[i] = live_group(f, fs->seg, i);
		fs->viewed[i] = true;
	}
	return fs->view[i];
}

static void
add_entry(Facets *f, const char *term, int len, int seg, const SiftTermInfo *info)
{
	if (f->nent == f->entcap)
	{
		f->entcap = Max(256, 2 * f->entcap);
		f->ent = f->ent ? repalloc_huge(f->ent, f->entcap * sizeof(Entry)) :
			MemoryContextAllocHuge(f->mcxt, f->entcap * sizeof(Entry));
	}
	if (f->nterms + len > f->termcap)
	{
		f->termcap = Max(4096, 2 * f->termcap + len);
		f->terms = f->terms ? repalloc_huge(f->terms, f->termcap) : MemoryContextAllocHuge(f->mcxt, f->termcap);
	}
	memcpy(f->terms + f->nterms, term, len);
	f->ent[f->nent++] = (Entry) {.off = f->nterms,.len = len,.seg = seg,.info = *info};
	f->nterms += len;
}

typedef struct DictVisit
{
	Facets	   *f;
	int			seg;
} DictVisit;

static bool
dict_visit(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	DictVisit  *v = arg;

	add_entry(v->f, term, len, v->seg, info);
	CHECK_FOR_INTERRUPTS();
	return true;
}

/* Matches of the query by value among the docs of the mutable segments (their records carry the value) */
static void
count_mutable(Facets *f, const SiftSegEntry *entry, HTAB *tab)
{
	SiftMutReader *r = sift_mut_open(f->findex, entry, f->mcxt, NULL, NULL);
	SiftMutDoc	doc;

	while (sift_mut_next(r, &doc))
	{
		SiftKey		key = sift_key_from_tid(&doc.tid);
		int			i;
		MutCount   *c;
		char		term[SIFT_MAX_TERM_BYTES + 1] = {0};
		bool		found;

		if (doc.deleted || doc.nterms == 0)
			continue;
		i = q_lower(f, 0, sift_block_sb(sift_key_block(key)));
		if (i == f->nqsb || f->qsb[i].sb != sift_block_sb(sift_key_block(key)) || !grp_has(&f->grp[i], key))
			continue;
		Assert(doc.nterms == 1);
		memcpy(term, doc.terms, doc.len[0]);
		c = hash_search(tab, term, HASH_ENTER, &found);
		c->n = found ? c->n + 1 : 1;
	}
	sift_mut_close(r);
}

static void
collect_entries(Facets *f, SiftIndexState *st)
{
	HTAB	   *mut = NULL;
	HASH_SEQ_STATUS seq;
	MutCount   *c;

	f->fseg = MemoryContextAllocZero(f->mcxt, Max(st->dir.nsegs, 1) * sizeof(FacetSeg));
	for (int i = 0; i < st->dir.nsegs; i++)
	{
		const SiftSegEntry *entry = &st->dir.segs[i];

		if (entry->kind == SIFT_SEG_IMMUTABLE)
		{
			FacetSeg   *fs = &f->fseg[f->nseg];
			DictVisit	v = {f, f->nseg++};

			fs->seg = sift_seg_open(f->findex, entry, f->mcxt);
			fs->dirty = (entry->flags & SIFT_SEG_DIRTY) != 0;
			if (fs->dirty)
			{
				fs->view = MemoryContextAllocZero(f->mcxt, f->nqsb * sizeof(sift_grp *));
				fs->viewed = MemoryContextAllocZero(f->mcxt, f->nqsb);
			}
			sift_dict_scan(fs->seg->dict, NULL, 0, NULL, 0, dict_visit, &v);
			sift_sr_release(&fs->seg->sr);
			continue;
		}
		if (mut == NULL)
		{
			HASHCTL		ctl = {.keysize = SIFT_MAX_TERM_BYTES + 1,.entrysize = sizeof(MutCount),.hcxt = f->mcxt};

			mut = hash_create("sift facet mutable values", 64, &ctl, HASH_ELEM | HASH_BLOBS | HASH_CONTEXT);
		}
		count_mutable(f, entry, mut);
	}
	if (mut == NULL)
		return;
	hash_seq_init(&seq, mut);
	while ((c = hash_seq_search(&seq)) != NULL)
	{
		SiftTermInfo info = {.df = c->n};

		add_entry(f, c->term, strlen(c->term), -1, &info);
	}
}

/* ---------- counting ---------- */

/* The matches among the docs of a term kept inline in the dictionary */
static int64
count_inline(Facets *f, FacetSeg *fs, const SiftTermInfo *info)
{
	int64		n = 0;

	for (uint32 k = 0; k < info->df; k++)
	{
		SiftKey		key = info->inline_keys[k];
		uint32		sb = sift_block_sb(sift_key_block(key));
		int			i = q_lower(f, 0, sb);
		const sift_grp *g;

		if (i < f->nqsb && f->qsb[i].sb == sb && (g = seg_view(f, fs, i)) != NULL && grp_has(g, key))
			n++;
	}
	return n;
}

/* The matches among the postings of a term: its groups against the query's, leapfrogging both */
static int64
count_postings(Facets *f, FacetSeg *fs, const SiftTermInfo *info)
{
	SiftPostingCursor *pc = sift_post_open(&fs->seg->sr, info, f->tmpcx);
	sift_cur   *cur = sift_post_cursor(pc);
	int64		n = 0;
	int			i = 0;

	while (i < f->nqsb)
	{
		const sift_grp *g = sift_seek(cur, f->qsb[i].sb);
		const sift_grp *q;

		if (g == NULL)
			break;
		i = q_lower(f, i, g->gid);
		if (i < f->nqsb && f->qsb[i].sb == g->gid)
		{
			if ((q = seg_view(f, fs, i)) != NULL)
				n += sift_grp_and_count(q, g);
			i++;
		}
	}
	sift_sr_release(&fs->seg->sr);
	MemoryContextReset(f->tmpcx);
	return n;
}

static int64
count_value(Facets *f, const Value *v)
{
	int64		n = 0;

	for (int k = 0; k < v->n; k++)
	{
		const Entry *e = &v->e[k];

		if (e->seg < 0)
			n += e->info.df;
		else if (e->info.flags & SIFT_TERM_INLINE)
			n += count_inline(f, &f->fseg[e->seg], &e->info);
		else
			n += count_postings(f, &f->fseg[e->seg], &e->info);
	}
	return n;
}

/* ---------- results ---------- */

static int
res_cmp(const Res *a, const Res *b)
{
	int			c;

	if (a->n != b->n)
		return a->n > b->n ? -1 : 1;
	if (a->term == NULL || b->term == NULL)
		return (a->term == NULL) - (b->term == NULL);
	c = memcmp(a->term, b->term, Min(a->len, b->len));
	return c ? c : a->len - b->len;
}

static int
res_qsort_cmp(const void *a, const void *b)
{
	return res_cmp(a, b);
}

static void
res_add(Facets *f, Res r)
{
	int64		pos = f->nres;

	if (f->nres == f->rescap && (!f->bounded || f->nres < f->lim))
	{
		f->rescap = Max(16, Min(2 * f->rescap, f->bounded ? f->lim : PG_INT64_MAX));
		f->res = f->res ? repalloc_huge(f->res, f->rescap * sizeof(Res)) :
			MemoryContextAllocHuge(f->mcxt, f->rescap * sizeof(Res));
	}
	if (f->bounded)
	{
		int64		lo = 0,
					hi = f->nres;

		while (lo < hi)
		{
			int64		mid = lo + (hi - lo) / 2;

			if (res_cmp(&f->res[mid], &r) > 0)
				hi = mid;
			else
				lo = mid + 1;
		}
		if ((pos = lo) == f->lim)
			return;
		if (f->nres == f->lim)
			f->nres--;
		memmove(&f->res[pos + 1], &f->res[pos], (f->nres - pos) * sizeof(Res));
	}
	f->res[pos] = r;
	f->nres++;
}

static int
ent_cmp(const void *a, const void *b, void *arg)
{
	const Facets *f = arg;
	const Entry *x = a,
			   *y = b;
	int			c = memcmp(f->terms + x->off, f->terms + y->off, Min(x->len, y->len));

	if (c)
		return c;
	return x->len != y->len ? (int) x->len - (int) y->len : x->seg - y->seg;
}

static bool
same_term(const Facets *f, const Entry *x, const Entry *y)
{
	return x->len == y->len && memcmp(f->terms + x->off, f->terms + y->off, x->len) == 0;
}

static int
val_cmp(const void *a, const void *b, void *arg)
{
	const Facets *f = arg;
	const Value *x = a,
			   *y = b;
	Res			rx = {f->terms + x->e->off, x->e->len, 0},
				ry = {f->terms + y->e->off, y->e->len, 0};

	if (x->bound != y->bound)
		return x->bound > y->bound ? -1 : 1;
	return res_cmp(&rx, &ry);
}

/* The distinct values, most promising first */
static Value *
collect_values(Facets *f, int64 *nvals)
{
	Value	   *vals = MemoryContextAlloc(f->mcxt, Max(f->nent, 1) * sizeof(Value));
	int64		n = 0;

	qsort_arg(f->ent, f->nent, sizeof(Entry), ent_cmp, f);
	for (int64 i = 0, j; i < f->nent; i = j)
	{
		uint64		bound = 0;

		for (j = i; j < f->nent && same_term(f, &f->ent[i], &f->ent[j]); j++)
			bound += f->ent[j].info.df;
		vals[n++] = (Value) {.e = &f->ent[i],.n = j - i,.bound = Min(bound, f->total)};
	}
	qsort_arg(vals, n, sizeof(Value), val_cmp, f);
	*nvals = n;
	return vals;
}

/* No value that follows can beat the worst of a full top-k */
static bool
cannot_enter(const Facets *f, const Value *v)
{
	const Res  *worst;
	Res			r;

	if (!f->bounded || f->nres < f->lim)
		return false;
	worst = &f->res[f->lim - 1];
	r = (Res) {f->terms + v->e->off, v->e->len, v->bound};
	return res_cmp(&r, worst) > 0;
}

static void
count_values(Facets *f, bool nulls)
{
	int64		nvals,
				counted = 0;
	Value	   *vals = collect_values(f, &nvals);

	for (int64 i = 0; i < nvals; i++)
	{
		const Value *v = &vals[i];
		int64		n;

		if (f->bounded && f->nres == f->lim && v->bound < f->res[f->lim - 1].n)
			break;
		if (cannot_enter(f, v))
			continue;
		CHECK_FOR_INTERRUPTS();
		if ((n = count_value(f, v)) > 0)
		{
			counted += n;
			res_add(f, (Res) {f->terms + v->e->off, v->e->len, n});
		}
	}
	if (nulls && f->total > counted)
		res_add(f, (Res) {NULL, 0, f->total - counted});
}

/* ---------- SQL ---------- */

static Relation
open_index(Oid oid)
{
	Relation	index = index_open(oid, AccessShareLock);

	if (index->rd_rel->relam != sift_catalog()->am || index->rd_rel->relkind != RELKIND_INDEX)
		ereport(ERROR,
				(errcode(ERRCODE_WRONG_OBJECT_TYPE),
				 errmsg("\"%s\" is not a sift index", RelationGetRelationName(index))));
	if (!index->rd_index->indisvalid)
		ereport(ERROR,
				(errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE),
				 errmsg("index \"%s\" is not valid", RelationGetRelationName(index))));
	return index;
}

static void
check_indexes(Facets *f)
{
	if (f->findex->rd_index->indrelid != f->index->rd_index->indrelid)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("indexes \"%s\" and \"%s\" are on different tables",
						RelationGetRelationName(f->index), RelationGetRelationName(f->findex))));
	if (sift_index_options(f->findex)->analyzer != SIFT_ANALYZER_KEYWORD)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("index \"%s\" is not a keyword index", RelationGetRelationName(f->findex)),
				 errhint("Create it with WITH (analyzer = keyword).")));
	if (!heap_attisnull(f->findex->rd_indextuple, Anum_pg_index_indpred, NULL))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("index \"%s\" is a partial index", RelationGetRelationName(f->findex)),
				 errdetail("Rows outside its predicate would be counted as having no value.")));
	if (pg_class_aclcheck(RelationGetRelid(f->heap), GetUserId(), ACL_SELECT) != ACLCHECK_OK)
		aclcheck_error(ACLCHECK_NO_PRIV, get_relkind_objtype(f->heap->rd_rel->relkind),
					   RelationGetRelationName(f->heap));
	if (check_enable_rls(RelationGetRelid(f->heap), InvalidOid, false) == RLS_ENABLED)
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift.facets does not support tables with row-level security")));
}

static void
emit_results(ReturnSetInfo *rsinfo, Facets *f)
{
	if (!f->bounded)
		qsort(f->res, f->nres, sizeof(Res), res_qsort_cmp);
	for (int64 i = 0; i < f->nres && (f->lim < 0 || i < f->lim); i++)
	{
		Datum		values[2];
		bool		nulls[2] = {f->res[i].term == NULL, false};

		values[0] = nulls[0] ? (Datum) 0 : PointerGetDatum(cstring_to_text_with_len(f->res[i].term, f->res[i].len));
		values[1] = Int64GetDatum(f->res[i].n);
		tuplestore_putvalues(rsinfo->setResult, rsinfo->setDesc, values, nulls);
	}
}

/*
 * sift.facets(index regclass, facet_index regclass, query text, lim int DEFAULT 10,
 *             nulls bool DEFAULT false): (value text, count bigint), count descending then value.
 */
Datum
sift_facet_counts(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsinfo = (ReturnSetInfo *) fcinfo->resultinfo;
	Facets		f = {0};
	text	   *query;
	SiftIndexState *fst;
	bool		nulls = !PG_ARGISNULL(4) && PG_GETARG_BOOL(4);
	Oid			heapoid;

	InitMaterializedSRF(fcinfo, 0);
	if (PG_ARGISNULL(0) || PG_ARGISNULL(1) || PG_ARGISNULL(2))
		return (Datum) 0;
	f.lim = PG_ARGISNULL(3) ? -1 : PG_GETARG_INT32(3);
	if (!PG_ARGISNULL(3) && f.lim < 0)
		ereport(ERROR,
				(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
				 errmsg("the limit must not be negative")));
	f.bounded = f.lim >= 0 && !nulls;
	query = PG_GETARG_TEXT_PP(2);
	f.mcxt = AllocSetContextCreate(CurrentMemoryContext, "sift facets", ALLOCSET_DEFAULT_SIZES);
	f.tmpcx = AllocSetContextCreate(f.mcxt, "sift facet value", ALLOCSET_DEFAULT_SIZES);
	f.index = open_index(PG_GETARG_OID(0));
	f.findex = open_index(PG_GETARG_OID(1));
	heapoid = f.index->rd_index->indrelid;
	f.heap = table_open(heapoid, AccessShareLock);
	check_indexes(&f);
	if (f.lim != 0)
	{
		collect_matches(&f, VARDATA_ANY(query), VARSIZE_ANY_EXHDR(query));
		if (f.total > 0)
		{
			fst = sift_state_open(f.findex, f.mcxt);
			collect_entries(&f, fst);
			count_values(&f, nulls);
			sift_state_close(fst);
			for (int i = 0; i < f.nseg; i++)
				sift_seg_close(f.fseg[i].seg);
			emit_results(rsinfo, &f);
		}
	}
	table_close(f.heap, NoLock);
	index_close(f.findex, NoLock);
	index_close(f.index, NoLock);
	MemoryContextDelete(f.mcxt);
	return (Datum) 0;
}
