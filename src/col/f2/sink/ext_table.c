/*
 * col/f2/sink/ext_table.c - the pull of a sift_table (col/f2/sink/ext.h): its granules come from the scan of its store (col/f2/store/scan.h), whose versions, deletes and column deltas decide the
 * rows the snapshot sees, as views of the leaves the spec names. The layout of each array is made from the view of its lengths leaf and the element columns from the views of its element leaves,
 * which hold a value for each element of the granule; a row whose array the leaves cannot hold (a document that is no array there, a list longer than the table keeps, a member of a type its leaf
 * cannot store) is poison, and the consumer reads it again by its address.
 */
/* check-memory: allow-allocs the arrays of a granule live in its context (SIFT_GRANULE_ROWS rows and their elements) */
#include "postgres.h"

#include "utils/memutils.h"

#include "agg/vexpr/str.h"
#include "col/f2/sink/ext_int.h"
#include "col/f2/store/scan.h"
#include "col/f2/store/store.h"

struct PullTable
{
	SiftStoreScanOpts o;
	SiftStoreScan *scan;
	List	   *run;
	int			at;				/* the next part of the run */
	SiftStorePart *part;
	SiftF2Batch b;				/* the granule being sliced, valid until the next granule of its part */
};

static void
put_value(SiftVec *v, void *data, const ColView *cv, uint32 at, uint32 j, bool bools)
{
	switch (v->type)
	{
		case SVEC_F64:
			((double *) data)[j] = cv->kind == SIFT_CELL_F8 ? cv->f[at] : (double) cv->i[at];
			break;
		case SVEC_STRVIEW:
		case SVEC_DICT32:
			((SiftStrView *) data)[j] = bools ? sift_f2_pull_bool_text(cv->i[at]) : sift_vstr_make(cv->s[at], cv->slen[at]);
			break;
		case SVEC_I32:
			((int32 *) data)[j] = (int32) cv->i[at];
			break;
		case SVEC_BOOL:
			((uint8 *) data)[j] = cv->i[at] != 0;
			break;
		default:
			((int64 *) data)[j] = cv->i[at];
			break;
	}
}

/* the rows [off, off + cnt) of column i from the view of its leaf */
static void
fill_col(SiftF2Pull *p, int i, uint16 off, uint16 cnt)
{
	const ColView *cv = &p->tbl->b.views[p->spec.col[i].leaf];
	SiftVec    *v = &p->vec[i];
	bool		nulls = false;

	memset(p->valid[i], 0, SIFT_VEC_WORDS * sizeof(uint64));
	for (uint16 j = 0; j < cnt; j++)
	{
		const bool	ok = !cv->isnull[off + j];

		p->valid[i][j >> 6] |= (uint64) ok << (j & 63);
		nulls |= !ok;
		if (ok)
			put_value(v, v->data, cv, off + j, j, p->bools[i] != NULL);
	}
	v->valid = nulls ? p->valid[i] : NULL;
	v->all_valid = !nulls;
	v->len = cnt;
	v->dense = true;
	v->sel = NULL;
}

static void
table_fill_slice(SiftF2Pull *p, uint16 off, uint16 cnt)
{
	for (int i = 0; i < p->spec.ncols; i++)
		fill_col(p, i, off, cnt);
}

static const char *
table_payload(SiftF2Pull *p, int i, uint32 at, uint32 *len)
{
	const ColView *cv = &p->tbl->b.views[p->spec.col[i].leaf];

	if (cv->xpay == NULL || cv->xpay[at] == NULL)
		return NULL;
	*len = cv->xplen[at];
	return cv->xpay[at];
}

