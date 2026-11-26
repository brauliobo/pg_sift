/*
 * core/reads.c - what the backends of a server paid to read what maintenance would spare them: the rows decoded from a write buffer, the instructions a mark of the tombstone log cost.
 *
 * A backend builds the batches of a buffer in its first statement that reads it (col/v1/wbuf/batches.c, 5,900 instructions a row) and keeps them, so that the cost is
 * paid again by each backend that comes after, and by none while the buffer is promoted away. Maintenance weighs the sum of what the backends paid against the promotion
 * (maint_wbuf_read_due): the sum is in the metapage of the index (SiftMetaPageData.reads), for the generation of the buffer, which no other buffer of the index ever has. A write-only buffer has no sum.
 * The marks of a log are weighed the same way (sift_col_fold_due), for the generation of the log, which is a new one when the log is rewritten (the fold of the marks, a VACUUM): what the reads paid to the log it replaced is not
 * owed to the new one, and a note for a generation newer than the one the sum is for starts it again, an older one is dropped. An index has its own sum however many there are, and a restart or a crash keeps what the log
 * has (the table of the shared memory it replaces was empty after either, and had 64 slots that every index of the server shared).
 *
 * A read adds to a small list of its backend, and the end of its statement writes the list in one record of the metapage alone (sift_meta_update: the directory is not touched, so no scan that holds a copy of it is told of a change).
 * A sum that already meets the rule (the caller's `met`, computed without the list) is not written again: more would be written for nothing, and a rule that cannot run (no worker) would otherwise make every read write.
 * A standby cannot write: its reads count nowhere. What the reads of a scope paid for its changed pairs is kept the same way (col/v1/scope/paid.c).
 *
 * The scans of a key-cut index are counted the same way (SIFT_READ_SCANS), for the weighing of the merge of its overlays (maint/rewrite.c): a count of every scan would write the metapage, which every scan reads,
 * once for each of them, so a scan is counted as many as the count says (SCANS_EXACT scans each, then a scan is sampled with the probability 1/k and counts k, k being 1 and one more for every SCANS_EXACT counted):
 * the count is the number of scans on average and exact while it is small, a session that reads once adds to it as much as one that reads a million times, and the index is written about
 * SCANS_EXACT times for each e-fold of its scans (a million scans: 700 records), where pg_stat's numscans, which this count replaces, is lost by pg_stat_reset() and by a crash.
 */
#include "postgres.h"

#include "access/xlog.h"
#include "common/pg_prng.h"
#include "utils/memutils.h"

#include "core/reads.h"
#include "core/restrict.h"
#include "core/store/store.h"

#define SCANS_EXACT		64
#define FLUSH_PAID_ROWS	8192
#define FLUSH_SAMPLE	64

typedef struct Pending
{
	Oid			index;
	int			kind;
	uint64		gen;
	uint64		paid;
} Pending;

static Pending *pending;
static int	npending,
			cap;
static bool marked_here;		/* ... or paid for marks, since the last sift_col_marks_take */

static void
reads_add(Relation index, int kind, uint64 gen, uint64 paid)
{
	Oid			oid = RelationGetRelid(index);

	if (paid == 0)
		return;
	for (int i = 0; i < npending; i++)
		if (pending[i].index == oid && pending[i].kind == kind && pending[i].gen == gen)
		{
			pending[i].paid += paid;
			return;
		}
	if (npending == cap)
	{
		cap = Max(cap * 2, 8);
		pending = pending ? repalloc(pending, cap * sizeof(Pending)) : MemoryContextAlloc(TopMemoryContext, cap * sizeof(Pending));
	}
	pending[npending++] = (Pending) {.index = oid,.kind = kind,.gen = gen,.paid = paid};
}

void
sift_scans_note(Relation index, const SiftMetaPageData *meta)
{
	uint64		k = 1 + meta->reads[SIFT_READ_SCANS].paid / SCANS_EXACT;

	if (k == 1 || pg_prng_uint64_range(&pg_global_prng_state, 0, k - 1) == 0)
		reads_add(index, SIFT_READ_SCANS, 1, k);
}

void
sift_cwbuf_decoded_note(Relation index, uint64 gen, uint64 rows)
{
	reads_add(index, SIFT_READ_WBUF, gen, rows);
}

void
sift_col_marks_note(Relation index, uint64 loggen, uint64 instr)
{
	marked_here |= instr > 0;
	reads_add(index, SIFT_READ_MARKS, loggen, instr);
}

