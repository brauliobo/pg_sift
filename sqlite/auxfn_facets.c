/*
 * auxfn_facets.c - sift_facets(table, facet, query[, lim[, nulls]]): the matches of a query per value of a keyword-analyzer table.
 *
 * The virtual table of auxfn.c's facets half; sq_facets_init registers it (called by sq_aux_init).
 */
#include "vtab.h"
#include "core/cmp.h"

SQLITE_EXTENSION_INIT3

/* ---------- facets ---------- */

typedef struct Matches
{
	BlockNumber *blk;
	uint32	   *mask;					/* offsets 1..32 of the block (rowids map to those only) */
	int64_t		n,
				cap;
	int64_t		total;
} Matches;

static void
add_page(void *arg, BlockNumber blk, const uint64 *offmask)
{
	Matches    *m = arg;

	if (m->n == m->cap)
	{
		m->cap = m->cap ? m->cap * 2 : 1024;
		m->blk = m->blk ? repalloc_huge(m->blk, m->cap * sizeof(BlockNumber)) : palloc(m->cap * sizeof(BlockNumber));
		m->mask = m->mask ? repalloc_huge(m->mask, m->cap * sizeof(uint32)) : palloc(m->cap * sizeof(uint32));
	}
	m->blk[m->n] = blk;
	m->mask[m->n++] = (uint32) offmask[0];
	m->total += pg_popcount32((uint32) offmask[0]);
}

static bool
is_match(const Matches *m, SiftKey key)
{
	BlockNumber blk = sift_key_block(key);
	int64_t		lo = 0,
				hi = m->n;

	SIFT_BISECT(int64_t, mid, lo, hi, m->blk[mid] < blk);
	return lo < m->n && m->blk[lo] == blk && (m->mask[lo] >> ((key & 0xFFFF) - 1) & 1);
}

typedef struct Facet
{
	char	   *value;
	int			len;
	int64_t		n;
} Facet;

typedef struct FacetCtx
{
	const Matches *m;
	Facet	   *f;
	int64_t		nf,
				cap;
} FacetCtx;

static void
count_term(void *arg, const char *term, int len, const SiftKey *keys, uint32 n)
{
	FacetCtx   *fc = arg;
	int64_t		c = 0;

	for (uint32 i = 0; i < n; i++)
		c += is_match(fc->m, keys[i]);
	if (c == 0)
		return;
	if (fc->nf == fc->cap)
	{
		fc->cap = fc->cap ? fc->cap * 2 : 64;
		fc->f = fc->f ? repalloc_huge(fc->f, fc->cap * sizeof(Facet)) : palloc(fc->cap * sizeof(Facet));
	}
	fc->f[fc->nf++] = (Facet) {memcpy(palloc(len), term, len), len, c};
}

static int
facet_cmp(const void *a, const void *b)
{
	const Facet *x = a,
			   *y = b;
	int			c = memcmp(x->value, y->value, Min(x->len, y->len));

	return c ? c : x->len - y->len;
}

static int
facet_by_count(const void *a, const void *b)
{
	const Facet *x = a,
			   *y = b;

	return x->n != y->n ? (x->n < y->n) - (x->n > y->n) : facet_cmp(a, b);
}

/* The same value in several segments of the facet table: one row */
static int64_t
combine(Facet *f, int64_t n)
{
	int64_t		out = 0;

	qsort(f, n, sizeof(Facet), facet_cmp);
	for (int64_t i = 0; i < n; i++)
	{
		if (out > 0 && facet_cmp(&f[out - 1], &f[i]) == 0)
			f[out - 1].n += f[i].n;
		else
			f[out++] = f[i];
	}
	return out;
}

static Matches *
query_matches(SqVtab *vt, const char *q, int qlen)
{
	Matches    *m = palloc0(sizeof(Matches));
	SiftIndexState *state;
	SiftQuery  *qs[1];
	SiftPlan   *plan;
	SiftVis		vis;

	sq_store_refresh(vt->st, true);
	sq_vt_flush(vt, false);
	state = sift_state_open(sq_store_rel(vt->st), CurrentMemoryContext);
	qs[0] = sift_state_query(state, q, qlen);
	plan = sift_plan_bind(state, qs, 1, 0);
	memset(&vis, 0, sizeof(vis));
	vis.use_vm = true;
	sift_eval_pages(plan, &vis, sq_vt_verify, vt, add_page, m);
	return m;
}

