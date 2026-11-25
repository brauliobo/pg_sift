/*
 * col/f2/f2_scan.c - the scan of an index of format major 2: the quals are turned into predicates on leaves (col/amscan/preds.c), the partitions, parts and granules the key range and
 * the zones rule out are not read (f2_prune.c), the rest are decoded for the leaves the predicates name only, and the TIDs of the rows that pass are returned, those the deletion
 * vectors and the pending deletes say are dead left out. A row is returned exact when every predicate was decided on its stored values, so the heap need not test the quals again;
 * a row a leaf could not hold (a wide number, an overflow value, a pattern the stored bytes cannot judge) is returned for the heap to decide. The heap still checks visibility.
 */
#include "postgres.h"

#include "access/relscan.h"
#include "access/transam.h"
#include "catalog/pg_operator_d.h"
#include "access/skey.h"
#include "access/stratnum.h"
#include "catalog/pg_type_d.h"
#include "miscadmin.h"
#include "pgstat.h"
#include "portability/instr_time.h"
#include "utils/memutils.h"
#include "utils/snapmgr.h"

#include "col/f2/scan/bridge_core.h"
#include "col/f2/scan/ord_stream.h"
#include "col/f2/scan/par_int.h"
#include "col/f2/f2_cache.h"
#include "col/f2/f2_col_delta_idx.h"
#include "col/f2/f2_part_pin_scan.h"
#include "col/f2/pin_win.h"
#include "col/f2/f2_point_dir.h"
#include "col/f2/f2_pull_prune.h"
#include "col/f2/f2_scope_route.h"
#include "col/f2/f2_unique.h"
#include "col/f2/f2_view.h"
#include "col/granule/cfilt_str_simd.h"
#include "col/skip/skip_index.h"
#include "core/memtrack.h"
#include "core/popcount.h"
#include "core/errctx.h"
#include "core/meta_peek.h"
#include "col/f2/f2_vacuum_run.h"
#include "core/quarantine.h"
#include "col/f2/l0_stage.h"
#include "col/f2/scan/stream.h"
#include "col/f2/part_vis.h"

#ifdef SIFT_TESTING
SiftF2ScanStats sift_f2_scan_stats;
#endif

static void
sink_flush(F2TbmSink *k)
{
	tbm_add_tuples(k->tbm, k->exact, k->ne, false);
	tbm_add_tuples(k->tbm, k->maybe, k->nm, true);
	k->ne = k->nm = 0;
}

static void
sink_add(F2TbmSink *k, uint64 tid, bool maybe)
{
	sift_f2_tid_set(maybe ? &k->maybe[k->nm++] : &k->exact[k->ne++], tid);
	if (k->ne == F2_TID_BATCH || k->nm == F2_TID_BATCH)
		sink_flush(k);
}

static void
emit(F2Scan *s, const SiftColSchema *schema, uint64 tid, Verdict v, const ColView *views, int r)
{
	bool		maybe = v == V_MAYBE || s->recheck_all;

	if (maybe)
		F2_STAT(recheck);
	else
		F2_STAT(exact);
	if (s->tbm != NULL)
		sink_add(s->tbm, tid, maybe);
	else
		sift_f2_scan_sort_put(s, schema, tid, maybe, views, r);
	s->ntids++;
}

static int
tid_cmp(const void *a, const void *b)
{
	uint64		x = *(const uint64 *) a,
				y = *(const uint64 *) b;

	return x < y ? -1 : x > y;
}

/* The zones of the leaf of every predicate from granule g0 of the part; NULL for a leaf when the part holds none */
static const SiftZone **
pred_zones(Relation rel, const F2Scan *s, SiftF2PartPin *pin, uint32 g0)
{
	const SiftZone **z = palloc0(Max(s->npreds, 1) * sizeof(SiftZone *));

	for (int k = 0; k < s->npreds; k++)
	{
		const SiftZone *all = sift_f2_pin_zones(rel, pin, s->preds[k].col);

		z[k] = all != NULL ? all + g0 : NULL;
	}
	return z;
}

/* Whether no row of a granule with this zone sorts before the bound of the sink (nor ties with it, when a later sort column can tell them apart): a NULL cannot, unless NULLs sort first; a leaf a patch changed is not what the zone says */
static bool
bound_excludes(const SiftF2Bound *b, const SiftZone *z, bool exceptions)
{
	if (!b->active || exceptions || (z->flags & SIFT_ZONE_NONE) || (b->nulls_first && z->nnull > 0))
		return false;
	if (b->ties)
		return b->desc ? z->max < b->value : z->min > b->value;
	return b->desc ? z->max <= b->value : z->min >= b->value;
}

/* The rows of n of the leaf of a key that have it, from the rows the granule keeps of it */
static bool *
decode_keyp(const SiftGranuleView *v, int c, int n)
{
	bool	   *kp = palloc0(n);

	for (int off = 0; off < n; off += SIFT_VEC_LEN)
	{
		uint64		mask[SIFT_VEC_WORDS];
		uint16		cnt = Min(SIFT_VEC_LEN, n - off);
		SiftMaskState st = sift_granule_decode_key_present(v, c, off, cnt, mask);

		for (int i = 0; i < cnt; i++)
			kp[off + i] = st == SIFT_MASK_ALL_ONE || (st == SIFT_MASK_BITS && ((mask[i >> 6] >> (i & 63)) & 1));
	}
	return kp;
}

/* A column whose few values are listed in its chunk is not decoded for IS NOT NULL: the rows it lists are the ones to look at (at most 1/16 of the granule); -1: no predicate is of that kind */
static int
sparse_rows(Relation rel, const F2Scan *s, const SiftColSchema *schema, const SiftPart *part, const SiftGranuleDesc *gd, bool exceptions[], bool *decided, uint16 *rows)
{
	uint16		cap = gd != NULL ? gd->nrows / SIFT_NULL_SPARSE_MAX_DIV : 0;

	for (int k = 0; gd != NULL && k < s->npreds; k++)
	{
		const SiftColDef *d = &schema->cols[s->preds[k].col];
		uint16		n;

		if (decided[k] || s->preds[k].op != SIFT_COLOP_NOTNULL || d->parent >= 0 || d->lane > 0 || sift_col_def_is_wide(d) || exceptions[k])
			continue;
		n = sift_part_read_nonnull_rows(rel, part, gd, s->preds[k].col, rows, cap);
		if (n <= cap)
		{
			decided[k] = true;
			return n;
		}
	}
	return -1;
}

