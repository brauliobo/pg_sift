/*
 * col/f2/pull_prune_test.c - the test-only SQL function sift._test_f2_pull, compiled only with SIFT_TESTING: it reads an index of format major 2 through sift_f2_pull (col/f2/sink/ext.h) the way
 * the aggregate scan of an unnest does, tests every row and element itself, and gives the answer beside what the pull left unread (SiftF2PullStats), so that a test says both that the answer is the
 * heap's and that the pruning happened. Its modes are the ways a consumer reads the elements of an array column (agg/unnest/unnest.h):
 *   rows     the live rows that pass the predicates of the rows: a = rows passing, b = sum of the first column of the predicates over them, c = live rows seen
 *   weights  no element column is read: a = the pairs the parents passing give (sift_unnest_weights), b = those parents, d = sum of the first column times the weight of its parent; the weight of
 *            each parent is checked against the pairs sift_unnest_expand makes of it (it too reads the layout alone). weights_left: as for a LEFT JOIN LATERAL
 *   elems    only elements: a = elements of the parents passing that pass the predicates of the elements, b = sum of the first integer child over them
 *   codes    the same elements counted by the dictionary codes of one or two children (sift_unnest_count_codes): groups = 'k0|k1=n;...', a = elements counted
 *   semi     a = parents passing that have an element that passes (sift_unnest_semi), b = parents passing
 *   any      the three-valued form (sift_unnest_any) of the same, the poison parents left out: a = parents whose test is true, b = parents whose test is known (the document is not NULL)
 * fallback is the live rows passing whose array the index cannot hold (poison, or the slab): the rows a consumer reads from the heap.
 * Predicates are 'n:op:value', op one of eq ne lt le gt ge: n is the attribute of the index (rows) or the child (elements, in the order of the children given), value an integer.
 */
#include "postgres.h"

#ifdef SIFT_TESTING

#include "access/genam.h"
#include "access/relation.h"
#include "fmgr.h"
#include "utils/array.h"
#include "utils/builtins.h"
#include "utils/snapmgr.h"

#include "agg/unnest/unnest.h"
#include "agg/vexpr/str.h"
#include "core/catname.h"
#include "col/am.h"
#include "col/f2/sink/ext.h"
#include "core/sql/sql.h"

PG_FUNCTION_INFO_V1(sift_test_f2_pull);

#define TP_MAX		8

typedef struct TestPred
{
	int			slot;			/* the column of the spec, or the child */
	SiftColOp	op;
	int64		val;
} TestPred;

typedef struct GroupCell
{
	char	   *k[2];			/* NULL: a NULL element */
	int64		n;
} GroupCell;

typedef struct Test
{
	SiftF2PullSpec spec;
	SiftColPred rpred[TP_MAX];
	SiftColPred epred[TP_MAX];
	SiftColValue vals[2 * TP_MAX];
	TestPred rt[TP_MAX];
	TestPred et[TP_MAX];
	int			nrt;
	int			net;
	int			first_int_child;
	int			ncodes;			/* the text children, counted by their codes: at most two */
	int			code_child[2];
	int64		out[4];
	int64		fallback;
	GroupCell		groups[512];
	int			ngroups;
	uint64	   *emask;
	uint64	   *hits;
	uint32		ecap;			/* words of emask and hits */
} Test;

static SiftColOp
op_of(const char *s)
{
	static const struct
	{
		const char *name;
		SiftColOp	op;
	}			ops[] = {{"eq", SIFT_COLOP_EQ}, {"ne", SIFT_COLOP_NE}, {"lt", SIFT_COLOP_LT}, {"le", SIFT_COLOP_LE}, {"gt", SIFT_COLOP_GT}, {"ge", SIFT_COLOP_GE}};

	for (int i = 0; i < (int) lengthof(ops); i++)
		if (strcmp(ops[i].name, s) == 0)
			return ops[i].op;
	elog(ERROR, "unknown operator \"%s\"", s);
}

