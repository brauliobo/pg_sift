/*
 * auxfn_facets.c - sift_facets(table, facet, query[, lim[, nulls]]): the matches of a query per value of a keyword-analyzer table.
 *
 * The virtual table of auxfn.c's facets half; sq_facets_init registers it (called by sq_aux_init).
 */
#include "vtab.h"
#include "core/cmp.h"
#include "core/memtrack.h"

SQLITE_EXTENSION_INIT3

/* ---------- facets ---------- */

/*
 * The memory of a facets call follows the data and has no plan to give way to: the pages with a match (4 bytes each and 40 for each
 * superblock), the values of the facet table (a record each) and the rows of the answer are held in all, up to work_mem
 * (sift_work_mem(), 1 GiB), or the call fails naming it.
 *
 * The matches are kept as one group per superblock. A value is counted against them by its postings, which skips the superblocks
 * the matches do not reach (the postings of those are not decoded); the values come out of the segments of the facet table in term
 * order, merged by a heap, and a bounded answer keeps the best `lim` of them in a heap, skipping a value whose most possible matches
 * cannot enter it.
 */
static void
facets_budget(Size bytes)
{
	sift_budget_check(bytes, work_mem, "sift_facets", "sift_work_mem()", "a narrower query");
}

typedef struct QSb					/* the matches of one superblock */
{
	uint32		sb;
	int64_t		row;				/* first of its pages in Matches.mask */
	uint64		pages[SIFT_PAGEMASK_WORDS];
} QSb;

typedef struct Matches
{
	QSb		   *sb;
	uint32	   *mask;				/* offsets 1..32 of each page (rowids map to those only), in page order */
	sift_grp   *grp;				/* per sb, once all pages are in */
	int64_t		nsb,
				capsb,
				nmask,
				capmask;
	int64_t		total;
} Matches;

static void
add_page(void *arg, BlockNumber blk, const uint64 *offmask)
{
	Matches    *m = arg;
	uint32		sb = sift_block_sb(blk),
				page = blk & (SIFT_SB_PAGES - 1);

	if (m->nsb == 0 || m->sb[m->nsb - 1].sb != sb)
	{
		if (m->nsb == m->capsb)
		{
			m->capsb = m->capsb ? m->capsb * 2 : 64;
			facets_budget((Size) m->capsb * sizeof(QSb) + (Size) m->capmask * sizeof(uint32));
			m->sb = m->sb ? repalloc_huge(m->sb, m->capsb * sizeof(QSb)) : palloc(m->capsb * sizeof(QSb));
		}
		m->sb[m->nsb++] = (QSb) {.sb = sb,.row = m->nmask};
	}
	m->sb[m->nsb - 1].pages[page / 64] |= UINT64CONST(1) << (page % 64);
	if (m->nmask == m->capmask)
	{
		m->capmask = m->capmask ? m->capmask * 2 : 1024;
		facets_budget((Size) m->capsb * sizeof(QSb) + (Size) m->capmask * sizeof(uint32));
		m->mask = m->mask ? repalloc_huge(m->mask, m->capmask * sizeof(uint32)) : palloc(m->capmask * sizeof(uint32));
	}
	m->mask[m->nmask++] = (uint32) offmask[0];
	m->total += pg_popcount32((uint32) offmask[0]);
}

/* First superblock of the matches at or after position lo whose number is at least sb */
static int64_t
sb_lower(const Matches *m, int64_t lo, uint32 sb)
{
	int64_t		hi = m->nsb;

	SIFT_BISECT(int64_t, mid, lo, hi, m->sb[mid].sb < sb);
	return lo;
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
	m->grp = palloc(Max(m->nsb, 1) * sizeof(sift_grp));
	for (int64_t i = 0; i < m->nsb; i++)
	{
		m->grp[i] = (sift_grp) {.gid = m->sb[i].sb,.w = sizeof(uint32),.off = (const uint8 *) &m->mask[m->sb[i].row]};
		memcpy(m->grp[i].pages, m->sb[i].pages, sizeof(m->grp[i].pages));
	}
	return m;
}

/* A value of a segment of the facet table: its term (in FacetCtx.terms) and where its postings are */
typedef struct Entry
{
	uint64		off;
	uint32		len;
	SiftTermInfo info;
} Entry;