static Facet *
facet_values(SqVtab *ft, const Matches *m, int64_t *nf)
{
	FacetCtx	fc = {.m = m};
	const SiftDirSnapshot *dir;

	sq_store_refresh(ft->st, true);
	sq_vt_flush(ft, false);
	dir = sq_store_dir(ft->st);
	for (int i = 0; i < dir->nsegs; i++)
		sq_seg_walk_terms(ft->st, &dir->segs[i], count_term, &fc);
	*nf = combine(fc.f, fc.nf);
	qsort(fc.f, *nf, sizeof(Facet), facet_by_count);
	return fc.f;
}

typedef struct FacetRow
{
	char	   *value;					/* NULL: the matches without a value */
	int64_t		n;
} FacetRow;

typedef struct FacetCursor
{
	sqlite3_vtab_cursor base;
	FacetRow   *rows;
	int64_t		nrows,
				at;
	MemoryContext cx;
} FacetCursor;

typedef struct FacetVtab
{
	sqlite3_vtab base;
	SqConn	   *conn;
} FacetVtab;

#define FC_VALUE	0
#define FC_COUNT	1
#define FC_TABLE	2
#define FC_FACET	3
#define FC_QUERY	4
#define FC_LIMIT	5
#define FC_NULLS	6

static int
fcConnect(sqlite3 *db, void *aux, int argc, const char *const *argv, sqlite3_vtab **ppVtab, char **pzErr)
{
	FacetVtab  *v = sqlite3_malloc(sizeof(FacetVtab));
	int			rc = sqlite3_declare_vtab(db, "CREATE TABLE x(value, n, \"table\" HIDDEN, facet HIDDEN, q HIDDEN, lim HIDDEN, nulls HIDDEN)");

	if (v == NULL)
		return SQLITE_NOMEM;
	memset(v, 0, sizeof(*v));
	v->conn = aux;
	*ppVtab = &v->base;
	return rc;
}

static int
fcDisconnect(sqlite3_vtab *p)
{
	sqlite3_free(p);
	return SQLITE_OK;
}

static int
fcBestIndex(sqlite3_vtab *p, sqlite3_index_info *info)
{
	int			argc = 0;
	int			need = 0;

	for (int i = 0; i < info->nConstraint; i++)
	{
		const struct sqlite3_index_constraint *c = &info->aConstraint[i];

		if (c->usable && c->op == SQLITE_INDEX_CONSTRAINT_EQ && c->iColumn >= FC_TABLE)
		{
			info->aConstraintUsage[i].argvIndex = c->iColumn - FC_TABLE + 1;
			info->aConstraintUsage[i].omit = 1;
			argc = Max(argc, c->iColumn - FC_TABLE + 1);
			need |= 1 << (c->iColumn - FC_TABLE);
		}
	}
	if ((need & 7) != 7)
		return SQLITE_CONSTRAINT;
	info->idxNum = need;
	info->estimatedCost = 1000;
	return SQLITE_OK;
}

static int
fcOpen(sqlite3_vtab *p, sqlite3_vtab_cursor **pp)
{
	FacetCursor *c = sqlite3_malloc(sizeof(FacetCursor));

	if (c == NULL)
		return SQLITE_NOMEM;
	memset(c, 0, sizeof(*c));
	*pp = &c->base;
	return SQLITE_OK;
}

static void
fc_reset(FacetCursor *c)
{
	if (c->cx)
		MemoryContextDelete(c->cx);
	c->cx = NULL;
	c->rows = NULL;
	c->nrows = c->at = 0;
}

static int
fcClose(sqlite3_vtab_cursor *cur)
{
	fc_reset((FacetCursor *) cur);
	sqlite3_free(cur);
	return SQLITE_OK;
}