static bool
holds(SiftColOp op, int64 v, int64 c)
{
	switch (op)
	{
		case SIFT_COLOP_EQ:
			return v == c;
		case SIFT_COLOP_NE:
			return v != c;
		case SIFT_COLOP_LT:
			return v < c;
		case SIFT_COLOP_LE:
			return v <= c;
		case SIFT_COLOP_GT:
			return v > c;
		default:
			return v >= c;
	}
}

static bool
bit_at(const uint64 *m, uint32 i)
{
	return m == NULL || ((m[i >> 6] >> (i & 63)) & 1);
}

static int
leaf_of_group(const SiftColRel *r, int att)
{
	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
		if (r->schema->cols[i].kind == SIFT_COLK_GROUP)
			return i;
	elog(ERROR, "attribute %d of the index is no array", att);
}

static int
leaf_of_child(const SiftColRel *r, int att, int group, int ord)
{
	int			k = 0;

	for (int i = r->first[att]; i < r->first[att] + r->nleaves[att]; i++)
		if (r->schema->cols[i].parent == group && r->schema->cols[i].kind == SIFT_COLK_SCALAR && r->schema->cols[i].lane == 0 && k++ == ord)
			return i;
	elog(ERROR, "child %d of attribute %d of the index does not exist", ord, att);
}

static SiftVecType
vec_type_of(const SiftColDef *d)
{
	return d->store == SIFT_COLS_STR ? SVEC_DICT32 : d->store == SIFT_COLS_INT ? SVEC_I64 : SVEC_F64;
}

/* the column of the spec for leaf, added once */
static int
add_col(Test *t, const SiftColRel *r, int att)
{
	const int	leaf = r->first[att];

	for (int i = 0; i < t->spec.ncols; i++)
		if (t->spec.col[i].leaf == leaf)
			return i;
	t->spec.col[t->spec.ncols] = (SiftF2PullCol) {.leaf = leaf,.type = SVEC_I64};
	return t->spec.ncols++;
}

static void
parse_pred(char *txt, int *n, SiftColOp *op, int64 *val)
{
	char	   *c1 = strchr(txt, ':'),
			   *c2 = c1 != NULL ? strchr(c1 + 1, ':') : NULL;

	if (c2 == NULL)
		elog(ERROR, "a predicate is n:op:value");
	*c1 = *c2 = '\0';
	*n = atoi(txt);
	*op = op_of(c1 + 1);
	*val = strtoll(c2 + 1, NULL, 10);
}

static int
texts_of(ArrayType *a, char ***out)
{
	Datum	   *d;
	int			n;

	if (a == NULL)
		return 0;
	deconstruct_array_builtin(a, TEXTOID, &d, NULL, &n);
	*out = palloc(Max(n, 1) * sizeof(char *));
	for (int i = 0; i < n; i++)
		(*out)[i] = TextDatumGetCString(d[i]);
	return n;
}

static void
setup(Test *t, const SiftColRel *r, ArrayType *preds, int group_att, ArrayType *children, ArrayType *epreds, bool hint)
{
	char	  **txt;
	int			n = texts_of(preds, &txt);
	int			nv = 0;

	t->spec.group_leaf = -1;
	for (int i = 0; i < n; i++)
	{
		int			att;
		int64		val;

		parse_pred(txt[i], &att, &t->rt[t->nrt].op, &val);
		t->rt[t->nrt].slot = add_col(t, r, att);
		t->rt[t->nrt].val = val;
		t->vals[nv].v.i = val;
		t->rpred[t->nrt] = (SiftColPred) {.col = r->first[att],.op = t->rt[t->nrt].op,.nvals = 1,.vals = &t->vals[nv++]};
		t->nrt++;
	}
	if (group_att > 0)
	{
		const int	group = leaf_of_group(r, group_att);
		int			nc = children != NULL ? ArrayGetNItems(ARR_NDIM(children), ARR_DIMS(children)) : 0;
		const int32 *ords = children != NULL ? (const int32 *) ARR_DATA_PTR(children) : NULL;

		t->spec.group_leaf = group;
		for (int c = 0; c < nc; c++)
		{
			const int	leaf = leaf_of_child(r, group_att, group, ords[c]);

			t->spec.child[t->spec.nchild++] = (SiftF2PullCol) {.leaf = leaf,.type = vec_type_of(&r->schema->cols[leaf])};
			if (t->first_int_child < 0 && t->spec.child[c].type == SVEC_I64)
				t->first_int_child = c;
			if (t->spec.child[c].type == SVEC_DICT32 && t->ncodes < 2)
				t->code_child[t->ncodes++] = c;
		}
		n = texts_of(epreds, &txt);
		for (int i = 0; i < n; i++)
		{
			int			child;
			int64		val;

			parse_pred(txt[i], &child, &t->et[t->net].op, &val);
			t->et[t->net].slot = child;
			t->et[t->net].val = val;
			t->vals[nv].v.i = val;
			t->epred[t->net] = (SiftColPred) {.col = t->spec.child[child].leaf,.op = t->et[t->net].op,.nvals = 1,.vals = &t->vals[nv++]};
			t->net++;
		}
	}
	if (hint)
	{
		t->spec.npred = t->nrt;
		t->spec.pred = t->rpred;
		t->spec.nelem_pred = t->net;
		t->spec.elem_pred = t->epred;
	}
}