/* What a predicate is expected to pass of its rows and to cost for each, before the granules have shown it: the order of the first granules */
static void
prior_of(const SiftColDef *d, const SiftColPred *p, double *pass, double *cost)
{
	double		unit = d->store == SIFT_COLS_STR ? 1.5 : 0.8;

	switch (p->op)
	{
		case SIFT_COLOP_ISNULL:
		case SIFT_COLOP_NOTNULL:
			*pass = 0.5;
			*cost = 0.1;
			break;
		case SIFT_COLOP_EQ:
			*pass = 0.1;
			*cost = unit;
			break;
		case SIFT_COLOP_IN:
		case SIFT_COLOP_PREFIX:
			*pass = 0.2;
			*cost = unit;
			break;
		case SIFT_COLOP_NE:
		case SIFT_COLOP_NOTIN:
			*pass = 0.9;
			*cost = unit;
			break;
		case SIFT_COLOP_CONTAINS:
		case SIFT_COLOP_SUFFIX:
		case SIFT_COLOP_ICONTAINS:
			*pass = 0.2;
			*cost = 15.0;
			break;
		default:
			*pass = 0.5;
			*cost = unit;
	}
}

void
compile_preds(F2Scan *s, const SiftColSchema *schema)
{
	SiftConjunctInit init[SIFT_AFILT_MAX];

	s->cf = MemoryContextAlloc(s->cx, Max(s->npreds, 1) * sizeof(SiftCfiltPred));
	s->cf_ok = MemoryContextAllocZero(s->cx, Max(s->npreds, 1));
	s->psets = sift_f2_pred_sets_new(s->cx, s->npreds);
	for (int k = 0; k < s->npreds; k++)
	{
		const SiftColDef *d = &schema->cols[s->preds[k].col];

		s->cf_ok[k] = k < SIFT_AFILT_MAX && sift_cfilt_compile(&s->cf[k], d, &s->preds[k]);
		if (k < SIFT_AFILT_MAX)
		{
			init[k] = (SiftConjunctInit) {.col = s->preds[k].col};
			prior_of(d, &s->preds[k], &init[k].pass_prior, &init[k].cpu_ns_row);
		}
	}
	sift_lf_init(&s->lf, &sift_f2_granule_filter_ops, NULL, init, Min(s->npreds, SIFT_AFILT_MAX), false);
	s->compiled = true;
}

bool
sift_f2_part_cfilt(const SiftPartDicts *ds, const SiftColSchema *schema, const SiftVariantExcSummary *exc, int nexc, const SiftColPred *preds, int npreds, const bool *cf_ok,
				   SiftCfiltPred *cf)
{
	bool		meets = true;
	bool	   *col_has_exc = palloc0(schema->ncols);

	for (int i = 0; i < nexc; i++)
		col_has_exc[exc[i].col] = true;
	for (int k = 0; k < npreds; k++)
	{
		const SiftColPred *p = &preds[k];
		const SiftPartDict *dict = sift_part_dict_find(ds, p->col, SIFT_DICT_KIND_STR);

		if (dict != NULL && (SIFT_COLOP_IS_EXT(p->op) || (cf_ok[k] && cf[k].str)))
		{
			SiftDictPredCache *dc = sift_dict_pred_prepare(p, dict->n, dict->off, dict->bytes);
			cf[k].part = dc;
			meets &= dc->n_matches + dc->n_maybe > 0 || col_has_exc[p->col];
		}
		else if (cf_ok[k] && cf[k].str && (p->op == SIFT_COLOP_EQ || p->op == SIFT_COLOP_IN || p->op == SIFT_COLOP_PREFIX))
		{
			const SiftPartDict *fd = sift_part_dict_find(ds, p->col, SIFT_DICT_KIND_FSST);

			if (fd != NULL)
				cf[k].fsst_part = sift_fsst_pred_prepare(fd->fsst, p);
		}
	}
	return meets;
}

/* False when a predicate that no string of the dictionary of the part passes, on a leaf that no row of the part holds an exception of, rules out every row of the part */
static bool
part_preds(Relation rel, const F2Scan *s, const SiftColSchema *schema, const SiftPart *part, const SiftVariantExcSummary *exc, int nexc, PartPreds *pp)
{
	const SiftPartDicts *ds = sift_part_dicts(rel, part);

	pp->skip = sift_skip_open(rel, part);
	pp->sq = palloc0(Max(s->npreds, 1) * sizeof(SiftSkipQuery *));
	pp->cf = palloc(Max(s->npreds, 1) * sizeof(SiftCfiltPred));
	if (s->npreds > 0)
		memcpy(pp->cf, s->cf, s->npreds * sizeof(SiftCfiltPred));
	for (int k = 0; k < s->npreds; k++)
		pp->sq[k] = sift_skip_query(pp->skip, &schema->cols[s->preds[k].col], &s->preds[k], sift_part_dict_find(ds, s->preds[k].col, SIFT_DICT_KIND_STR));
	return sift_f2_part_cfilt(ds, schema, exc, nexc, s->preds, s->npreds, s->cf_ok, pp->cf);
}