/* The rows, in the cursor's context: values by descending count, then the NULL row if nulls is set and some matches have no value */
static void
facet_rows(FacetCursor *c, SqVtab *vt, SqVtab *ft, const char *q, int qlen, int64_t lim, bool nulls)
{
	Matches    *m;
	Facet	   *f;
	int64_t		nf,
				counted = 0,
				n;
	SqStore    *prev;

	prev = sq_store_enter(vt->st);
	m = query_matches(vt, q, qlen);
	sq_store_leave(prev);
	prev = sq_store_enter(ft->st);
	f = facet_values(ft, m, &nf);
	sq_store_leave(prev);
	for (int64_t i = 0; i < nf; i++)
		counted += f[i].n;
	n = lim >= 0 ? Min(nf, lim) : nf;
	c->rows = palloc((n + 1) * sizeof(FacetRow));
	for (int64_t i = 0; i < n; i++)
		c->rows[c->nrows++] = (FacetRow) {pnstrdup(f[i].value, f[i].len), f[i].n};
	if (nulls && m->total > counted)
		c->rows[c->nrows++] = (FacetRow) {NULL, m->total - counted};
}

static int
fcFilter(sqlite3_vtab_cursor *cur, int idxNum, const char *idxStr, int argc, sqlite3_value **argv)
{
	FacetCursor *c = (FacetCursor *) cur;
	FacetVtab  *v = (FacetVtab *) cur->pVtab;
	SqVtab	   *vt,
			   *ft;
	int			rc = SQLITE_OK;
	MemoryContext old,
				call;

	fc_reset(c);
	vt = sq_find_table(v->conn, (const char *) sqlite3_value_text(argv[0]));
	ft = sq_find_table(v->conn, (const char *) sqlite3_value_text(argv[1]));
	if (vt == NULL || ft == NULL)
	{
		sqlite3_free(v->base.zErrMsg);
		v->base.zErrMsg = sqlite3_mprintf("sift_facets: no such sift table: %s", vt == NULL ? sqlite3_value_text(argv[0]) : sqlite3_value_text(argv[1]));
		return SQLITE_ERROR;
	}
	old = sq_enter();
	call = CurrentMemoryContext;
	c->cx = AllocSetContextCreate(NULL, "sift facets", ALLOCSET_DEFAULT_SIZES);
	SQ_TRY();
	MemoryContextSwitchTo(c->cx);
	facet_rows(c, vt, ft, (const char *) sqlite3_value_text(argv[2]), sqlite3_value_bytes(argv[2]),
			   (idxNum & 8) ? sqlite3_value_int64(argv[3]) : -1, (idxNum & 16) && sqlite3_value_int(argv[4]));
	SQ_CATCH();
	sqlite3_free(v->base.zErrMsg);
	v->base.zErrMsg = sqlite3_mprintf("%s", sq_errmsg());
	sq_store_recover(vt->st);
	sq_store_recover(ft->st);
	rc = SQLITE_ERROR;
	SQ_END();
	MemoryContextSwitchTo(call);
	sq_leave(old);
	return rc;
}

static int
fcNext(sqlite3_vtab_cursor *cur)
{
	((FacetCursor *) cur)->at++;
	return SQLITE_OK;
}

static int
fcEof(sqlite3_vtab_cursor *cur)
{
	FacetCursor *c = (FacetCursor *) cur;

	return c->at >= c->nrows;
}

static int
fcColumn(sqlite3_vtab_cursor *cur, sqlite3_context *ctx, int i)
{
	FacetCursor *c = (FacetCursor *) cur;

	if (i == FC_VALUE && c->rows[c->at].value)
		sqlite3_result_text(ctx, c->rows[c->at].value, -1, SQLITE_TRANSIENT);
	else if (i == FC_COUNT)
		sqlite3_result_int64(ctx, c->rows[c->at].n);
	return SQLITE_OK;
}

static int
fcRowid(sqlite3_vtab_cursor *cur, sqlite3_int64 *r)
{
	*r = ((FacetCursor *) cur)->at;
	return SQLITE_OK;
}

static const sqlite3_module facet_module = {
	.iVersion = 3,
	.xConnect = fcConnect,
	.xBestIndex = fcBestIndex,
	.xDisconnect = fcDisconnect,
	.xOpen = fcOpen,
	.xClose = fcClose,
	.xFilter = fcFilter,
	.xNext = fcNext,
	.xEof = fcEof,
	.xColumn = fcColumn,
	.xRowid = fcRowid,
};

int
sq_facets_init(sqlite3 *db, SqConn *conn)
{
	return sqlite3_create_module(db, "sift_facets", &facet_module, conn);
}