/* the live rows of the batch that pass the predicates of the rows */
static void
parent_mask(const Test *t, const SiftF2ExtBatch *b, uint64 *mask)
{
	memset(mask, 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint32 i = 0; i < b->n; i++)
	{
		bool		ok = bit_at(b->live, i);

		for (int k = 0; ok && k < t->nrt; k++)
		{
			const SiftVec *v = &b->col[t->rt[k].slot];

			ok = (v->all_valid || bit_at(v->valid, i)) && holds(t->rt[k].op, ((const int64 *) v->data)[i], t->rt[k].val);
		}
		mask[i >> 6] |= (uint64) ok << (i & 63);
	}
}

static int64
first_col(const Test *t, const SiftF2ExtBatch *b, uint32 i)
{
	const SiftVec *v = t->nrt > 0 ? &b->col[t->rt[0].slot] : NULL;

	return v != NULL && (v->all_valid || bit_at(v->valid, i)) ? ((const int64 *) v->data)[i] : 0;
}

static int64
count_bits(const uint64 *m, uint32 n)
{
	return m == NULL ? 0 : sift_unnest_count_bits(m, 0, n);
}

static void
grow(Test *t, uint32 n_elems)
{
	const uint32 words = n_elems / 64 + 2;

	if (words > t->ecap)
	{
		t->ecap = words;
		t->emask = t->emask == NULL ? palloc(words * sizeof(uint64)) : repalloc(t->emask, words * sizeof(uint64));
		t->hits = t->hits == NULL ? palloc(words * sizeof(uint64)) : repalloc(t->hits, words * sizeof(uint64));
	}
}

static bool
child_int(const SiftF2ElemStream *es, int c, uint32 e, int64 *v)
{
	*v = ((const int64 *) es->child_data[c])[e];
	return bit_at(es->child_valid[c], e);
}

/* hits bit e of [lo, hi) = the element passes the predicates of the elements and the mask given (NULL: any element) */
static void
element_hits(const Test *t, const SiftF2ElemStream *es, const uint64 *only, uint32 lo, uint32 hi, uint64 *hits)
{
	memset(hits + (lo >> 6), 0, (((hi - 1) >> 6) - (lo >> 6) + 1) * sizeof(uint64));
	for (uint32 e = lo; e < hi; e++)
	{
		bool		ok = bit_at(only, e);

		for (int k = 0; ok && k < t->net; k++)
		{
			int64		v;

			ok = child_int(es, t->et[k].slot, e, &v) && holds(t->et[k].op, v, t->et[k].val);
		}
		hits[e >> 6] |= (uint64) ok << (e & 63);
	}
}