/* the layout of array g from its lengths: a row whose document holds no array there is poison, one with no document has none */
static void
table_layout(SiftF2Pull *p, int g)
{
	PullGroup  *gr = &p->grp[g];
	const int	gl = p->spec.group[g].leaf;
	const ColView *cv = gl >= 0 ? &p->tbl->b.views[gl] : NULL;
	uint32	   *lengths = palloc(Max(p->n, 1) * sizeof(uint32));
	uint32	   *offsets = palloc((p->n + 1) * sizeof(uint32));
	uint32	   *blocks = palloc(((p->n + 63) / 64 + 1) * sizeof(uint32));

	gr->doc_valid = palloc0(((p->n + 63) / 64 + 1) * sizeof(uint64));
	gr->has_layout = cv != NULL;
	gr->n_elems = 0;
	if (cv == NULL)
	{
		sift_f2_pull_poison_granule(p);
		return;
	}
	offsets[0] = 0;
	for (uint32 r = 0; r < p->n; r++)
	{
		lengths[r] = cv->isnull[r] ? 0 : (uint32) cv->i[r];
		offsets[r + 1] = offsets[r] + lengths[r];
		p->poison[r >> 6] |= (uint64) (cv->tag != NULL && cv->tag[r] != 0) << (r & 63);
		gr->doc_valid[r >> 6] |= (uint64) !cv->isnull[r] << (r & 63);
	}
	for (uint32 k = 0; k <= (p->n + 63) / 64; k++)
		blocks[k] = offsets[Min(k * 64, p->n)];
	gr->lay = (SiftArrayLayout) {.n_parents = p->n,.n_elems = offsets[p->n],.block_elem_off = blocks,.lengths = lengths,.offsets = offsets};
	gr->n_elems = offsets[p->n];
}

/* the cell of element e of a kind column: a value of the leaf, a JSON null or a missing member */
static int64
elem_kind(const ColView *cv, uint32 e)
{
	const uint8 tag = cv->tag != NULL ? cv->tag[e] : 0;

	return !cv->isnull[e] ? SIFT_ELEM_KIND_VALUE : tag == SIFT_COLTAG_JNULL ? SIFT_ELEM_KIND_JNULL : SIFT_ELEM_KIND_ABSENT;
}

/* a member of another JSON type than its leaf's, or of an element that is no object: the leaf holds no value of it */
static bool
elem_unheld(const ColView *cv, uint32 e)
{
	return cv->isnull[e] && cv->tag != NULL && cv->tag[e] != 0 && cv->tag[e] != SIFT_COLTAG_JNULL;
}

/* element column c of array g, a value for each element of the granule: a row one of whose elements the leaf cannot hold is poison */
static void
table_child(SiftF2Pull *p, int g, int c)
{
	PullGroup  *gr = &p->grp[g];
	const SiftF2PullCol *pc = &p->spec.group[g].child[c];
	const ColView *cv = &p->tbl->b.views[pc->leaf];
	const uint32 n = gr->n_elems;
	SiftVec		v = {.type = pc->kind ? SVEC_I64 : pc->type == SVEC_DICT32 ? SVEC_STRVIEW : pc->type};

	gr->child_type[c] = v.type;
	gr->child_dict[c] = NULL;
	gr->child_valid[c] = palloc0(((n + 63) / 64 + 1) * sizeof(uint64));
	gr->child_data[c] = palloc_aligned(Max((Size) n * sizeof(SiftStrView), 1) + SIFT_VEC_PAD, SIFT_VEC_ALIGN, 0);
	for (uint32 r = 0; r < p->n; r++)
		for (uint32 e = gr->lay.offsets[r]; e < gr->lay.offsets[r + 1]; e++)
		{
			if (pc->kind)
				((int64 *) gr->child_data[c])[e] = elem_kind(cv, e);
			else if (!cv->isnull[e])
				put_value(&v, gr->child_data[c], cv, e, e, false);
			p->poison[r >> 6] |= (uint64) (!pc->kind && elem_unheld(cv, e)) << (r & 63);
			gr->child_valid[c][e >> 6] |= (uint64) (pc->kind || !cv->isnull[e]) << (e & 63);
		}
}

