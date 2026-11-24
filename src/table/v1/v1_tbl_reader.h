/*
 * table/v1/v1_tbl_reader.h - the state of the table reader (v1_tbl_reader*.c), which the aggregate engine's source of a sift_table starts with
 */
#ifndef SIFT_TABLE_V1_V1_TBL_READER_H
#define SIFT_TABLE_V1_V1_TBL_READER_H

#include "col/col.h"
#include "col/v1/src_sel.h"
#include "col/v1/v1_reader.h"
#include "table/v1/table_v1.h"

/* The source of one column layout */
typedef struct TblPart
{
	SiftColSource *src;
	const SiftColSchema *schema;
	int		   *col;			/* [nleaves] the leaf of this schema that holds layout leaf i, -1 when it has none (a column added later) */
	int		   *leaf_of_col;	/* [schema->ncols] the layout leaf a leaf of this schema holds, -1 when the table dropped it */
	SiftColUnit unit;			/* the head of this source's units, valid when have */
	bool		have;
} TblPart;

typedef struct TblWbatch
{
	uint32		sb;
	MemoryContext cxt;
} TblWbatch;

typedef struct SiftV1Tbl
{
	Relation	rel;
	SiftTblRel *st;
	Snapshot	snapshot;
	MemoryContext cxt;
	SiftDirSnapshot *dir;
	SiftTblVers *vers;			/* the groups of dir with unfrozen rows */
	SiftTblSeg *wbuf;			/* private copies of the write buffers */
	int			nwbuf;
	SiftTblScanState ss;		/* what the page filter reads of the table: the copies above */
	uint32		wbuf_lo;		/* the superblocks that hold write buffer rows: [wbuf_lo, wbuf_hi] */
	uint32		wbuf_hi;
	struct TblWbatch *wb;		/* the superblocks of the last begin whose write buffer rows are in the source: each batch has a context of its own */
	int			nwb;
	int			wbcap;
	uint32		range_hi;		/* the end of the range of the last begin, and of the window being read (win_hi): a begin reads the write buffer rows one superblock at a time */
	uint32		win_hi;
	SiftSrcLayout *lay;
	TblPart    *part;
	int			nparts;
	int			last;			/* the part of the unit returned last, -1 at the start */
	SiftSrcDict *dict;			/* per leaf: the dictionary of the last read */
	SiftSrcDict *adict;			/* per leaf: the dictionary of its default in the segments that lack the column */
	SiftSrcLeafBuf *buf;		/* per leaf */
	SiftSrcSel	sel;			/* the match set the units are restricted to */
	bool	   *raw;			/* per leaf: an image leaf read as the codes of its stored dictionary */
	bool	   *tol;			/* per leaf: its values the table cannot hold are read as NULL */
	MemoryContext *wcxt;		/* per wide leaf: the dictionary of its last read */
	uint64		wseq;			/* the id of the next wide dictionary */
	/* begins the source of part k of the window [sb_lo, sb_hi) (NULL: with no test, sift_col_begin_zones) */
	void		(*begin_part) (struct SiftV1Tbl *t, int k, uint32 sb_lo, uint32 sb_hi);
} SiftV1Tbl;

/* t is zeroed storage the caller owns; the reader's allocations live in mcxt. view: what sift_v1_tbl_view_export wrote for another reader of the statement, NULL: read the table now */
extern void sift_v1_tbl_init(SiftV1Tbl *t, Relation heap, Snapshot snapshot, MemoryContext mcxt, const void *view);
extern Size sift_v1_tbl_view_size(void *impl);
extern void sift_v1_tbl_view_export(void *impl, void *buf);

extern SiftSrcLayout *sift_v1_tbl_layout(Relation rel);
extern void sift_v1_tbl_stats(Relation rel, SiftSrcStats *st);
/* A leaf of a declared path of a jsonb column, not one of the table's own columns */
extern bool sift_v1_tbl_leaf_is_path(const SiftColDef *def);

extern void sift_v1_tbl_close(void *impl);
extern const SiftSrcLayout *sift_v1_tbl_source_layout(const void *impl);
extern uint32 sift_v1_tbl_nsb(void *impl);
extern void sift_v1_tbl_select(void *impl, const SiftFilter *keep);
extern void sift_v1_tbl_begin(void *impl, uint32 sb_lo, uint32 sb_hi);
extern bool sift_v1_tbl_next(void *impl, SiftSrcUnit *unit);
extern void sift_v1_tbl_live(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern bool sift_v1_tbl_vet_needed(void *impl, const SiftSrcUnit *unit);
extern void sift_v1_tbl_vet(void *impl, const SiftSrcUnit *unit, uint64 *bits);
extern void sift_v1_tbl_tids(void *impl, const SiftSrcUnit *unit, const uint32 *rows, int n, ItemPointerData *tids);
extern void sift_v1_tbl_read(void *impl, const SiftSrcUnit *unit, int leaf, SiftSrcVec *vec);
extern void sift_v1_tbl_read_rows(void *impl, const SiftSrcUnit *unit, int leaf, const uint32 *rows, int n, SiftSrcVec *vec);
extern void sift_v1_tbl_read_sel(void *impl, const SiftSrcUnit *unit, int leaf, const uint64 *bits, SiftSrcVec *vec);
extern void sift_v1_tbl_wide_read(SiftV1Tbl *t, int leaf, const TblPart *p, const SiftColVec *v, size_t n, Datum dflt, bool dnull, SiftSrcVec *vec);

#endif							/* SIFT_TABLE_V1_V1_TBL_READER_H */
