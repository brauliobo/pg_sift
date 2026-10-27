/*
 * col/scan_int.h - shared between the files of the column source (col/scan.c, col/scan_unit.c, col/scan_filter.c, col/scan_read.c).
 */
#ifndef SIFT_COL_SCAN_INT_H
#define SIFT_COL_SCAN_INT_H

#include "col/am.h"
#include "col/seg.h"
#include "core/visibility.h"

#define corrupt(...) \
	ereport(ERROR, \
			(errcode(ERRCODE_INDEX_CORRUPTED), \
			 errmsg("sift_col unit is corrupted"), \
			 errdetail(__VA_ARGS__)))

#define COLO_TABLE		0x8000		/* internal flag: a source over a sift_table (sift_col_open_table) */
#define MEM_ID_FLAG		(UINT64CONST(1) << 63)
#define INL_ID_FLAG		(UINT64CONST(1) << 62)	/* the dictionary of an inline STR chunk: one per chunk read */

typedef struct ColCursor
{
	SiftColSegment *seg;
	uint64	   *keep;			/* units that passed the zone maps, nsb bits */
	uint64	   *setcut;			/* units a small-set summary cleared, nsb bits */
	uint64	   *ckeep;			/* keep and setcut kept between begins (in the source's sigcx) while the predicates stay the same, and the words of units they hold the answer for */
	uint64	   *csetcut;
	uint64	   *cdone;
	uint32		cgen;			/* the generation of the predicates they were made for (SiftColSource.sig_gen); 0: none */
	const SiftColSegment *cseg;	/* and the segment: a table's source is kept between statements and its segments change */
	uint64		cseg_gen;
	uint32		cur;
	uint32		end;
	bool		full;			/* the scan covers every unit of the segment, and the cursor has handed none over yet: its key table may stand for them (SiftColSource.whole) */
	bool		count_ok;		/* the whole segment is exact and its dictionary counts the rows that pass SiftColSource.pcnt (col_count_segment): the first unit handed over is the segment, with count_rows of them */
	uint64		count_rows;
	uint32		count_unit_rows;	/* the rows of the segment's units */
	uint32		nosuper_to;		/* the units below this one are read one by one: a consumer split the superunit there */
	uint32	   *prow;			/* the rows of the segment the point directory lists for predicate ppred (ascending); the units without one are cut */
	uint32		nprow;
	int			ppred;
	bool		ptids;			/* a point directory of heap addresses answered a predicate: the addresses went to the source (SiftColSource.ptid) */
	uint32		rhint;			/* the rows of the narrowest range of the scan that its leaf's global order counts in this segment (UINT32_MAX: none) */
	uint32		prun;			/* ... from this position of SiftColSource.ptid, the run of this segment (a range of the global order narrows it) */
	bool		others;			/* the scan has predicates besides the directory's: a unit with many of its addresses is read and filtered, not handed to the heap */
	SiftKey    *ptidv;			/* ... and, for a source that narrows its units (SIFT_COLO_POINT_ROWS), the addresses of the segment, ascending */
	uint32		nptidv;
	uint64		pc_gen;			/* of the segment */
	int			pc_col;			/* the last list of addresses the directory gave for a leaf and these keys (pc_nkeys of pc_keys), kept for the next begin of the source */
	int			pc_nkeys;
	int64	   *pc_keys;
	SiftKey    *pc_tids;
	uint32		pc_n;
	uint64	   *pc_keep;		/* the units the list holds addresses of (the others were skipped as no begin read them) */
	struct SiftPointStream *ps;	/* SIFT_COLO_POINT_LAZY: the addresses of the lookup not handed over yet; the units read after them are pend, the units it covers pkept */
	uint64	   *pkept;
	uint64	   *ppend;
} ColCursor;

/* the ids of the in-memory units of sift_col_unit_batch start here, above any write buffer batch */
#define MEM_BATCH_ID	0x40000000

typedef struct ColMem
{
	SiftColBatch *b;
	uint32		id;
	bool		lane;			/* read from the write buffer when the source began */
} ColMem;

/* The element structure of one group leaf in the current unit */
typedef struct ColGroup
{
	bool		ready;
	uint32		nelems;
	uint32	   *counts;			/* nrows */
	uint32	   *eoff;			/* nrows + 1 */
	uint32	   *parent;			/* nelems, on demand */
	const uint64 *nulls;		/* rows without an array */
	const uint64 *ovf;			/* rows whose whole array overflowed */
	uint32		selgen;			/* the ColSelCache generation selpos / seloff belong to (0: none) */
	uint32		selne;
	uint32	   *selpos;			/* the elements of the cached selection's rows */
	uint32	   *seloff;			/* per selected row, its first element in selpos (+ the end) */
} ColGroup;

/*
 * The positions of the last selection a unit was read through: the leaves read through one selection (the usual scan step) share its
 * row positions and, per group, the element positions and offsets of those rows. Keyed by the selection's bits, so a different
 * selection (even at the same address) replaces it; owned by the unit and gone with it.
 */
