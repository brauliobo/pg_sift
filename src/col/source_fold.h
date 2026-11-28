/*
 * col/source_fold.h - the aggregate scan of a format that gives batches (agg/batch_source.h): the columns the statement reads are asked of the source, and each batch it gives is folded into the
 * groups, the star pipe or the emit of a parallel participant, as the zone scan of Format 2 does with the chunks it decodes. Format 1 (col/v1/store_ops_zone.c, v1_store_ops_par.c) is its caller; the file
 * includes the operators of agg/, which a format file may not (scripts/check-format-isolation.sh), so the formats reach them through this one.
 */
#ifndef SIFT_COL_SOURCE_FOLD_H
#define SIFT_COL_SOURCE_FOLD_H

#include "postgres.h"

#include "nodes/execnodes.h"
#include "nodes/pg_list.h"
#include "utils/rel.h"

#include "agg/batch_source.h"
#include "agg/vec.h"
#include "core/store/ops.h"

typedef struct SiftSrcFold SiftSrcFold;

/* What a fold needs of the scan: spec, and where its rows go: group, else emit (a participant of a parallel run), else the pipe of star (into rows for a row Top-N; it hands them to emit when there is one) */
typedef struct SiftSrcFoldArgs
{
	const struct SiftV6Spec *spec;
	struct SiftV6Group *group;
	struct SiftV6ScanStats *stats;
	struct SiftV6StarRun *star;
	struct SiftV6Rows *rows;	/* the best rows of a row Top-N the pipe of star joins: the heap addresses of the rows ride its last slot */
	const struct SiftClause *gate;	/* an arm of a pass over the union of the keys of its arms: the rows it takes (NULL: all) */
	void		(*emit) (void *arg, const SiftBatchSel *bs, SiftVec *const *keyvec, SiftVec *const *argvec);
	void	   *emit_arg;
	Relation	host;				/* the index, or the sift_table, the source reads */
	bool		is_table;			/* host is a sift_table (the spec of a partitioned table is of no one of its partitions) */
	List	   *quals;				/* conjuncts over the heap attributes as Vars of varno 1 (clauses of an index scan are mapped by sift_srcfold_scankey_quals) */
	EState	   *estate;				/* the values of the parameters the quals read, NULL for none */
} SiftSrcFoldArgs;

/* The source of the columns of in, over the rows that pass quals (NULL: all); texts: the batch quals of the fold, whose text quals a source may test on a unit's dictionary (NULL: none) */
struct SiftV6Bq;
typedef SiftV6Source *(*SiftSrcOpen) (void *arg, const SiftV6Input *in, const SiftV6Vexpr *quals, const struct SiftV6Bq *texts);

extern SiftSrcFold *sift_srcfold_new(const SiftSrcFoldArgs *a, MemoryContext cx);
/* Opens the source, folds every batch of it and closes it; the fold can run again (a morsel of a parallel scan) */
extern void sift_srcfold_run(SiftSrcFold *f, SiftSrcOpen open, void *open_arg);
/* The last run of this source is over: after the last source of the star join, the rows of a RIGHT or FULL join's dimensions that no row matched are given, once */
extern void sift_srcfold_finish(SiftSrcFold *f);
extern void sift_srcfold_free(SiftSrcFold *f);
/* A text-hit chunk of the scan: the statistics count it and its TIDs */
extern void sift_srcfold_count_chunk(struct SiftV6ScanStats *stats, const struct SiftTidBridge *bridge);
/* The columns the fold reads (the score of a text hit among them, as SIFT_V6_SCORE_ATTNO) and the folding of a batch whose vectors are those, in that order: for a caller that has the rows already
 * (and counts them in the statistics: a run counts the rows of the granules its source read) */
extern const SiftV6Input *sift_srcfold_input(const SiftSrcFold *f);
extern void sift_srcfold_batch(SiftSrcFold *f, const SiftV6Batch *b);
/* The heap addresses of the rows of the batch the fold is folding: what its emit reads to tell the rows it keeps */
extern const uint64 *sift_srcfold_tags(const SiftSrcFold *f);

/* The arms of a cohort (agg/fuse/fuse.h) read the same relation with the same keys: each arm folds its rows into its groups, those its gate takes; args gives the relation, quals and estate */
extern void sift_srcfold_arms(struct SiftV6FuseCohort *cohort, const struct SiftV6ScanArm *arms, const SiftSrcFoldArgs *common, SiftSrcOpen open, void *open_arg);

/* A conjunct of the quals that bounds an integer column of the input (its slot) to a closed range: a source may leave unread a unit that lies wholly outside it */
#define SIFT_SRC_MAX_BOUNDS	12

/* ... or, with nvals, to the values of a list (ascending, within lo and hi): a source with a point directory of the column reads the units that list them */
typedef struct SiftSrcBound
{
	int			slot;
	bool		has_lo;
	bool		has_hi;
	int64		lo;
	int64		hi;
	int			nvals;
	const int64 *vals;
	bool		isnull;			/* a bound of a range of an OR: the column IS NULL */
} SiftSrcBound;

extern int	sift_srcfold_bounds(const SiftV6Vexpr *quals, SiftSrcBound *out);

#define SIFT_SRC_MAX_SETS	4

/* The OR conjuncts of the quals (sift_srcfold_nsets of them, at most SIFT_SRC_MAX_SETS) as their ranges: a row passes one only inside a range of it, each an AND of the bounds sift_srcfold_range gives (into out, at most SIFT_SRC_MAX_BOUNDS) */
extern int	sift_srcfold_nsets(const SiftV6Vexpr *quals);
extern int	sift_srcfold_nranges(const SiftV6Vexpr *quals, int set);
extern int	sift_srcfold_range(const SiftV6Vexpr *quals, int set, int range, SiftSrcBound *out);

/* The clauses an index scan's keys say, as conjuncts over the heap attributes (Vars of varno 1) of the table of index: scan keys on a column of the index that is no attribute of the table are refused */
extern List *sift_srcfold_scankey_quals(Relation index, ScanKey keys, int nkeys);
/* The clauses of an index qual (Vars of INDEX_VAR, and the attributes of a table the index quals of a sift_table name the same way) over the heap attributes of host, as Vars of varno 1 */
extern List *sift_srcfold_heap_quals(Relation host, const List *indexqual);

#endif							/* SIFT_COL_SOURCE_FOLD_H */