typedef struct FacetSeg
{
	SiftSegment *seg;
	bool		dirty;				/* some doc was deleted: the matches are filtered by liveness (seg_view) */
	const sift_grp **view;			/* per superblock of the matches: its live part; NULL once looked at: none */
	bool	   *viewed;
	int64_t		from,				/* its entries, ascending terms */
				to;
} FacetSeg;

typedef struct Res					/* a value and its matches */
{
	const char *term;
	int			len;
	int64_t		n;
} Res;

typedef struct FacetCtx
{
	const Matches *m;
	MemoryContext tmpcx;			/* one value's cursor */
	FacetSeg   *seg;
	int			nseg;
	char	   *terms;
	size_t		nterms,
				termcap;
	Entry	   *ent;
	int64_t		nent,
				entcap;
	int64_t	   *member;				/* the entries of each value, by value */
	int		   *member_seg;
	int64_t		nmember;
	Res		   *res;				/* the answer: a heap, worst at the root, once it is full (lim >= 0), else in order of arrival */
	int64_t		nres,
				lim;
	bool		bounded;			/* the answer is cut to lim while collecting */
} FacetCtx;

static Size
facets_bytes(const FacetCtx *fc)
{
	return (Size) fc->m->capsb * sizeof(QSb) + (Size) fc->m->capmask * sizeof(uint32) + fc->termcap +
		(Size) fc->entcap * sizeof(Entry) + (Size) fc->nres * sizeof(Res);
}

static inline const char *
ent_term(const FacetCtx *fc, const Entry *e)
{
	return fc->terms + e->off;
}

static bool
dict_visit(void *arg, const char *term, int len, const SiftTermInfo *info)
{
	FacetCtx   *fc = arg;

	if (fc->nent == fc->entcap)
	{
		fc->entcap = fc->entcap ? fc->entcap * 2 : 256;
		facets_budget(facets_bytes(fc));
		fc->ent = fc->ent ? repalloc_huge(fc->ent, fc->entcap * sizeof(Entry)) : palloc(fc->entcap * sizeof(Entry));
	}
	if (fc->nterms + len > fc->termcap)
	{
		fc->termcap = Max(4096, 2 * fc->termcap + len);
		facets_budget(facets_bytes(fc));
		fc->terms = fc->terms ? repalloc_huge(fc->terms, fc->termcap) : palloc(fc->termcap);
	}
	memcpy(fc->terms + fc->nterms, term, len);
	fc->ent[fc->nent++] = (Entry) {.off = fc->nterms,.len = len,.info = *info};
	fc->nterms += len;
	CHECK_FOR_INTERRUPTS();
	return true;
}

/* Is the document of key in the group (offsets of 4 bytes a page) */
static bool
grp_has(const sift_grp *g, SiftKey key)
{
	uint32		page = sift_key_block(key) & (SIFT_SB_PAGES - 1),
				off = (key & 0xFFFF) - 1,
				row = 0,
				m;

	if (!(g->pages[page / 64] >> (page % 64) & 1))
		return false;
	for (int w = 0; w < page / 64; w++)
		row += pg_popcount64(g->pages[w]);
	row += pg_popcount64(g->pages[page / 64] & ((UINT64CONST(1) << (page % 64)) - 1));
	memcpy(&m, g->off + (size_t) row * sizeof(uint32), sizeof(m));
	return m >> off & 1;
}

