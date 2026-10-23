/*
 * table/seg_int.h - shared between the pieces of the column groups of a sift_table (seg.c and the files that hold the rest of it).
 */
#ifndef SIFT_SEG_INT_H
#define SIFT_SEG_INT_H

#include "col/codec.h"
#include "col/pg.h"
#include "table/table.h"

struct SiftTblLayout
{
	SiftColSchema *schema;
	int			natts;
	int		   *leaf;			/* [natts + 1] the leaf of attribute a, -1 for a dropped column */
	int			exc;			/* the leaf of the exceptions */
	SiftColShredAdapter *shred;	/* the declared paths of the jsonb columns (sift.table_set_paths), NULL when there are none */
	int		   *first;			/* [natts + 1] the first leaf the shredder fills for attribute a */
};

/* The schemas of the segments seen so far, by generation (a segment is immutable) */
typedef struct SchemaEnt
{
	uint64		gen;
	SiftColSchema *schema;
} SchemaEnt;

/* The superblocks and the version summary of the units of a group with unfrozen rows, read from its segment once (a segment is immutable) */
typedef struct VerGroup
{
	uint64		gen;
	uint32		nsb;
	uint32	   *sb;
	SiftColVerSum *sum;
} VerGroup;

/* An index scan over rows whose versions sit in different superblocks (updated rows) alternates between them: each needs its decoded unit kept */
#define SIFT_TBL_FETCH_SCANS	4

struct SiftTblGroupCache
{
	int			n;
	SchemaEnt  *ent;
	int			cap;
	VerGroup   *vg;
	int			nvg;
	/* serve the TID fetches while the directory and the backend's xmin stay as they were, one per superblock being fetched from */
	SiftTblGroupScan *fetch[SIFT_TBL_FETCH_SCANS];
	uint64		fetch_used[SIFT_TBL_FETCH_SCANS];
	uint64		fetch_clock;
	uint32		fetch_version;
	TransactionId fetch_xmin;
	uint64		pkey;			/* path_zones: the segments pls was computed over */
	SiftColLeafStats *pls;
	struct PathWbuf *pwb;		/* path_wbuf_ovf: per write buffer */
	int			npwb;
};

typedef struct SealWriter
{
	Relation	rel;
	const SiftTblLayout *l;
	const SiftCcPolicy *policy;
	MemoryContext cxt;
	SiftColBatch *batch;
	SiftWriteTarget *wt;
	SiftColSegWriter *w;
	uint64		expect;			/* bytes the segment will take at most (the write buffer pages): free runs shorter than a 256th are reused */
	int			format;			/* the column format minor of the group */
} SealWriter;

extern SiftCcPolicy *layout_policy(Relation rel, const SiftTblLayout *l);
extern const SiftTblLayout *layout_of(Relation rel, SiftTblRel *st);
extern void row_in(const SiftTblLayout *l, TupleDesc desc, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row);
extern Datum image_datum(Form_pg_attribute att, const char *p, uint32 len);
extern Datum value_out(const SiftColDef *def, Form_pg_attribute att, const SiftColVec *vec, uint32 i);
extern SiftColSchema *schema_of(Relation rel, SiftTblRel *st, const SiftSegEntry *e);
extern bool seg_page(Relation rel, SiftTblSeg *seg, uint32 idx, Page copy);
extern void seal_flush(SealWriter *sw);

/*
 * The column format minor of a group: sift.col_format_minor, but the version section of a group with unfrozen rows is minor 4 whatever it says, and no
 * dictionary is stored for a lookup in place (minor 14) unless sift.table_dict_lookup is on: it costs a quarter to three quarters of a high-distinct text
 * column and a row fetch gets back the load of the dictionary in the first statement of a backend only
 */
static inline int
tbl_col_format(bool vers)
{
	int			minor = sift_tbl_dict_lookup ? sift_col_format_minor : Min(sift_col_format_minor, SIFT_COL_FORMAT_RUNS);

	return vers ? Max(minor, SIFT_COL_FORMAT_VERS) : minor;
}

/* Whether a merge of the n segments of sel can carry rows that are not final: a version section, or a group tagged with its writer by 3.0.1 */
static inline bool
any_vers(const SiftSegEntry *sel, int n)
{
	for (int i = 0; i < n; i++)
		if ((sel[i].flags & SIFT_SEG_VERS) || (sel[i].format < SIFT_COL_FORMAT_VERS && sel[i].reserved != 0))
			return true;
	return false;
}

static inline struct SiftTblGroupCache *
gcache_of(SiftTblRel *st)
{
	if (st->gcache == NULL)
		st->gcache = MemoryContextAllocZero(st->cxt, sizeof(struct SiftTblGroupCache));
	return st->gcache;
}

/* The directory is more than half full: the rows of a running transaction cannot wait in row format any longer */
static inline bool
dir_pressure(const SiftTblRel *st)
{
	return st->dir.nsegs > sift_tbl_max_segments / 2;
}

/* The directory is past sift.table_backpressure percent of its entries: a writer settles, waiting for the maintenance lock, before it opens another write buffer */
static inline bool
dir_backpressure(const SiftTblRel *st)
{
	return sift_tbl_backpressure > 0 && st->dir.nsegs * 100 > sift_tbl_backpressure * sift_tbl_max_segments;
}

#endif							/* SIFT_SEG_INT_H */