static void
add_group(Test *t, char *k0, char *k1, int64 n)
{
	for (int i = 0; i < t->ngroups; i++)
		if (((t->groups[i].k[0] == NULL) == (k0 == NULL)) && (k0 == NULL || strcmp(t->groups[i].k[0], k0) == 0) &&
			((t->groups[i].k[1] == NULL) == (k1 == NULL)) && (k1 == NULL || strcmp(t->groups[i].k[1], k1) == 0))
		{
			t->groups[i].n += n;
			return;
		}
	if (t->ngroups == lengthof(t->groups))
		elog(ERROR, "the test holds %d groups at most", (int) lengthof(t->groups));
	t->groups[t->ngroups++] = (GroupCell) {.k = {k0, k1},.n = n};
}

static char *
dict_str(const SiftGranuleDict *d, uint32 code)
{
	return pnstrdup(d->bytes + d->off[code], d->off[code + 1] - d->off[code]);
}

static char *
view_str(const SiftF2ElemStream *es, int c, uint32 e)
{
	const SiftStrView *v = &((const SiftStrView *) es->child_data[c])[e];

	return bit_at(es->child_valid[c], e) ? pnstrdup(sift_vstr_bytes(v), v->len) : NULL;
}

/* one slice of elements counted by the codes of the text children when the granule holds them as codes, else by their strings */
static void
count_groups(Test *t, const SiftF2ElemStream *es, const uint64 *sel, uint32 lo, uint32 hi)
{
	const int	c0 = t->code_child[0],
				c1 = t->ncodes > 1 ? t->code_child[1] : -1;
	const bool	coded = es->child_type[c0] == SVEC_DICT32 && (c1 < 0 || es->child_type[c1] == SVEC_DICT32);

	if (coded)
	{
		const SiftGranuleDict *d0 = es->child_dict[c0],
				   *d1 = c1 >= 0 ? es->child_dict[c1] : NULL;
		const uint32 stride = d1 != NULL ? d1->n + 1 : 1;
		uint64	   *cells = palloc0((uint64) (d0->n + 1) * stride * sizeof(uint64));

		sift_unnest_count_codes(es->child_data[c0], es->child_valid[c0], d1 != NULL ? es->child_data[c1] : NULL, d1 != NULL ? es->child_valid[c1] : NULL, d1 != NULL ? d1->n : 0, sel, lo, hi, cells);
		for (uint32 s0 = 0; s0 <= d0->n; s0++)
			for (uint32 s1 = 0; s1 < stride; s1++)
				if (cells[(uint64) s0 * stride + s1] != 0)
					add_group(t, s0 > 0 ? dict_str(d0, s0 - 1) : NULL, d1 != NULL && s1 > 0 ? dict_str(d1, s1 - 1) : NULL, cells[(uint64) s0 * stride + s1]);
		pfree(cells);
		return;
	}
	for (uint32 e = lo; e < hi; e++)
		if (bit_at(sel, e))
			add_group(t, view_str(es, c0, e), c1 >= 0 ? view_str(es, c1, e) : NULL, 1);
}

/* the rows passing the predicates whose array the index cannot hold: the layout is not there (the slab), or the row is poison */
static void
rows_for_heap(const SiftF2ExtBatch *b, const uint64 *mask, uint64 *fallback)
{
	const uint32 words = (b->n + 63) / 64;

	for (uint32 w = 0; w < words; w++)
		fallback[w] = b->elem->layout.n_parents < b->n ? mask[w] : mask[w] & b->elem->poison[w];
}

/* the parents the primitives leave to the heap are those whose array the index cannot hold */
static void
check_fallback(const uint64 *want, const uint64 *got, uint32 n)
{
	for (uint32 w = 0; w < (n + 63) / 64; w++)
		if (want[w] != got[w])
			elog(ERROR, "the parents left to the heap differ in word %u: %llx and %llx", w, (unsigned long long) got[w], (unsigned long long) want[w]);
}