/* the rows whose leaf of a key of a document the statement reads hold a value of another JSON type than the leaf's: the consumer reads them from the heap */
static void
poison_key_rows(SiftF2Pull *p)
{
	for (int i = 0; i < p->spec.ncols; i++)
	{
		const ColView *cv = &p->tbl->b.views[p->spec.col[i].leaf];

		for (uint32 r = 0; p->key_leaf[p->spec.col[i].leaf] && cv->tag != NULL && r < p->n; r++)
			p->poison[r >> 6] |= (uint64) (cv->tag[r] != 0 && cv->tag[r] != SIFT_COLTAG_JNULL) << (r & 63);
	}
}

static void
table_granule(SiftF2Pull *p)
{
	const SiftF2Batch *b = &p->tbl->b;
	const uint32 words = (b->n + 63) / 64 + 1;
	MemoryContext old;

	MemoryContextReset(p->gcx);
	old = MemoryContextSwitchTo(p->gcx);
	p->n = b->n;
	p->off = 0;
	p->tids = b->tid;
	p->live = palloc0(words * sizeof(uint64));
	memcpy(p->live, b->live, ((b->n + 63) / 64) * sizeof(uint64));
	p->poison = palloc0(words * sizeof(uint64));
	for (int g = 0; g < p->spec.ngroups; g++)
	{
		table_layout(p, g);
		for (int c = 0; p->grp[g].has_layout && c < p->spec.group[g].nchild; c++)
			table_child(p, g, c);
	}
	if (p->spec.ngroups > 0)
		poison_key_rows(p);
	p->stats.granules_read++;
	MemoryContextSwitchTo(old);
}

/* the next granule of the parts of the runs of the store; false after the last */
static bool
table_advance(SiftF2Pull *p)
{
	PullTable  *t = p->tbl;

	for (;;)
	{
		if (t->part != NULL && sift_f2_store_part_next(t->part, &t->b))
		{
			table_granule(p);
			return true;
		}
		if (t->at < list_length(t->run))
		{
			t->part = list_nth(t->run, t->at++);
			continue;
		}
		t->part = NULL;
		if ((t->run = sift_f2_store_scan_next_run(t->scan, false)) == NIL)
			return false;
		t->at = 0;
	}
}

static void
table_end(SiftF2Pull *p)
{
	sift_f2_store_scan_close(p->tbl->scan);
}

static const SiftF2PullHost table_host = {.advance = table_advance,.fill_slice = table_fill_slice,.payload = table_payload,.end = table_end,.visible = true};

static int
add_leaf(int *leaves, int n, int leaf)
{
	for (int i = 0; i < n; i++)
		if (leaves[i] == leaf)
			return n;
	leaves[n] = leaf;
	return n + 1;
}

/* the leaves of the spec the scan of the store decodes */
static int
spec_leaves(const SiftF2PullSpec *spec, int *leaves)
{
	int			n = 0;

	for (int i = 0; i < spec->ncols; i++)
		n = add_leaf(leaves, n, spec->col[i].leaf);
	for (int g = 0; g < spec->ngroups; g++)
		for (int c = -1; spec->group[g].leaf >= 0 && c < spec->group[g].nchild; c++)
			n = add_leaf(leaves, n, c < 0 ? spec->group[g].leaf : spec->group[g].child[c].leaf);
	return n;
}

void
sift_f2_pull_table_begin(SiftF2Pull *p)
{
	SiftF2Store *store = sift_f2_store_open(p->index);
	PullTable  *t = palloc0(sizeof(PullTable));
	int		   *leaves = palloc((SIFT_F2_PULL_MAX_COLS + SIFT_F2_PULL_MAX_GROUPS * (1 + SIFT_F2_PULL_MAX_CHILD)) * sizeof(int));

	SIFT_INVARIANT(p->spec.nmasks == 0 && p->spec.gran_sink == NULL, "the pull of a sift_table reads no mask and offers no granule");
	t->o = (SiftStoreScanOpts) {.rel = p->index,.store = store,.snap = p->snap,.leaves = leaves,.nleaves = spec_leaves(&p->spec, leaves)};
	t->scan = sift_f2_store_scan_open(&t->o);
	p->tbl = t;
	p->host = &table_host;
	p->kinds = store->kinds;
	p->key_leaf = sift_f2_path_key_leaves(store);
}