/* What the skipping indexes say of the predicates the zones left, for granule g: false when a predicate rules the granule out */
static bool
probe_skips(Relation rel, const F2Scan *s, const SiftPart *part, const PartPreds *pp, const SiftZone *const *zones, uint32 g0, uint32 g, const bool *exceptions, bool *decided)
{
	for (int k = 0; k < s->npreds; k++)
	{
		SiftSkipVerdict v;

		if (pp->sq[k] == NULL || decided[k] || exceptions[k])
			continue;
		F2_STAT(skip_probed);
		v = sift_skip_probe(rel, part, pp->skip, pp->sq[k], g, zones[k] != NULL && zones[k][g - g0].nnull == 0);
		if (v == SIFT_SKIP_PRUNE)
		{
			F2_STAT(skip_pruned);
			return false;
		}
		if (v == SIFT_SKIP_ALL_MATCH)
			decided[k] = true;
	}
	return true;
}

/* The chunks the rows left by the chunks that were filtered still need: the columns of the predicates that were not decided, those an index-only scan returns, and the _tid */
static void
load_rest(Relation rel, const F2Scan *s, SiftLazyGranule *lg, const bool *decided)
{
	int		   *chunks = palloc((s->npreds + s->nret + s->nsink_leaves + 1) * sizeof(int));
	int			nc = 0;

	chunks[nc++] = SIFT_COL_TID_IDX;
	for (int k = 0; k < s->npreds; k++)
		if (!decided[k])
			chunks[nc++] = s->preds[k].col + 1;
	for (int j = 0; j < s->nret; j++)
		chunks[nc++] = s->ret_leaf[j] + 1;
	for (int j = 0; j < s->nsink_leaves; j++)
		chunks[nc++] = s->sink_leaves[j] + 1;
	sift_lazy_granule_load(rel, lg, chunks, nc);
}

/* Whether granule g is ruled out before it is read: by the tid bridge, by the scope router, by the zones of the predicates or by the skipping indexes; decided says which predicates they found true of every row of it */
static bool
granule_ruled_out(Relation rel, F2Scan *s, const SiftColSchema *schema, const SiftPart *part, const PartPreds *pp, const SiftGranuleDesc *dir, const SiftZone *const *zones, uint32 g0, uint32 g,
				  const SiftVariantExcSummary *exc, int nexc, const SiftTidIndexEntry *tix, bool *decided)
{
	const bool	bridge_miss = sift_f2_scan_bridge_granule_miss(s, g, tix);
	const bool	ruled = bridge_miss || (s->router != NULL && !sift_scope_router_wants(s->router, part->hdr.part_gen, g));
	bool		skip = ruled;
	bool	   *exceptions = palloc0(Max(s->npreds, 1));

	for (int k = 0; k < s->npreds; k++)
		exceptions[k] = sift_f2_exc_has(exc, nexc, g, s->preds[k].col);
	for (int k = 0; k < s->npreds && !skip; k++)
	{
		ZoneVerdict zv;

		if (zones[k] == NULL)
			continue;
		zv = sift_f2_zone_verdict(&schema->cols[s->preds[k].col], &s->preds[k], &zones[k][g - g0], dir != NULL ? dir[g].nrows : 0, exceptions[k]);
		skip = zv == Z_NONE;
		decided[k] = zv == Z_ALL;
	}
	if (s->bridge_seen)
		(*s->bridge_seen)++;
	if (bridge_miss && s->bridge_skipped)
		(*s->bridge_skipped)++;
	if (skip && !ruled)
		F2_STAT(zone_skipped);
	else if (!ruled && !probe_skips(rel, s, part, pp, zones, g0, g, exceptions, decided))
		skip = true;
	pfree(exceptions);
	return skip;
}

/* What a window of SIFT_PF_WINDOW_GRANULES granules of a span holds before it is read: which are ruled out, and the predicates the zones and the skipping indexes decided in the others */
typedef struct GranuleWindow
{
	bool		skipped[SIFT_PF_WINDOW_GRANULES];
	bool	   *decided;		/* SIFT_PF_WINDOW_GRANULES rows of npreds */
} GranuleWindow;

/* The granules of the window that stay are found first, and their pages are asked of the storage together (key range or not); a scan whose filter leaves most granules nothing reads no further ahead than the first window */
static void
prune_window(Relation rel, F2Scan *s, const SiftColSchema *schema, const SiftPart *part, const PartPreds *pp, const SiftGranuleDesc *dir, const SiftGranuleDesc *pf_dir, const SiftZone *const *zones,
			 uint32 g0, uint32 wb, uint32 wn, uint32 widx, const SiftVariantExcSummary *exc, int nexc, const SiftTidIndexEntry *tix, GranuleWindow *w)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 window");
	MemoryContext old = MemoryContextSwitchTo(cx);
	uint32		spans[SIFT_PF_WINDOW_SPANS][2];
	size_t		np = Max(s->npreds, 1);
	int			ns;

	memset(w->decided, 0, SIFT_PF_WINDOW_GRANULES * np);
	for (uint32 i = 0; i < wn; i++)
		w->skipped[i] = granule_ruled_out(rel, s, schema, part, pp, dir, zones, g0, wb + i, exc, nexc, tix, w->decided + i * np);
	ns = sift_pf_survivor_spans(w->skipped, wb, wn, spans);
	if (pf_dir != NULL && ns > 0 && sift_pf_window_wanted(widx, sift_afilt_dense(&s->lf.af)))
		F2_STAT_ADD(prefetched, sift_f2_prefetch_spans(rel, part, pf_dir, (const uint32 (*)[2]) spans, ns));
	MemoryContextSwitchTo(old);
	MemoryContextDelete(cx);
}

/* The rows set in the bitmap, into rows (SIFT_POINT_ROWS_MAX of them): their number, or 0 when there are more or when they are a quarter of the n rows or more, which the dense decode reads faster */
static int
active_rows(const uint64 *active, uint32 live, int n, uint16 *rows)
{
	int			m = 0;

	if (live * 4 >= (uint32) n)
		return 0;
	for (int w = 0; w < (n + 63) / 64; w++)
		for (uint64 bits = active[w]; bits != 0; bits &= bits - 1)
		{
			if (m == SIFT_POINT_ROWS_MAX)
				return 0;
			rows[m++] = w * 64 + __builtin_ctzll(bits);
		}
	return m;
}