/* The matches of superblock i restricted to the live docs of the segment; NULL when none is left */
static const sift_grp *
seg_view(const FacetCtx *fc, FacetSeg *fs, int64_t i)
{
	const Matches *m = fc->m;
	const sift_grp *q = &m->grp[i];
	uint32	   *rows;
	sift_grp   *g;
	uint64		pages[SIFT_PAGEMASK_WORDS] = {0};
	int			n = 0,
				idx = 0;

	if (!fs->dirty || !sift_seg_sb_dirty(fs->seg, q->gid))
		return q;
	if (fs->viewed[i])
		return fs->view[i];
	fs->viewed[i] = true;
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		n += pg_popcount64(q->pages[wd]);
	rows = palloc(n * sizeof(uint32));
	n = 0;
	for (int wd = 0; wd < SIFT_PAGEMASK_WORDS; wd++)
		for (uint64 pm = q->pages[wd]; pm; pm &= pm - 1, idx++)
		{
			int			page = wd * 64 + pg_rightmost_one_pos64(pm);
			uint64		mask[SIFT_OFFMASK_WORDS] = {m->mask[m->sb[i].row + idx]};

			if (sift_seg_live_mask(fs->seg, q->gid, page, mask) > 0 && (uint32) mask[0] != 0)
			{
				rows[n++] = (uint32) mask[0];
				pages[wd] |= UINT64CONST(1) << (page & 63);
			}
		}
	if (n == 0)
		return NULL;
	g = palloc(sizeof(sift_grp));
	*g = (sift_grp) {.gid = q->gid,.w = sizeof(uint32),.off = (const uint8 *) rows};
	memcpy(g->pages, pages, sizeof(pages));
	fs->view[i] = g;
	return g;
}

/* The matches among the docs of a term kept inline in the dictionary */
static int64_t
count_inline(const FacetCtx *fc, FacetSeg *fs, const SiftTermInfo *info)
{
	const Matches *m = fc->m;
	int64_t		n = 0;

	for (uint32 k = 0; k < info->df; k++)
	{
		SiftKey		key = info->inline_keys[k];
		uint32		sb = sift_block_sb(sift_key_block(key));
		int64_t		i = sb_lower(m, 0, sb);
		const sift_grp *g;

		if (i < m->nsb && m->sb[i].sb == sb && (g = seg_view(fc, fs, i)) != NULL && grp_has(g, key))
			n++;
	}
	return n;
}

/* The matches among the postings of a term: its groups against the matches', leapfrogging both */
static int64_t
count_postings(const FacetCtx *fc, FacetSeg *fs, const SiftTermInfo *info)
{
	const Matches *m = fc->m;
	SiftPostingCursor *pc = sift_post_open(&fs->seg->sr, info, fc->tmpcx);
	sift_cur   *cur = sift_post_cursor(pc);
	int64_t		n = 0,
				i = 0;

	while (i < m->nsb)
	{
		const sift_grp *g = sift_seek(cur, m->sb[i].sb);
		const sift_grp *q;

		if (g == NULL)
			break;
		i = sb_lower(m, i, g->gid);
		if (i < m->nsb && m->sb[i].sb == g->gid)
		{
			if ((q = seg_view(fc, fs, i)) != NULL)
				n += sift_grp_and_count(q, g);
			i++;
		}
	}
	sift_sr_release(&fs->seg->sr);
	MemoryContextReset(fc->tmpcx);
	return n;
}

/* The answer's order: more matches first, then the smaller term */
static int
res_cmp(const Res *a, const Res *b)
{
	return a->n != b->n ? (a->n < b->n) - (a->n > b->n) : sift_term_cmp(a->term, a->len, b->term, b->len);
}

static int
res_qsort_cmp(const void *a, const void *b)
{
	return res_cmp(a, b);
}

static void
res_up(Res *res, int64_t i)
{
	while (i > 0 && res_cmp(&res[(i - 1) / 2], &res[i]) < 0)
	{
		Res			t = res[i];

		res[i] = res[(i - 1) / 2];
		res[(i - 1) / 2] = t;
		i = (i - 1) / 2;
	}
}

static void
res_down(Res *res, int64_t n, int64_t i)
{
	for (;;)
	{
		int64_t		w = i,
					l = 2 * i + 1;

		if (l < n && res_cmp(&res[w], &res[l]) < 0)
			w = l;
		if (l + 1 < n && res_cmp(&res[w], &res[l + 1]) < 0)
			w = l + 1;
		if (w == i)
			return;
		Res			t = res[i];

		res[i] = res[w];
		res[w] = t;
		i = w;
	}
}

static void
res_add(FacetCtx *fc, Res r)
{
	if (fc->bounded && fc->nres == fc->lim)
	{
		if (fc->lim > 0 && res_cmp(&r, &fc->res[0]) < 0)
		{
			fc->res[0] = r;
			res_down(fc->res, fc->nres, 0);
		}
		return;
	}
	if (fc->nres % 256 == 0)
	{
		facets_budget(facets_bytes(fc) + 256 * sizeof(Res));
		fc->res = fc->res ? repalloc_huge(fc->res, (fc->nres + 256) * sizeof(Res)) : palloc(256 * sizeof(Res));
	}
	fc->res[fc->nres++] = r;
	if (fc->bounded)
		res_up(fc->res, fc->nres - 1);
}