typedef struct ColSelCache
{
	uint32		gen;			/* bumped for each new selection */
	uint32		n;
	uint32		count;
	uint64	   *bits;			/* copy of the selection */
	uint32	   *idx;			/* its row positions */
} ColSelCache;

/* An inline STR chunk of the unit as the codes of a dictionary of its own (the form every reader and test works on), built on first use */
typedef struct ColInline
{
	SiftCDict  *dict;
	SiftCChunk	chunk;
	uint64		id;
} ColInline;

typedef struct ColUnit
{
	SiftColSource *src;
	SiftColSegment *seg;		/* NULL for an in-memory unit */
	uint32		u;
	uint32		nsu;			/* a superunit: the units [u, u + nsu) of seg; v.nrows is their rows and nothing else of the unit is loaded */
	bool		whole;			/* ... of the whole segment, which has only the key table of src->whole - 1 (col/ktab.h) */
	bool		counted;		/* ... of the whole segment, which has only the rows that pass the test of sift_col_count_range: crows of them */
	uint64		crows;
	uint32		sb;
	uint32		memid;
	uint8	   *loaded;			/* [ncols] the chunk's bytes are in the unit buffer (a stored unit reads the chunks it needs) */
	ColInline  *istr;			/* [ncols] stored units */
	SiftColBatch *batch;
	SiftCUnitView v;
	SiftCZone  *mzones;			/* in-memory units: the encoder's zones */
	bool		codes_only;		/* sift_col_read_text: a STR leaf is read as codes, without its dictionary */
	bool		dict_lazy;		/* sift_col_unit_lazy_dict: a STR leaf may be read with a dictionary that stays in the segment (SiftColDict.get) */
	const uint32 *prow;			/* the rows of this unit the point directory lists (segment rows), when the cursor has one: ppred is its predicate */
	uint32		nprow;
	int			ppred;
	const SiftKey *ptid;		/* the addresses of ptidv that lie in this unit's superblock, when the cursor has some (nptid may be 0 only for a unit the directory lists nothing in) */
	uint32		nptid;
	bool		lane;			/* a batch of the write buffer: no all-visible page (unit_allvisible) */
	bool		clean;			/* no dead row: valid once clean_ready (unit_clean) */
	bool		clean_ready;
	struct ColDelta *delta;		/* a dirty unit net of its dead rows (col/delta.c): NULL until asked; delta_on while a consumer reads the unit that way */
	bool		delta_on;
	bool		delta_bounds;	/* delta_on, but only the unit's zone may be read */
	uint64	   *live;			/* the liveness bits of the unit, when col/delta.c has read them (SIFT_COL_NWORDS(nrows) words): the selection of its rows copies them */
	SiftKey    *keys;
	uint8		sparse;			/* 0 not looked at, 1 no page has a sparse offset bitmap, 2 some has */
	bool		av_ready;
	uint64		av[SIFT_PAGEMASK_WORDS];
	uint64	   *avk;			/* a key-cut unit: the all-visible bit of its p-th page (unit_allvisible_keyed) */
	ColGroup   *grp;
	ColSelCache selc;
	struct
	{
		int			col;		/* the leaf whose run was looked for last (valid with known) */
		bool		known;
		bool		found;
		SiftCRun	r;
		uint64		at;
	}			run;
} ColUnit;

/* The test of a predicate on a dictionary leaf of a stored segment, made once for every unit that shares the dictionary (seg_test) */
typedef struct ColTest
{
	const SiftColSegment *seg;
	SiftColPred pred;			/* a copy, constants included */
	SiftColValue *vals;
	SiftCTest	t;
} ColTest;

#define COL_TEST_CACHE	16

struct SiftColSource
{
	Relation	heap;
	Relation	index;
	MemoryContext mcxt;
	MemoryContext arena;		/* reset by sift_col_next */
	MemoryContext scan;			/* reset by sift_col_begin */
	Snapshot	snap;
	uint32		flags;
	const SiftColSchema *schema;
	uint32		dir_checked;	/* the directory version segments_current last compared with, and its answer */
	bool		dir_current;
	SiftVis		vis;
	SiftColVisibility visibility;
	bool		super;			/* sift_col_super */
	int			whole;			/* sift_col_super_whole: 1 + the key leaf whose table may answer for a whole segment, 0: none */
	int			pcnt;			/* sift_col_count_range: 1 + the text leaf, 0: none */
	SiftColOp	pcnt_op;
	SiftColValue pcnt_val;
	int			nwmeas;			/* ... and the measures it must keep */
	int			wmeas[SIFT_PAGG_MAX_MEAS];
	int			nseg;
	ColCursor  *seg;
	ColMem	   *mem;
	int			nmem;
	int			capmem;
	bool		mem_loaded;
	ColMem	  **order;
	int			norder;
	int			ocur;
	uint32		heap_sbs;
	ColUnit		cu;
	uint64		inl_seq;		/* ids of the dictionaries of inline chunks */
	uint32		batch_seq;		/* in-memory units made by sift_col_unit_batch, for their dictionary ids */
	MemoryContext sigcx;		/* the predicates of the last begin and the bitmaps the cursors made under them */
	SiftColPred *sig;
	int			nsig;
	const SiftColPrune *sigzone;	/* the zone trees of the last begin: made once for the entries of a scan, so a pointer tells them */
	uint32		sig_gen;
	uint32		counted;		/* the sig_gen whose pruned units sift_col_kept_map counted for the whole scan: a cursor counts none of them (0: none) */
	int			ntests;			/* made so far; the slot of the next is ntests % COL_TEST_CACHE */
	SiftColCounters cnt;
	SiftKey    *ptid;			/* the heap addresses the point directories answered since the last begin, in sift_col_begin's memory (SIFT_COLO_POINT_TIDS) */
	bool		ptid_unsorted;	/* a segment's run of addresses started below the last address of the segments before it: sorted at the end of the begin */
	uint32		nptid;
	uint32		capptid;
	int			pstream;		/* the first cursor that may still have addresses to hand over (SIFT_COLO_POINT_LAZY) */
	int			gpin_leaf;		/* the leaf the global order blocks are grouped by and the group the scan pinned (sift_col_gorder_open), -1: none */
	int64		gpin_val;
	struct ColGOrdMem *gmem;	/* the rows of the write buffers sorted for the global order (sift_col_gorder_open), in mcxt */
	/* col_source zeroes the fields before these two only (a partitioned table's scan opens thousands of sources): a reader looks at the first ntests tests and the first dir.nsegs segments */
	ColTest		tests[COL_TEST_CACHE];	/* in mcxt: a begin does not drop them, a segment stays open as long as the source */
	SiftDirSnapshot dir;
};