bool
sift_col_marks_take(void)
{
	bool		was = marked_here;

	marked_here = false;
	return was;
}

/* What the metapage has for the generation and what this backend has not written yet */
static uint64
reads_paid(Relation index, const SiftMetaPageData *meta, int kind, uint64 gen)
{
	uint64		paid = meta->reads[kind].gen == gen ? meta->reads[kind].paid : 0;

	for (int i = 0; i < npending; i++)
		if (pending[i].index == RelationGetRelid(index) && pending[i].kind == kind && pending[i].gen == gen)
			paid += pending[i].paid;
	return paid;
}

uint64
sift_cwbuf_decoded(Relation index, const SiftMetaPageData *meta, uint64 gen)
{
	return reads_paid(index, meta, SIFT_READ_WBUF, gen);
}

uint64
sift_col_marks_paid(Relation index, const SiftMetaPageData *meta, uint64 loggen)
{
	return reads_paid(index, meta, SIFT_READ_MARKS, loggen);
}

uint64
sift_scans_paid(Relation index, const SiftMetaPageData *meta)
{
	return reads_paid(index, meta, SIFT_READ_SCANS, 1);
}

bool
sift_reads_pending(Oid index)
{
	for (int i = 0; i < npending; i++)
		if (pending[i].index == index)
			return true;
	return false;
}

typedef struct Flush
{
	const Pending *items;
	int			n;
} Flush;

static bool
add_pending(SiftMetaPageData *meta, void *arg)
{
	const Flush *f = arg;
	bool		changed = false;

	for (int i = 0; i < f->n; i++)
	{
		const Pending *p = &f->items[i];
		SiftMetaReads *r = &meta->reads[p->kind];

		if (p->gen < r->gen)
			continue;
		if (p->gen > r->gen)
			*r = (SiftMetaReads) {.gen = p->gen};
		r->paid += p->paid;
		changed = true;
	}
	return changed;
}

static uint64
pending_paid(Oid index)
{
	uint64		paid = 0;

	for (int i = 0; i < npending; i++)
		if (pending[i].index == index)
			paid += pending[i].paid;
	return paid;
}

/* A backend that reads a buffer again and again writes the metapage when its sums are worth a record (FLUSH_PAID_ROWS) or for one call in FLUSH_SAMPLE: the sums it holds count in reads_paid meanwhile */
void
sift_reads_flush_sampled(Relation index, bool (*wbuf_met) (Relation), bool (*marks_met) (Relation))
{
	if (pending_paid(RelationGetRelid(index)) < FLUSH_PAID_ROWS && pg_prng_uint64_range(&pg_global_prng_state, 0, FLUSH_SAMPLE - 1) != 0)
		return;
	sift_reads_flush(index, wbuf_met, marks_met);
}

/*
 * The sums this backend has for the index go to its metapage, except those the rule is met for already (wbuf_met, marks_met: computed from the metapage alone, the list is taken out first). In a standby, a parallel operation or a
 * VACUUM they are dropped: it cannot write (sift_catalog_write_ok, as the counter of the scopes).
 */
void
sift_reads_flush(Relation index, bool (*wbuf_met) (Relation), bool (*marks_met) (Relation))
{
	Oid			oid = RelationGetRelid(index);
	Pending		mine[2 * SIFT_META_READS];	/* a buffer or a log of the generation before and one of the generation after is the most a statement notes */
	Flush		f = {.items = mine};
	bool		met[SIFT_META_READS] = {false};
	int			kept = 0,
				n = 0;

	for (int i = 0; i < npending; i++)
		if (pending[i].index != oid || n == lengthof(mine))
			pending[kept++] = pending[i];
		else
			mine[n++] = pending[i];
	npending = kept;
	if (n == 0 || RecoveryInProgress() || !sift_catalog_write_ok())
		return;
	for (int i = 0; i < n; i++)
	{
		bool		(*fn) (Relation) = mine[i].kind == SIFT_READ_WBUF ? wbuf_met : mine[i].kind == SIFT_READ_MARKS ? marks_met : NULL;

		if (fn != NULL && !met[mine[i].kind] && fn(index))
			met[mine[i].kind] = true;
	}
	for (int i = 0; i < n; i++)
		if (!met[mine[i].kind])
			mine[f.n++] = mine[i];
	if (f.n > 0)
		sift_meta_update(index, add_pending, &f);
}