/* Whether the rows the filter chunks left are the answer as they stand: a sink that reads no column and no address (count(*)), and every predicate decided */
static bool
bitmap_answers(const F2Scan *s, const bool *decided, bool pend_hit, bool few)
{
	if (s->sink == NULL || s->nsink_leaves > 0 || s->bridge != NULL || !s->tids_unneeded || pend_hit || few)
		return false;
	for (int k = 0; k < s->npreds; k++)
		if (!decided[k])
			return false;
	return true;
}

static void
sink_bitmap(F2Scan *s, const SiftColSchema *schema, const SiftDvGran *dg, Snapshot snap, const uint64 *active, int n)
{
	uint64		dead[SIFT_DV_MASK_WORDS],
				live[SIFT_CFILT_WORDS + 2] = {0},
				maybe[SIFT_CFILT_WORDS + 2] = {0};
	const int	words = (n + 63) / 64;

	if (dg != NULL && sift_dv_mask64(dg, snap, dead) > 0)
		for (int w = 0; w < words; w++)
			live[w] = active[w] & ~dead[w];
	else
		memcpy(live, active, words * sizeof(uint64));
	s->sink(s->sink_arg, &(SiftF2Batch) {.views = palloc0(schema->ncols * sizeof(ColView)),.live = live,.maybe = maybe,.scores = s->bscores,.part = s->part,.n = n});
}