/* No value of at most `bound` matches can beat the worst of a full answer */
static bool
cannot_enter(const FacetCtx *fc, const char *term, int len, int64_t bound)
{
	Res			r = {term, len, bound};

	return fc->bounded && fc->nres == fc->lim && (fc->lim == 0 || res_cmp(&r, &fc->res[0]) > 0);
}

/* A value (one term of the facet table) with the entries it has in the segments and the most matches it can have */
typedef struct Value
{
	int64_t		first;				/* its members in FacetCtx.member */
	int			n;
	int64_t		bound;
} Value;

/* The matches of a value: its postings in every segment of the facet table that has it */
static int64_t
count_value(const FacetCtx *fc, const Value *v)
{
	int64_t		c = 0;

	for (int k = 0; k < v->n; k++)
	{
		const Entry *e = &fc->ent[fc->member[v->first + k]];
		FacetSeg   *fs = &fc->seg[fc->member_seg[v->first + k]];

		c += (e->info.flags & SIFT_TERM_INLINE) ? count_inline(fc, fs, &e->info) : count_postings(fc, fs, &e->info);
	}
	return c;
}

/* The segment whose next entry has the smallest term, in a heap of the segments with entries left */
static bool
seg_before(const FacetCtx *fc, const int64_t *at, int a, int b)
{
	const Entry *x = &fc->ent[at[a]],
			   *y = &fc->ent[at[b]];

	return sift_term_cmp(ent_term(fc, x), x->len, ent_term(fc, y), y->len) < 0;
}

static void
seg_down(const FacetCtx *fc, const int64_t *at, int *heap, int n, int i)
{
	for (;;)
	{
		int			w = i,
					l = 2 * i + 1;

		if (l < n && seg_before(fc, at, heap[l], heap[w]))
			w = l;
		if (l + 1 < n && seg_before(fc, at, heap[l + 1], heap[w]))
			w = l + 1;
		if (w == i)
			return;
		int			t = heap[i];

		heap[i] = heap[w];
		heap[w] = t;
		i = w;
	}
}

/* The values in term order: the segments' entries merged by a heap, those of one term made one value */
static Value *
collect_values(FacetCtx *fc, int64_t *nvals)
{
	int64_t	   *at = palloc(Max(fc->nseg, 1) * sizeof(int64_t));
	int		   *heap = palloc(Max(fc->nseg, 1) * sizeof(int));
	Value	   *vals = palloc(Max(fc->nent, 1) * sizeof(Value));
	int			nheap = 0;

	facets_budget(facets_bytes(fc) + (Size) fc->nent * (sizeof(Value) + sizeof(int64_t) + sizeof(int)));
	fc->member = palloc(Max(fc->nent, 1) * sizeof(int64_t));
	fc->member_seg = palloc(Max(fc->nent, 1) * sizeof(int));
	fc->nmember = 0;
	*nvals = 0;
	for (int s = 0; s < fc->nseg; s++)
	{
		at[s] = fc->seg[s].from;
		if (at[s] < fc->seg[s].to)
			heap[nheap++] = s;
	}
	for (int i = nheap / 2 - 1; i >= 0; i--)
		seg_down(fc, at, heap, nheap, i);
	while (nheap > 0)
	{
		const Entry *first = &fc->ent[at[heap[0]]];
		Value	   *v = &vals[(*nvals)++];

		*v = (Value) {.first = fc->nmember};
		while (nheap > 0)
		{
			const Entry *e = &fc->ent[at[heap[0]]];

			if (v->n > 0 && sift_term_cmp(ent_term(fc, e), e->len, ent_term(fc, first), first->len) != 0)
				break;
			fc->member[fc->nmember] = at[heap[0]];
			fc->member_seg[fc->nmember++] = heap[0];
			v->n++;
			v->bound += e->info.df;
			if (++at[heap[0]] == fc->seg[heap[0]].to)
				heap[0] = heap[--nheap];
			seg_down(fc, at, heap, nheap, 0);
		}
		v->bound = Min(v->bound, fc->m->total);
	}
	return vals;
}