/* the weight of every parent is the number of pairs the expansion makes of it */
static void
check_weights(const SiftUnnestIn *in, const uint32 *weight, uint32 n)
{
	uint32		pidx[SIFT_VEC_LEN],
				eidx[SIFT_VEC_LEN],
				seen[SIFT_VEC_LEN] = {0};
	uint64		unused[SIFT_VEC_WORDS] = {0};
	SiftUnnestCursor cur = {0};

	while (!sift_unnest_done(in, &cur))
	{
		const uint32 m = sift_unnest_expand(in, &cur, pidx, eidx, SIFT_VEC_LEN, unused);

		for (uint32 k = 0; k < m; k++)
			seen[pidx[k]]++;
	}
	for (uint32 p = 0; p < n; p++)
		if (seen[p] != weight[p])
			elog(ERROR, "parent %u has weight %u and gives %u pairs", p, weight[p], seen[p]);
}

static void
run_group_batch(Test *t, const char *mode, const SiftF2ExtBatch *b, const uint64 *mask)
{
	const SiftF2ElemStream *es = b->elem;
	uint64		fallback[SIFT_VEC_WORDS] = {0};
	uint64		hit[SIFT_VEC_WORDS];
	uint64		answered[SIFT_VEC_WORDS] = {0};
	uint32		weight[SIFT_VEC_LEN];
	SiftUnnestIn in = {.lay = &es->layout,.parent_mask = mask,.poison = es->poison,.doc_valid = es->doc_valid};
	uint32		lo = 0,
				hi = 0;

	rows_for_heap(b, mask, fallback);
	t->fallback += count_bits(fallback, b->n);
	if (es->layout.n_parents < b->n)
		return;
	if (strncmp(mode, "weights", 7) == 0)
	{
		in.left = strcmp(mode, "weights_left") == 0;
		t->out[0] += sift_unnest_weights(&in, weight, answered);
		check_weights(&in, weight, b->n);
		check_fallback(fallback, answered, b->n);
		t->out[1] += count_bits(mask, b->n) - count_bits(fallback, b->n);
		for (uint32 i = 0; i < b->n; i++)
			t->out[3] += (int64) weight[i] * first_col(t, b, i);
		return;
	}
	grow(t, es->n_elems);
	if (strcmp(mode, "semi") == 0 || strcmp(mode, "any") == 0)
	{
		if (es->layout.offsets[b->n] > es->layout.offsets[0])
			element_hits(t, es, NULL, es->layout.offsets[0], es->layout.offsets[b->n], t->hits);
		in.elem_mask = t->hits;
		if (strcmp(mode, "any") == 0)
		{
			uint64		known[SIFT_VEC_WORDS];

			sift_unnest_any(&es->layout, t->hits, mask, es->doc_valid, hit, known);
			for (uint32 w = 0; w < (uint32) (b->n + 63) / 64; w++)
			{
				hit[w] &= ~fallback[w];
				known[w] &= ~fallback[w];
			}
			t->out[0] += count_bits(hit, b->n);
			t->out[1] += count_bits(known, b->n);
			return;
		}
		t->out[0] += sift_unnest_semi(&in, hit, answered);
		check_fallback(fallback, answered, b->n);
		t->out[1] += count_bits(mask, b->n) - count_bits(fallback, b->n);
		return;
	}
	lo = es->layout.offsets[0];
	hi = es->layout.offsets[b->n];
	if (hi <= lo)
		return;
	element_hits(t, es, NULL, lo, hi, t->hits);
	in.elem_mask = t->hits;
	t->out[0] += sift_unnest_elem_mask(&in, t->emask, &lo, &hi);
	if (strcmp(mode, "codes") == 0)
		count_groups(t, es, t->emask, lo, hi);
	for (uint32 e = lo; strcmp(mode, "elems") == 0 && t->first_int_child >= 0 && e < hi; e++)
	{
		int64		v;

		if (bit_at(t->emask, e) && child_int(es, t->first_int_child, e, &v))
			t->out[1] += v;
	}
}

static void
run_batch(Test *t, const char *mode, const SiftF2ExtBatch *b)
{
	uint64		mask[SIFT_VEC_WORDS];

	parent_mask(t, b, mask);
	if (strcmp(mode, "rows") != 0)
	{
		run_group_batch(t, mode, b, mask);
		return;
	}
	for (uint32 i = 0; i < b->n; i++)
	{
		t->out[0] += bit_at(mask, i);
		t->out[1] += bit_at(mask, i) ? first_col(t, b, i) : 0;
		t->out[2] += bit_at(b->live, i);
	}
}