/* The granules [g0, g1) of a pinned part, filtered on their chunks for the predicates that can be and decoded for the leaves of the others */
static void
scan_granules(Relation rel, F2Scan *s, const SiftColSchema *schema, SiftF2PartPin *pin, const PartPreds *pp, uint32 g0, uint32 g1, const SiftPendDelIndex *px, Snapshot snap)
{
	const SiftPart *part = pin->part;
	const SiftGranuleDesc *dir = pin->gdir;
	const SiftDv *dv = pin->dv;
	const SiftTidIndexEntry *tix = sift_f2_bridge_needs_tix(px != NULL, s->bridge) ? pin->tidx : NULL;
	const SiftVariantExcSummary *exc = pp->exc;
	int			nexc = pp->nexc;
	const SiftZone **zones = pred_zones(rel, s, pin, g0);
	const SiftZone *bound_zones = s->bound != NULL ? sift_f2_pin_zones(rel, pin, s->sink_leaves[s->bound_sink_leaf]) : NULL;
	MemoryContext gcx = sift_mem_allocset_create(s->cx, "sift f2 granule");
	size_t		np = Max(s->npreds, 1);
	GranuleWindow win = {.decided = palloc0((Size) SIFT_PF_WINDOW_GRANULES * np)};
	uint16		cand[SIFT_GRANULE_ROWS / SIFT_NULL_SPARSE_MAX_DIV + 1];
	const SiftZone **offer_zones = palloc0(np * sizeof(SiftZone *));
	bool	   *offer_exc = palloc0(np);

	for (uint32 g = g0; g < g1; g++)
	{
		uint32		wi = (g - g0) % SIFT_PF_WINDOW_GRANULES;
		MemoryContext old;
		bool	   *decided,
				   *exceptions;
		SiftLazyGranule lg;
		SiftGranuleView v;
		ColView    *views;
		uint64	   *tids;
		uint64		dead[SIFT_DV_MASK_WORDS],
					active[SIFT_CFILT_WORDS + 2] = {0},
					sink_live[SIFT_CFILT_WORDS + 2] = {0},
					sink_maybe[SIFT_CFILT_WORDS + 2] = {0};
		const SiftDvGran *dg = dv ? sift_dv_granule(dv, g) : NULL;
		bool		any_dead = false,
					few;
		bool		pend_hit = px != NULL && sift_pdel_idx_may_hit_granule(px, &tix[g]);
		int			n,
					ncand,
					nrows;
		uint32		live;
		uint16		arows[SIFT_POINT_ROWS_MAX];
		const uint16 *rows;

		if (wi == 0)
			prune_window(rel, s, schema, part, pp, dir, dir, zones, g0, g, Min(SIFT_PF_WINDOW_GRANULES, g1 - g), (g - g0) / SIFT_PF_WINDOW_GRANULES, exc, nexc, tix, &win);
		if (win.skipped[wi] || (bound_zones != NULL && bound_excludes(s->bound, &bound_zones[g], sift_f2_exc_has(exc, nexc, g, s->sink_leaves[s->bound_sink_leaf]))))
			continue;
		if (s->survey)
		{
			s->survey_granules++;
			continue;
		}
		if (s->gran_sink != NULL && s->bridge == NULL && s->cands == NULL)
		{
			for (int k = 0; k < s->npreds; k++)
			{
				offer_zones[k] = zones[k] != NULL ? &zones[k][g - g0] : NULL;
				offer_exc[k] = sift_f2_exc_has(exc, nexc, g, s->preds[k].col);
			}
			if (s->gran_sink(s->gran_arg, &(SiftF2GranOffer) {.rel = rel,.part = part,.desc = s->part,.schema = schema,.g = g,.gd = &dir[g],.dead = dg,.snap = snap,.preds = s->preds,.npreds = s->npreds,
							 .decided = win.decided + (Size) wi * np,.zones = offer_zones,.exceptions = offer_exc,.pend_hit = pend_hit,.patched = sift_cdi_scan_granule_patched(s, g),.px = px,.vis = SIFT_GVIS_HEAP}))
				continue;
		}
		if (s->pinwin != NULL && sift_pin_win_take(s->pinwin, rel, s, schema, pin, pp, g, win.decided + (Size) wi * np, px, snap))
			continue;
		old = MemoryContextSwitchTo(gcx);
		sift_cfstr_memo_open(gcx);
		decided = memcpy(palloc((Size) np), win.decided + (Size) wi * np, np);
		exceptions = palloc0(np);
		for (int k = 0; k < s->npreds; k++)
			exceptions[k] = sift_f2_exc_has(exc, nexc, g, s->preds[k].col);
		F2_STAT(read);
		ncand = s->cands != NULL ? sift_point_cands_rows(s->cands, g, cand, lengthof(cand)) : sparse_rows(rel, s, schema, part, dir != NULL ? &dir[g] : NULL, exceptions, decided, cand);
		if (ncand >= 0)
			F2_STAT(sparse);
		sift_lazy_granule_open_desc(rel, part, g, &dir[g], &lg);
		v = sift_granule_open_part(rel, part, lg.rec, lg.gd.data_len, &lg.gd);
		n = lg.gd.nrows;
		if (ncand >= 0 && s->cands != NULL)
		{
			const int	chunk = s->cands->pred->col + 1;

			sift_lazy_granule_load(rel, &lg, &chunk, 1);
			ncand = sift_point_cands_verify(s->cands, &v, cand, ncand);
			decided[s->cands->pred_idx] = true;
		}
		if (ncand >= 0)
			for (int i = 0; i < ncand; i++)
				active[cand[i] >> 6] |= UINT64CONST(1) << (cand[i] & 63);
		else
		{
			memset(active, 0xFF, n / 64 * sizeof(uint64));
			if (n & 63)
				active[n / 64] = (UINT64CONST(1) << (n & 63)) - 1;
		}
		live = s->bridge != NULL && sift_f2_scan_bridge_probe(rel, s, g, &lg, &v, dg, px, pend_hit, snap, active) == 0 ? 0 : sift_f2_filter_chunks(rel, s, g, &lg, &v, pp, exceptions, decided, active);
		if (live == 0)
		{
			F2_STAT(late_skipped);
			MemoryContextSwitchTo(old);
			MemoryContextReset(gcx);
			continue;
		}
		if (s->rowwin.hi != 0 && sift_f2_scan_window(s, active, n) == 0)
		{
			MemoryContextSwitchTo(old);
			MemoryContextReset(gcx);
			continue;
		}
		few = s->heap_decide && s->bridge != NULL && sift_f2_scan_few_rows(active, n);
		if (bitmap_answers(s, decided, pend_hit, few))
		{
			sink_bitmap(s, schema, dg, snap, active, n);
			MemoryContextSwitchTo(old);
			MemoryContextReset(gcx);
			continue;
		}
		if (!few)
			load_rest(rel, s, &lg, decided);
		nrows = active_rows(active, live, n, arows);
		rows = nrows > 0 ? arows : NULL;
		views = palloc0(schema->ncols * sizeof(ColView));
		for (int k = 0; k < s->npreds && !few; k++)
		{
			int			c = s->preds[k].col;

			if (!decided[k] && s->preds[k].op != SIFT_COLOP_KEY_EXISTS && views[c].isnull == NULL)
				views[c] = sift_f2_decode_view(&schema->cols[c], &v, c, n, false, rows, nrows);
		}
		for (int j = 0; j < s->nret && !few; j++)
			if (views[s->ret_leaf[j]].isnull == NULL)
				views[s->ret_leaf[j]] = sift_f2_decode_view(&schema->cols[s->ret_leaf[j]], &v, s->ret_leaf[j], n, false, rows, nrows);
		for (int j = 0; j < s->nsink_leaves && !few; j++)
		{
			const int	leaf = s->sink_leaves[j];

			if (views[leaf].isnull == NULL)
				views[leaf] = sift_f2_decode_view(&schema->cols[leaf], &v, leaf, n, s->sink_dict != NULL && s->sink_dict[j], rows, nrows);
			if (schema->cols[leaf].path[0] != '\0')
				sift_f2_decode_payloads(&v, leaf, n, &views[leaf]);
		}
		for (int k = 0; k < s->npreds && !few; k++)
			if (!decided[k] && s->preds[k].op == SIFT_COLOP_KEY_EXISTS && views[s->preds[k].col].keyp == NULL)
				views[s->preds[k].col].keyp = decode_keyp(&v, s->preds[k].col, n);
		tids = s->tids_unneeded && !pend_hit ? NULL : palloc(n * sizeof(uint64));
		if (tids != NULL)
			sift_granule_tids_head(&v, s->rowwin.fwd && s->cd == NULL ? sift_f2_scan_rows_to(active, n) : n, tids);
		pend_hit |= sift_cdi_scan_patch(s, schema, g, views, tids, n, gcx) && px != NULL;
		if (dg != NULL)
			any_dead = sift_dv_mask64(dg, snap, dead) > 0;
		MemoryContextSwitchTo(old);
		if (s->sink != NULL && s->npreds == 0 && !any_dead && !pend_hit && !few)
			memcpy(sink_live, active, (n + 63) / 64 * sizeof(uint64));
		else
			for (int w = 0; w < (n + 63) / 64; w++)
				for (uint64 bits = active[w]; bits != 0; bits &= bits - 1)
				{
					int			r = w * 64 + __builtin_ctzll(bits);
					Verdict		verdict;

					if (any_dead && ((dead[r >> 6] >> (r & 63)) & 1))
						continue;
					if (pend_hit && sift_pdel_idx_has(px, tids[r]))
						continue;
					verdict = few ? sift_f2_heap_verdict(s, decided) : sift_f2_row_verdict(schema, s, views, decided, r);
					if (s->sink != NULL)
						sift_f2_sink_verdict(s, verdict, sink_live, sink_maybe, r);
					else if (verdict != V_NO)
						emit(s, schema, tids[r], verdict, views, r);
				}
		if (s->sink != NULL)
			s->sink(s->sink_arg, &(SiftF2Batch) {.views = views,.tid = tids,.live = sink_live,.maybe = sink_maybe,.scores = s->bscores,.part = s->part,.n = n});
		MemoryContextReset(gcx);
	}
	MemoryContextDelete(gcx);
}