static int
value_by_bound(const void *a, const void *b)
{
	const Value *x = a,
			   *y = b;

	return x->bound != y->bound ? (x->bound < y->bound) - (x->bound > y->bound) : (x->first > y->first) - (x->first < y->first);
}

/*
 * Counts the values against the matches; returns the matches that have a value. A bounded answer takes the values by their
 * most possible matches, best first, and stops when the best that is left cannot enter it.
 */
static int64_t
count_values(FacetCtx *fc, bool nulls)
{
	int64_t		nvals,
				counted = 0;
	Value	   *vals = collect_values(fc, &nvals);

	if (fc->bounded)
		qsort(vals, nvals, sizeof(Value), value_by_bound);
	for (int64_t i = 0; i < nvals; i++)
	{
		const Value *v = &vals[i];
		const Entry *e = &fc->ent[fc->member[v->first]];
		int64_t		n;

		if (fc->bounded && fc->nres == fc->lim && (fc->lim == 0 || v->bound < fc->res[0].n))
			break;
		if (!nulls && cannot_enter(fc, ent_term(fc, e), e->len, v->bound))
			continue;
		CHECK_FOR_INTERRUPTS();
		if ((n = count_value(fc, v)) > 0)
		{
			counted += n;
			res_add(fc, (Res) {ent_term(fc, e), e->len, n});
		}
	}
	return counted;
}

/* The values of the facet table with their matches, best first: at most lim of them (all when lim < 0); *counted the matches that have a value */
static Res *
facet_values(SqVtab *ft, const Matches *m, int64_t lim, bool nulls, int64_t *nres, int64_t *counted)
{
	FacetCtx	fc = {.m = m,.lim = lim,.bounded = lim >= 0 && !nulls};
	const SiftDirSnapshot *dir;

	sq_store_refresh(ft->st, true);
	sq_vt_flush(ft, false);
	dir = sq_store_dir(ft->st);
	fc.tmpcx = AllocSetContextCreate(CurrentMemoryContext, "sift facet value", ALLOCSET_DEFAULT_SIZES);
	fc.seg = palloc(Max(dir->nsegs, 1) * sizeof(FacetSeg));
	for (int i = 0; i < dir->nsegs; i++)
	{
		FacetSeg   *fs = &fc.seg[fc.nseg++];

		fs->seg = sift_seg_open(sq_store_rel(ft->st), &dir->segs[i], CurrentMemoryContext);
		fs->dirty = (dir->segs[i].flags & SIFT_SEG_DIRTY) != 0;
		if (fs->dirty)
		{
			fs->view = palloc0(Max(m->nsb, 1) * sizeof(sift_grp *));
			fs->viewed = palloc0(Max(m->nsb, 1));
		}
		fs->from = fc.nent;
		sift_dict_scan(fs->seg->dict, NULL, 0, NULL, 0, dict_visit, &fc);
		sift_sr_release(&fs->seg->sr);
		fs->to = fc.nent;
	}
	*counted = count_values(&fc, nulls);
	for (int i = 0; i < fc.nseg; i++)
		sift_seg_close(fc.seg[i].seg);
	MemoryContextDelete(fc.tmpcx);
	qsort(fc.res, fc.nres, sizeof(Res), res_qsort_cmp);
	*nres = fc.nres;
	return fc.res;
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
	Res		   *res;
	int64_t		nres,
				counted;
	SqStore    *prev;

	prev = sq_store_enter(vt->st);
	m = query_matches(vt, q, qlen);
	sq_store_leave(prev);
	prev = sq_store_enter(ft->st);
	res = facet_values(ft, m, lim, nulls, &nres, &counted);
	sq_store_leave(prev);
	nres = lim >= 0 ? Min(nres, lim) : nres;
	facets_budget((Size) (nres + 1) * sizeof(FacetRow));
	c->rows = palloc((nres + 1) * sizeof(FacetRow));
	for (int64_t i = 0; i < nres; i++)
		c->rows[c->nrows++] = (FacetRow) {pnstrdup(res[i].term, res[i].len), res[i].n};
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