static int
group_cmp(const void *a, const void *b)
{
	const GroupCell *x = a,
			   *y = b;

	for (int i = 0; i < 2; i++)
	{
		const char *p = x->k[i] != NULL ? x->k[i] : "~",
				   *q = y->k[i] != NULL ? y->k[i] : "~";
		int			c = strcmp(p, q);

		if (c != 0)
			return c;
	}
	return 0;
}

static char *
groups_text(Test *t)
{
	StringInfoData s;

	initStringInfo(&s);
	qsort(t->groups, t->ngroups, sizeof(GroupCell), group_cmp);
	for (int i = 0; i < t->ngroups; i++)
		appendStringInfo(&s, "%s%s|%s=" INT64_FORMAT, i > 0 ? ";" : "", t->groups[i].k[0] != NULL ? t->groups[i].k[0] : "~",
						 t->ncodes > 1 ? (t->groups[i].k[1] != NULL ? t->groups[i].k[1] : "~") : "", t->groups[i].n);
	return s.data;
}

Datum
sift_test_f2_pull(PG_FUNCTION_ARGS)
{
	Relation	index = index_open(sift_rel_must_exist(PG_GETARG_OID(0)), AccessShareLock);
	const char *mode = text_to_cstring(PG_GETARG_TEXT_PP(1));
	Test	   *t = palloc0(sizeof(Test));
	SiftF2Pull *pull;
	const SiftF2ExtBatch *b;
	const SiftF2PullStats *st;
	Datum		v[16];
	bool		n[16] = {0};

	t->first_int_child = -1;
	setup(t, sift_col_rel(index), PG_ARGISNULL(3) ? NULL : PG_GETARG_ARRAYTYPE_P(3), PG_GETARG_INT32(4), PG_ARGISNULL(5) ? NULL : PG_GETARG_ARRAYTYPE_P(5),
		  PG_ARGISNULL(6) ? NULL : PG_GETARG_ARRAYTYPE_P(6), PG_GETARG_BOOL(2));
	if (strncmp(mode, "weights", 7) == 0 && t->spec.nchild > 0)
		elog(ERROR, "weights read no element column");
	if (strcmp(mode, "codes") == 0 && t->ncodes == 0)
		elog(ERROR, "codes count at least one text child");
	if (strcmp(mode, "rows") != 0 && strncmp(mode, "weights", 7) != 0 && strcmp(mode, "elems") != 0 && strcmp(mode, "codes") != 0 && strcmp(mode, "semi") != 0 && strcmp(mode, "any") != 0)
		elog(ERROR, "unknown mode \"%s\"", mode);
	pull = sift_f2_pull_begin(index, GetActiveSnapshot(), &t->spec);
	while ((b = sift_f2_pull_next(pull)) != NULL)
		run_batch(t, mode, b);
	st = sift_f2_pull_stats(pull);
	for (int i = 0; i < 4; i++)
		v[i] = Int64GetDatum(t->out[i]);
	v[4] = Int64GetDatum(t->fallback);
	v[5] = CStringGetTextDatum(groups_text(t));
	v[6] = Int64GetDatum(st->partitions_skipped);
	v[7] = Int64GetDatum(st->parts_skipped);
	v[8] = Int64GetDatum(st->granules_key_skipped);
	v[9] = Int64GetDatum(st->granules_zone_skipped);
	v[10] = Int64GetDatum(st->granules_elem_skipped);
	v[11] = Int64GetDatum(st->granules_elem_elided);
	v[12] = Int64GetDatum(st->granules_read);
	v[13] = Int64GetDatum(st->child_dict_granules);
	v[14] = Int64GetDatum(st->child_str_granules);
	v[15] = Int64GetDatum(st->child_chunks);
	sift_f2_pull_end(pull);
	index_close(index, AccessShareLock);
	return sift_result_row(fcinfo, v, n);
}

#endif							/* SIFT_TESTING */