/* The granules [lo, hi) of a pinned part: those the point directory names for a predicate when it names some, else all */
void
scan_span(Relation rel, F2Scan *s, const SiftColSchema *schema, SiftF2PartPin *pin, const PartPreds *pp, uint32 lo, uint32 hi, const SiftPendDelIndex *px, Snapshot snap)
{
	const SiftPointCands *pc = s->cands;

	if (pc == NULL)
		scan_granules(rel, s, schema, pin, pp, lo, hi, px, snap);
	for (uint32 g = pc != NULL ? sift_point_cands_next(pc, lo) : hi; g < hi;)
	{
		uint32		end = g + 1;

		while (end < hi && sift_point_cands_next(pc, end) == end)
			end++;
		scan_granules(rel, s, schema, pin, pp, g, end, px, snap);
		g = sift_point_cands_next(pc, end);
	}
}

#define SIFT_DICT_TEST_NS	0.25	/* a code looked up in the bits of the strings of the dictionary that pass */

/* What an equality on an integer passes of the rows of the part at most: one of the values its zones span */
static double
eq_pass(Relation rel, SiftF2PartPin *pin, int col)
{
	const SiftZone *z = sift_f2_pin_zones(rel, pin, col);
	int64		lo = PG_INT64_MAX,
				hi = PG_INT64_MIN;

	if (z == NULL || pin->part->hdr.ngranules == 0)
		return 1.0;
	for (uint32 g = 0; g < pin->part->hdr.ngranules; g++)
	{
		if (z[g].flags & SIFT_ZONE_NONE)
			return 1.0;
		lo = Min(lo, z[g].min);
		hi = Max(hi, z[g].max);
	}
	return Max(1e-5, 1.0 / ((double) ((uint64) hi - (uint64) lo) + 1.0));
}

/* The order of the first granules, before any was measured: a predicate on a dictionary of the part costs a bit test and passes the share of its strings that match, an equality on an integer passes the
 * share its value takes of the range the zones span */
static void
price_filter(Relation rel, const F2Scan *s, SiftF2PartPin *pin, const PartPreds *pp, const SiftColSchema *schema, SiftAdaptiveFilter *af)
{
	SiftConjunctInit init[SIFT_AFILT_MAX];
	const int	n = Min(s->npreds, SIFT_AFILT_MAX);

	if (af->granules_seen > 0 || n == 0)
		return;
	for (int k = 0; k < n; k++)
	{
		const SiftConjunctState *st = &af->st[k];
		const SiftDictPredCache *dc = pp->cf[k].part;

		init[k] = (SiftConjunctInit) {.col = st->col,.barrier_group = st->barrier_group,.pass_prior = st->pass,.cpu_ns_row = st->cpu,.io_bytes_row = st->io};
		if (dc != NULL)
		{
			init[k].cpu_ns_row = SIFT_DICT_TEST_NS;
			init[k].pass_prior = (double) (dc->n_matches + dc->n_maybe) / Max(dc->dict_n, 1);
		}
		else if (s->preds[k].op == SIFT_COLOP_EQ && schema->cols[s->preds[k].col].store == SIFT_COLS_INT)
			init[k].pass_prior = Min(st->pass, eq_pass(rel, pin, s->preds[k].col));
	}
	sift_afilt_init(af, init, n, af->io_cold);
}

