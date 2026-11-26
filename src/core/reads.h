/*
 * core/reads.h - what the backends of a server paid to read what maintenance would spare them (core/reads.c): the rows decoded from a write buffer, the instructions a mark of the tombstone
 * log cost, the scans of an index. The sums are in the metapage (SiftMetaPageData.reads) and what a backend has not written yet.
 */
#ifndef SIFT_READS_H
#define SIFT_READS_H

#include "core/host.h"

#include "core/page.h"

/* The rows the backends of the server decoded from the buffer of generation gen to read it, since it was opened (core/reads.c): what its promotion is weighed against (maint_wbuf_read_due). The sum is in the metapage (SiftMetaPageData.reads) and what this backend has not written yet */
extern void sift_cwbuf_decoded_note(Relation index, uint64 gen, uint64 rows);
extern uint64 sift_cwbuf_decoded(Relation index, const SiftMetaPageData *meta, uint64 gen);
/*
 * The instructions the reads of the server paid for the marks of the tombstone log of generation loggen (a scan that read a unit page by page for a mark in its range of heap
 * blocks, or did without its pre-aggregate: col/v1/scan/unit.c), what the fold of the marks is weighed against (sift_col_fold_due); whether this backend paid some since the last call
 */
extern void sift_col_marks_note(Relation index, uint64 loggen, uint64 instr);
extern uint64 sift_col_marks_paid(Relation index, const SiftMetaPageData *meta, uint64 loggen);
/*
 * A scan of the index begins (the metapage as the scan read it): counted in the sum of the scans that the merge of the overlays is weighed against (core/reads.c says how a scan counts), and what the metapage has
 * for them plus what this backend has not written yet
 */
extern void sift_scans_note(Relation index, const SiftMetaPageData *meta);
extern uint64 sift_scans_paid(Relation index, const SiftMetaPageData *meta);
/* Does this backend hold sums not written to the metapage of the index; write them, except those whose rule is met already (the callers' due functions, NULL for a sum the index has none of): the end of a statement (core/reads.c) */
extern bool sift_reads_pending(Oid index);
extern void sift_reads_flush(Relation index, bool (*wbuf_met) (Relation), bool (*marks_met) (Relation));
/* The same, for a read that repeats on an open buffer: it writes when the sums it holds for the index are 8192 rows, else for one call in 64 */
extern void sift_reads_flush_sampled(Relation index, bool (*wbuf_met) (Relation), bool (*marks_met) (Relation));
extern bool sift_col_marks_take(void);

#endif							/* SIFT_READS_H */