/* scan_unit.c: the group of units at the cursor as one unit in cu, when its summary can answer (docs/COL-PREAGG.md section 10); false: read the units one by one */
extern bool col_super_take(SiftColSource *src, ColCursor *c, ColUnit *cu);
extern bool col_count_segment(SiftColSource *src, ColCursor *c);
/* scan.c: sel narrowed to the rows of the unit that the point directory lists, and the index of the predicate it answers; -1: the unit has none */
extern int sift_col_point_sel(const SiftColUnit *unit, SiftColSel *sel);

static inline void
set_ones(uint64 *bits, uint32 n)
{
	memset(bits, 0xFF, (n / 64) * sizeof(uint64));
	if (n % 64)
		bits[n / 64] = (UINT64CONST(1) << (n % 64)) - 1;
}

#define FOREACH_BIT(bits, nwords, r, ...) \
	do { \
		for (uint32 w_ = 0; w_ < (nwords); w_++) \
			for (uint64 x_ = (bits)[w_]; x_; x_ &= x_ - 1) \
			{ \
				uint32 r = w_ * 64 + pg_rightmost_one_pos64(x_); \
				__VA_ARGS__ \
			} \
	} while (0)

extern void seg_test(SiftColSource *src, SiftColSegment *seg, const SiftColDef *def, const SiftColPred *p, SiftCTest *t);
extern bool unit_chunk_raw(ColUnit *cu, int col, SiftCChunk *c);
extern bool unit_chunk(ColUnit *cu, int col, SiftCChunk *c);
extern bool unit_inline(ColUnit *cu, int col);
extern SiftCDict *unit_dict(ColUnit *cu, int col);
extern const uint64 *unit_allvisible(ColUnit *cu);
extern ColGroup *unit_group(ColUnit *cu, int group);
extern bool types_known(const ColUnit *cu, const SiftCChunk *c, uint32 mask, bool mixed, const uint64 *sel);

static inline void
make_test(SiftColSource *src, const SiftColDef *def, SiftCDict *dict, const SiftColPred *p, SiftCTest *t)
{
	src->cnt.tests_made++;
	sift_ctest_make(def, dict, p, t);
}

static inline uint64
dict_id(const ColUnit *cu, int col)
{
	if (cu->istr && cu->istr[col].dict)
		return cu->istr[col].id;
	return cu->seg ? (cu->seg->entry.gen << 8 | col) : (MEM_ID_FLAG | (uint64) cu->memid << 8 | col);
}

static inline SiftKey *
unit_keys(ColUnit *cu)
{
	if (cu->keys == NULL)
	{
		cu->keys = MemoryContextAlloc(cu->src->arena, (cu->v.nrows + 1) * sizeof(SiftKey));
		sift_cunit_keys(&cu->v, cu->sb, cu->keys);
	}
	return cu->keys;
}

/*
 * Whether the stored unit has no dead row, read from the liveness pages now and after the all-visible mask: a page turns all-visible only
 * after the VACUUM that made it so has removed the index entries of its dead tuples, so a mask that says "all-visible" is followed by liveness
 * that already shows those removals. The other order lets a unit read as clean before the VACUUM, and the page as all-visible after it, count
 * rows that no snapshot sees.
 */
static inline bool
unit_clean(ColUnit *cu)
{
	if (!cu->clean_ready)
	{
		unit_allvisible(cu);
		SIFT_COL_JITTER();
		cu->clean = !sift_cseg_dirty(cu->seg, cu->u);
		cu->clean_ready = true;
	}
	return cu->clean;
}


/* col/delta.c */
extern bool unit_pages_allvisible(ColUnit *cu);

#endif