bool
part_open(Relation rel, F2Scan *s, const SiftColSchema *schema, const SiftPartitionEntry *e, SiftF2PartPin *pin, Snapshot snap, PartCx *cx)
{
	const bool	patched = (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0;
	const SiftPointDir *pd = s->npreds > 0 && !patched ? sift_f2_pin_point(rel, pin) : NULL;

	cx->pin = pin;
	cx->pp.exc = sift_f2_pin_exc(rel, pin, &cx->pp.nexc);
	cx->pp.exc = sift_cdi_scan_exceptions(s, rel, schema, e, pin->part, snap, cx->pp.exc, &cx->pp.nexc);
	if (!part_preds(rel, s, schema, pin->part, cx->pp.exc, cx->pp.nexc, &cx->pp))
	{
		F2_STAT(part_pruned);
		return false;
	}
	sift_f2_seed_filter(rel, pin->part, &pin->desc, s->preds, Min(s->npreds, SIFT_AFILT_MAX), &s->lf.af);
	price_filter(rel, s, pin, &cx->pp, schema, &s->lf.af);
	if (s->npreds > 0 && s->part_sink != NULL)
		s->part_sink(s->sink_arg, s->part, false, &s->tids_unneeded);
	s->tids_unneeded &= !patched;
	s->cands = pd != NULL && sift_point_dir_cands(rel, pin->part, pd, schema, s->preds, s->npreds, s->cx, &cx->pc) ? &cx->pc : NULL;
	return true;
}

/* The granules of a pinned part the key ranges and the zones leave */
static void
scan_pinned(Relation rel, F2Scan *s, const SiftF2Rel *f, const SiftColSchema *schema, const SiftPartitionEntry *e, SiftF2PartPin *pin, const SiftPendDelIndex *px, Snapshot snap)
{
	uint32		spans[SIFT_F2_MAX_RANGES][2];
	int			nspans;
	PartCx		cx;

	if (sift_f2_part_dicts_answer(rel, s, e, pin, px))
		return;
	nspans = sift_f2_granule_spans(rel, pin->part, f, &s->keys, &s->keyplan, spans);
	F2_STAT(parts_read);
	if (nspans == 0 || !part_open(rel, s, schema, e, pin, snap, &cx))
		return;
	for (int i = 0; i < nspans; i++)
		scan_span(rel, s, schema, pin, &cx.pp, spans[i][0], spans[i][1], px, snap);
	s->cands = NULL;
}

void
sift_f2_scan_set_zone_sink(IndexScanDesc scan, SiftF2ZoneSink zone_sink)
{
	((F2Scan *) scan->opaque)->zone_sink = zone_sink;
}

void
sift_f2_scan_set_bound(IndexScanDesc scan, int sink_leaf, const SiftF2Bound *bound)
{
	F2Scan	   *s = (F2Scan *) scan->opaque;

	s->bound = bound;
	s->bound_sink_leaf = sink_leaf;
}

static void
scan_part(Relation rel, F2Scan *s, const SiftF2Rel *f, const SiftColSchema *schema, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftPendDelIndex *px, Snapshot snap)
{
	SiftF2PartPin *pin = sift_f2_scan_pin(rel, d, (uint32) e->part_id);

	PG_TRY();
	{
		if (!sift_f2_zones_answer(rel, s, pin, e, d, px))
			scan_pinned(rel, s, f, schema, e, pin, px, snap);
	}
	PG_FINALLY();
	{
		sift_f2_pin_release(pin);
	}
	PG_END_TRY();
}

static bool
part_offered(F2Scan *s, const SiftPartitionEntry *e, const SiftPartDesc *d, const SiftPendDelIndex *px)
{
	bool		patched = (e->flags & SIFT_PARTENTRY_HAS_COL_DELTA) != 0;
	bool		answered;

	s->part = d;
	s->tids_unneeded = false;
	answered = s->part_sink != NULL && s->npreds == 0 && s->part_sink(s->sink_arg, d, px == NULL && s->bridge == NULL && d->dv_blk == InvalidBlockNumber && !patched, &s->tids_unneeded);
	s->tids_unneeded &= !patched;
	return answered;
}

void
scan_slab(Relation rel, F2Scan *s, const SiftF2Rel *f, const SiftColSchema *schema, const SiftPartitionEntry *e, const SiftPendDelIndex *px, Snapshot snap)
{
	MemoryContext slab_cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 scan slab");
	MemoryContext old = MemoryContextSwitchTo(slab_cx);
/* check-memory: allow bounded to one micro-slab: SIFT_L0_SLAB_MAX_PAGES pages */
	SiftRowBuf *rb = sift_rowbuf_new(&f->spec);
	BlockNumber *pages;
	int			npages;
	ColView    *views = MemoryContextAllocZero(slab_cx, schema->ncols * sizeof(ColView));
	bool	   *decided = MemoryContextAllocZero(slab_cx, Max(s->npreds, 1));
	uint64	   *live,
			   *maybe,
			   *bmask;

	sift_l0_slab_read(rel, e->part_id, e->l0_slab_blk, rb, &f->spec, snap, &pages, &npages);
	MemoryContextSwitchTo(old);
	for (int c = 0; c < f->ncols; c++)
		views[c] = (ColView) {.kind = rb->kinds[c],.i = rb->i[c],.f = rb->f[c],.s = (const char *const *) rb->s[c],.slen = rb->slen[c],.isnull = rb->isnull[c],.tag = rb->tag[c],.xpay = (const char *const *) rb->xpay[c],.xplen = rb->xplen[c]};
	for (int k = 0; k < s->npreds; k++)
		if (s->preds[k].op == SIFT_COLOP_KEY_EXISTS && views[s->preds[k].col].keyp == NULL)
		{
			ColView    *cv = &views[s->preds[k].col];
			bool	   *kp = MemoryContextAlloc(slab_cx, Max(rb->n, 1));

			for (int r = 0; r < rb->n; r++)
				kp[r] = sift_key_row_present(cv->isnull[r], cv->tag != NULL ? cv->tag[r] : 0);
			cv->keyp = kp;
		}
	bmask = sift_f2_scan_slab_bridge(s, rb->tid, rb->n);
	live = s->sink != NULL ? MemoryContextAllocZero(slab_cx, 2 * ((rb->n + 63) / 64 * sizeof(uint64) + sizeof(uint64))) : NULL;
	maybe = live != NULL ? live + (rb->n + 63) / 64 + 1 : NULL;
	for (int r = 0; r < rb->n; r++)
	{
		Verdict		verdict;

		if ((px != NULL && sift_pdel_idx_has(px, rb->tid[r])) || (bmask != NULL && !((bmask[r >> 6] >> (r & 63)) & 1)))
			continue;
		verdict = sift_f2_row_verdict(schema, s, views, decided, r);
		if (s->sink != NULL)
			sift_f2_sink_verdict(s, verdict, live, maybe, r);
		else if (verdict != V_NO)
			emit(s, schema, rb->tid[r], verdict, views, r);
	}
	if (s->sink != NULL && rb->n > 0)
		s->sink(s->sink_arg, &(SiftF2Batch) {.views = views,.tid = rb->tid,.live = live,.maybe = maybe,.scores = s->bscores,.n = rb->n});
	MemoryContextDelete(slab_cx);
}

static bool
part_skipped(const F2Scan *s, uint64 gen)
{
	return s->nskip_gens > 0 && bsearch(&gen, s->skip_gens, s->nskip_gens, sizeof(uint64), tid_cmp) != NULL;
}

/* The first partition that reaches the lower bound: the partitions tile the key space in order, so those whose range ends before it are one prefix */
int
sift_f2_first_reaching(const SiftPlanSnapshot *plan, const uint8 *lo)
{
	int			a = 0,
				b = plan->npartitions;

	while (a < b)
	{
		int			mid = a + ((b - a) >> 1);

		if (sift_manifest_overlaps(plan->partitions[mid].key_lo, plan->partitions[mid].key_hi, lo, NULL))
			b = mid;
		else
			a = mid + 1;
	}
	return a;
}

int
sift_f2_last_overlapping(const SiftPlanSnapshot *plan, int from, const uint8 *hi)
{
	int			a = from,
				b = plan->npartitions;

	while (a < b)
	{
		int			mid = a + ((b - a) >> 1);

		if (sift_manifest_overlaps(plan->partitions[mid].key_lo, plan->partitions[mid].key_hi, NULL, hi))
			a = mid + 1;
		else
			b = mid;
	}
	return a - 1;
}

void
walk_units(Relation rel, F2Scan *s, const SiftPlanSnapshot *plan, Snapshot snap, const uint64 *staged, int nstaged, bool want_px, F2UnitFn fn, void *arg)
{
	const uint8 *lo = s->keys.hull.has_lo ? s->keys.hull.lo : NULL,
			   *hi = s->keys.hull.has_hi ? s->keys.hull.hi : NULL;
	const SiftPendDelIndex *staged_px = want_px ? sift_f2_staged_index(staged, nstaged) : NULL;

	sift_f2_maint_note_parts(rel, plan->partitions, plan->npartitions, NULL, NULL);
	for (int i = sift_f2_first_reaching(plan, lo); i < plan->npartitions; i++)
	{
		const SiftPartitionEntry *e = &plan->partitions[i];
		const SiftPlanPartList *list = &plan->partlists[i];
		SiftPendDelIndex *px = NULL;
		SiftF2TimeWin win = sift_f2_time_window_clean(rel, e, &s->time);
		bool		in_window = sift_f2_entry_meets_time(e, &win);

		if (!sift_manifest_overlaps(e->key_lo, e->key_hi, NULL, hi))
			break;
		if (!in_window && e->l0_slab_blk == InvalidBlockNumber)
		{
			F2_STAT(partitions_time_skipped);
			F2_STAT_ADD(parts_time_skipped, list->nparts);
			continue;
		}
		if (want_px)
			px = sift_f2_pending_index(rel, e, snap, staged_px);
		if (e->l0_slab_blk != InvalidBlockNumber && !s->survey)
			fn(s, rel, &(F2Unit) {e, NULL, px}, arg);
		for (int p = 0; p < list->nparts; p++)
		{
			if (!sift_f2_part_overlaps(&list->parts[p], &s->keys) || part_skipped(s, list->parts[p].part_gen) || sift_f2_scan_bridge_part_skip(s, e, &list->parts[p]))
				continue;
			if (!in_window || !sift_f2_part_meets_time(&list->parts[p], &win))
				F2_STAT(parts_time_skipped);
			else
				fn(s, rel, &(F2Unit) {e, &list->parts[p], px}, arg);
		}
		if (px != NULL)
			sift_pdel_idx_release(px, staged_px);
	}
}

typedef struct Collect
{
	const SiftF2Rel *f;
	const SiftColSchema *schema;
	Snapshot	snap;
} Collect;

static void
collect_unit(F2Scan *s, Relation rel, const F2Unit *u, void *arg)
{
	const Collect *c = arg;

	if (u->d == NULL)
		scan_slab(rel, s, c->f, c->schema, u->e, u->px, c->snap);
	else if (!part_offered(s, u->e, u->d, u->px))
		scan_part(rel, s, c->f, c->schema, u->e, u->d, u->px, c->snap);
}

/* What the scan reads of the rows the stage of this transaction holds: the check of one key of a unique index finds them in the registry, any other scan reads them from the partitions, to which they are flushed */
static void
stage_for_scan(IndexScanDesc scan, F2Scan *s, const SiftColSchema *schema)
{
	SiftUniqLookup lookup;

	if (sift_f2_unique_lookup(scan, s->cx, &lookup))
	{
		s->pinned_plan = lookup.plan;
		for (int i = 0; i < lookup.hits.n; i++)
			emit(s, schema, sift_f2_tid(&lookup.hits.hit[i].tid), V_MAYBE, NULL, 0);
	}
	else if (sift_stage_has_pending(scan->indexRelation))
		sift_stage_flush(scan->indexRelation);
}

/* pg_stat_*_indexes.idx_scan and the Index Searches of EXPLAIN: a scan counts once, when it starts reading */
void
sift_f2_scan_count(IndexScanDesc scan)
{
	pgstat_count_index_scan(scan->indexRelation);
#if PG_VERSION_NUM >= 180000
	if (scan->instrument)
		scan->instrument->nsearches++;
#endif
}

void
sift_f2_scan_collect(IndexScanDesc scan, F2Scan *s)
{
	Relation	rel = scan->indexRelation;
	Collect		c = {.f = sift_f2_rel(rel),.schema = sift_col_rel(rel)->schema,.snap = scan->xs_snapshot != NULL ? scan->xs_snapshot : GetActiveSnapshot()};
	int			nstaged;
	uint64	   *staged;

	sift_f2_scan_count(scan);
	s->collected = true;
	if (!s->possible)
		return;
	if (s->pinned_plan == NULL && !RecoveryInProgress())
		stage_for_scan(scan, s, c.schema);
	staged = sift_stage_dead(rel, c.snap, &nstaged);
	if (!s->compiled)
		compile_preds(s, c.schema);
	walk_units(rel, s, s->pinned_plan != NULL ? s->pinned_plan : sift_plan_cache_get(rel, s->cx), c.snap, staged, nstaged, true, collect_unit, &c);
	if (s->tbm != NULL)
		sink_flush(s->tbm);
	else if (s->stream != NULL)
		sift_f2_stream_finish(s);
	else if (s->sort != NULL)
		tuplesort_performsort(s->sort);
}

IndexScanDesc
sift_f2_ambeginscan(Relation index, int nkeys, int norderbys)
{
	IndexScanDesc scan = RelationGetIndexScan(index, nkeys, norderbys);
	F2Scan	   *s = palloc0(sizeof(F2Scan));
	bool		ordered,
				ordpair;

	sift_quarantine_net(index, sift_meta_peek_format(index).flags);
	s->hdr.ops = &sift_col_v2_ops;
	s->cx = sift_mem_allocset_create(CurrentMemoryContext, "sift f2 scan");
	scan->opaque = s;
	scan->xs_hitupdesc = s->itupdesc = RelationGetDescr(index);
	s->project = sift_col_ioss_take(index, &ordered, &ordpair, &s->want);
	return scan;
}

void
sift_f2_amrescan(IndexScanDesc scan, ScanKey scankey, int nscankeys, ScanKey orderbys, int norderbys)
{
	F2Scan	   *s = scan->opaque;

	if (nscankeys > 0)
		memcpy(scan->keyData, scankey, nscankeys * sizeof(ScanKeyData));
	sift_f2_scan_sort_end(s);
	MemoryContextReset(s->cx);
	*s = (F2Scan) {.hdr = s->hdr,.cx = s->cx,.itupdesc = s->itupdesc,.want = s->want,.project = s->project,
				   .rescans = s->rescans + 1,.rescan = s->rescan};
}
