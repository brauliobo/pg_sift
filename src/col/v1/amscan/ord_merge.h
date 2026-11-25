/*
 * col/v1/amscan/ord_merge.h - the merge of the global order of a leaf (col/v1/gorder/gorder.h) over the sources of a scan: the entries (key, heap address) of the stored segments and the
 * rows of the write buffers, in the order of an ORDER BY on the leaf. The entries are of rows that may have died or changed since: whoever reads them has the
 * heap say which are live and what they hold.
 */
#ifndef SIFT_COL_V1_AMSCAN_ORD_MERGE_H
#define SIFT_COL_V1_AMSCAN_ORD_MERGE_H

#include "postgres.h"

#include "storage/itemptr.h"

#include "col/col.h"
#include "col/v1/gorder/gorder.h"

typedef struct SiftOrdMerge SiftOrdMerge;

/* What sift_col_gorder_exact knows of an entry of a source at a heap address */
#define SIFT_GORD_EXACT		1	/* a row that is live and visible to every snapshot: the heap need not confirm it */
#define SIFT_GORD_CHECK		0	/* the unit holds the row live, the heap says whether this snapshot sees it */
#define SIFT_GORD_DEAD		(-1)	/* the unit has cleared the row: another source holds the row now at the address, if any */
#define SIFT_GORD_UNKNOWN	(-2)

/* A range of the key, ends included (of the strings, for a text key, each end included or not), for the conjuncts of the query on it: the rows with no value are not in it */
typedef struct SiftOrdBound
{
	bool		has_lo;
	bool		has_hi;
	int64		lo;
	int64		hi;
	const char *slo;			/* a text key (collid not 0 at the open): the range is of strings, in the order of the collation, instead of lo and hi; has_lo and has_hi say which ends it has */
	const char *shi;
	uint32		slolen;
	uint32		shilen;
	bool		slo_excl;		/* the strings must be after slo, not equal to it */
	bool		shi_excl;		/* ... before shi */
	int			npin;			/* conditions leaf = value of the query: a global order grouped by one of the leaves reads the entries of that group only, and cannot answer without it */
	int			pinleaf[SIFT_GORD_PIN_MAX];	/* the same leaf more than once: an IN list, the group of any of the values */
	int64		pinval[SIFT_GORD_PIN_MAX];
	uint32		nin;			/* an IN list on the key (an integer one): its values, ascending and distinct, at least two, between lo and hi; the scan reads the entries of each value after those of the one before it */
	const int64 *in;
} SiftOrdBound;


/* The merge for a scan that wants rows rows. NULL when the source has no global order of the leaf, or its write buffers hold too many rows to sort for so few (sift_col_gorder_open) */
extern SiftOrdMerge *sift_ordm_open(SiftColSource *src, int leaf, int rows, bool desc, bool nulls_first, uint32 collid, const SiftOrdBound *bound, MemoryContext cxt);
/* The same, for a scan that opens one merge after another over the same segments (the groups of a pair): the chunks prev decoded are the new merge's to start from, and prev gives them up (it may be released after) */
extern SiftOrdMerge *sift_ordm_open_from(SiftOrdMerge *prev, SiftColSource *src, int leaf, int rows, bool desc, bool nulls_first, uint32 collid, const SiftOrdBound *bound, MemoryContext cxt);
/* The string of the entry the last sift_ordm_next gave, of a text key (collid not 0 at the open): the key is then a rank in its source, not comparable with another's */
/* The next entry: its key (isnull: the row has none) and heap address; false at the end. An entry that repeats the last one (a row in two sources) is skipped. */
extern bool sift_ordm_next(SiftOrdMerge *g, int64 *key, bool *isnull, ItemPointerData *tid);
/* What its source knows of the entry sift_ordm_next just gave (SIFT_GORD_EXACT: a row live and visible to every snapshot, the heap need not confirm it, as the first rows an OFFSET throws away; SIFT_GORD_DEAD: its segment lost the row,
 * nothing in that source stands for it, and a repeat of it in another source (sift_ordm_repeated) is the row that took its address) */
extern int sift_ordm_state(const SiftOrdMerge *g, const ItemPointerData *tid);
/* The sift_ordm_next just made dropped an entry that repeated the one it gave before */
extern bool sift_ordm_repeated(const SiftOrdMerge *g);
/* An entry sift_ordm_skip took off the front that the heap has to confirm: the address, and its key when the block has it exactly (check) */
typedef struct SiftOrdPend
{
	uint64		tid;
	int64		key;
	bool		check;
	int			cur;			/* a text key: the source of the entry, whose string the key is the rank of (sift_ordm_pend_str) */
} SiftOrdPend;
/* Takes off the front of the sources the entries that are certainly among the first k of the scan: those of rows that need no check in the heap are counted (returned), the others are listed (sift_ordm_pending) for the caller to confirm; it throws the same number of rows away. Before the first sift_ordm_next */
/* The string of the pending entry of a text key, valid until the next call */
/* Some source is the ends of a segment's order: the entries it has end where the segment's go on. The scan that reads them may find the order short of rows (sift_ordm_short) */
extern bool sift_ordm_ends(const SiftOrdMerge *g);
/* A source of the ends went through its last entry while the segment has more after it: the entries given so far are the order's, those it would give next are not known */
/* Some source has compact chunks: the exact key of their entries costs a heap row apiece, and a scan that reads every entry does better sorting the rows of the units */
extern bool sift_ordm_compact(const SiftOrdMerge *g);
/* The leaf the arrays are grouped by, whose pinned value the scan reads the entries of; -1 when they are not grouped */
extern int	sift_ordm_group(const SiftOrdMerge *g);

#endif
