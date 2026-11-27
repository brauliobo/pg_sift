/*
 * col/v1/store_ops_par.c - SiftStoreParOps of Format 1: the superblocks of a sift_col index, or of a sift_table, are the granules of a parallel scan. The leader lists them once as one unit (plan) and gives the directory
 * view its own source read in the words after the units (SiftF2ParPlan.staged_dead, which the engine carries to every participant without looking at it), so that every participant numbers the
 * superblocks as the leader does however the index is maintained meanwhile; a participant reads the runs of superblocks the morsels give it through the source of col/v1/store_ops.c, which
 * vets each row against the tombstones and its snapshot and reads the write buffers as units, and hands the batches to the emit of the engine.
 */
#include "postgres.h"

#include "access/genam.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "col/source_fold.h"
#include "col/v1/reader.h"
#include "col/v1/store_ops.h"
#include "core/memtrack.h"
#include "core/store/ops.h"

typedef struct V1ParScan
{
	const struct SiftV6Spec *spec;
	Relation	heap;
	Relation	index;			/* NULL: heap is a sift_table, which quals decide on */
	ScanKey		keys;
	int			nkeys;
	const struct SiftStoreQuals *quals;
	Snapshot	snap;
	struct SiftV6ScanStats *stats;
	void		(*emit) (void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec);
	void	   *emit_arg;
	MemoryContext cx;
	const void *view;
	SiftSrcFold *fold;
	SiftV1Scan	key_scan;		/* the JSON type and key tests of the scan keys, which every morsel applies as the serial scan does */
} V1ParScan;

static struct SiftV6Scan *
par_begin(const struct SiftV6Spec *spec, Relation heap, Relation index, ScanKey keys, int nkeys, const struct SiftStoreQuals *quals, Snapshot snap, struct SiftV6ScanStats *stats,
		  void (*emit) (void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec), void *emit_arg)
{
	MemoryContext cx = sift_mem_allocset_create(CurrentMemoryContext, "sift v1 parallel scan");
	V1ParScan  *s = MemoryContextAllocZero(cx, sizeof(V1ParScan));

	SIFT_INVARIANT((index == NULL) == (quals != NULL), "a parallel scan of Format 1 reads a sift_col index by its scan keys or a sift_table by its quals");
	*s = (V1ParScan) {.spec = spec,.heap = heap,.index = index,.keys = keys,.nkeys = nkeys,.quals = quals,.snap = snap,.stats = stats,.emit = emit,.emit_arg = emit_arg,.cx = cx,
	.key_scan = index != NULL ? sift_v1_key_scan(index, keys, nkeys) : (SiftV1Scan) {0}};
	return (struct SiftV6Scan *) s;
}

static void
par_plan(struct SiftV6Scan *scan, SiftF2ParPlan *out)
{
	V1ParScan  *s = (V1ParScan *) scan;
	const SiftV1ReaderOps *rd = sift_v1_reader_of(s->index == NULL);
	void	   *src = rd->open(s->heap, s->index, s->snap, s->cx, NULL);
	const uint32 nsb = rd->nsb(src);
	const Size	bytes = rd->view_size(src);

	*out = (SiftF2ParPlan) {0};
	if (nsb == 0)
	{
		rd->close(src);
		return;
	}
	out->refs = palloc0(sizeof(SiftParPartRef));
	out->refs[0] = (SiftParPartRef) {.g_base = 0,.ngranules = nsb};
	out->nrefs = 1;
	out->nstaged = (uint32) ((bytes + sizeof(uint64) - 1) / sizeof(uint64));
	out->staged_dead = palloc0(Max(out->nstaged, 1) * sizeof(uint64));
	if (bytes > 0)
		rd->view_export(src, out->staged_dead);
	rd->close(src);
}

static void
par_morsel_begin(struct SiftV6Scan *scan, const uint64 *view, uint32 nview)
{
	V1ParScan  *s = (V1ParScan *) scan;
	const bool	is_table = s->index == NULL;
	SiftSrcFoldArgs a = {.spec = s->spec,.stats = s->stats,.emit = s->emit,.emit_arg = s->emit_arg,.host = is_table ? s->heap : s->index,.is_table = is_table,
		.quals = is_table ? s->quals->clauses : sift_srcfold_scankey_quals(s->index, s->keys, s->nkeys),.estate = is_table ? s->quals->estate : NULL};

	s->view = nview > 0 ? view : NULL;
	s->fold = sift_srcfold_new(&a, sift_mem_allocset_create(s->cx, "sift v1 parallel fold"));
}

typedef struct V1Morsel
{
	const V1ParScan *s;
	SiftV1Range range;
} V1Morsel;

static SiftV6Source *
open_morsel(void *arg, const SiftV6Input *in, const SiftV6Vexpr *quals, const struct SiftV6Bq *texts)
{
	const V1Morsel *m = arg;
	SiftV1Scan	scan = m->s->key_scan;

	scan.range = &m->range;
	scan.texts = texts;
	scan = sift_v1_spec_scan(&scan, m->s->spec);

	return sift_v1_source_open(m->s->cx, m->s->snap, in, quals, &scan);
}

static void
par_morsel(struct SiftV6Scan *scan, struct SiftV6Group *into, const SiftParPartRef *ref, uint32 first, uint32 n)
{
	V1ParScan  *s = (V1ParScan *) scan;
	V1Morsel	m = {.s = s,.range = {.view = s->view,.sb_lo = ref->g_base + first,.sb_hi = ref->g_base + first + n}};

	sift_srcfold_run(s->fold, open_morsel, &m);
}

static void
par_end(struct SiftV6Scan *scan)
{
	MemoryContextDelete(((V1ParScan *) scan)->cx);
}

const SiftStoreParOps sift_v1_par_ops = {
	.begin = par_begin,
	.plan = par_plan,
	.morsel_begin = par_morsel_begin,
	.morsel = par_morsel,
	.end = par_end
};
