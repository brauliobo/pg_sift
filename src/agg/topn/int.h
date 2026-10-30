/*
 * agg/topn/int.h - what the files of agg/topn/ share
 * The keys, entries and state of a top-N reader, and the small helpers every part inlines.
 * The functions the parts call in one another are declared here.
 */
#ifndef SIFT_AGG_TOPN_DIR_INT_H
#define SIFT_AGG_TOPN_DIR_INT_H

#include "postgres.h"

#include <math.h>

#include "port/pg_bitutils.h"
#include "utils/memutils.h"
#include "utils/tuplesort.h"

#include "agg/part.h"
#include "agg/topn.h"
#include "agg/kern.h"
#include "core/cmp.h"

/* Superblocks per window of the visit order at most: the zones are looked at again, and the match set is asked for, once per window */
#define TOPN_WINDOW		8
/* Superblocks whose zones tell whether the sort column follows the table */
#define TOPN_PROBES		6
/* Rows a unit gives the heap, at least, for a column of the output to be read from it: decoding costs as much as fetching this many rows from the heap, and a unit that gives less than half the heap is not one most of the final rows come from */
#define TOPN_OUT_ROWS	16
/* Rows of a unit the first key of which is an integer, at least, for the best value of a segment to be looked at before its rows (vec_rows), and the rows of a segment */
#define TOPN_VEC_ROWS	512
#define TOPN_SEG		256
/*
 * ... and one row in this many of the unit passes the filters at least: the passes of the kernels cost the same whatever the rows are, so fewer rows go
 * to the heap one by one. Measured on 1,000,000 rows (LIMIT 10, keys in no order): at 30% and above the kernels take 25 to 45% fewer instructions, at 15-20%
 * the same to 14% fewer, at 10% and 6% 2 to 10% more (the decode of the filter and the key column is what costs, 28M of 28M instructions) and the time is the same;
 * a key that follows the table gains 1.3M of 2.3M at 10%, which is 0.03 ms. One row in 8 keeps all of it but that.
 */
#define TOPN_VEC_DENSITY	8
#define SPEC_KEYS(spec)		((List *) linitial((List *) (spec)))
#define SPEC_COUNT(spec)	((Node *) lsecond((List *) (spec)))
#define SPEC_OFFSET(spec)	((Node *) lthird((List *) (spec)))
#define SPEC_EXPRS(spec)	((List *) lfourth((List *) (spec)))
/* The bytes of a dictionary entry, copied out of the unit: what a heap entry holds for a key of byte strings (kept at int64 in TopEnt.k) */
typedef struct KeyBlob
{
	uint32		len;
	char		data[FLEXIBLE_ARRAY_MEMBER];
} KeyBlob;
/*
 * A sort expression: PostgreSQL's own ExprState over a virtual row of the leaves it reads, evaluated for every row of a unit that passes (the normal plan
 * evaluates a sort expression for each of them, so an error is raised for the same rows), its results kept as a vector of the type the key sorts by
 */
typedef struct TopIn
{
	int			leaf;
	Oid			typid;
	SiftSrcVecType vtype;
	int			scale;			/* numeric leaf: digits after the point */
	const SiftSrcVec *v;		/* the unit's */
	Datum	   *memo;			/* a leaf of strings: the datum of each code of the unit's dictionary, once built */
	bool	   *built;
} TopIn;
typedef struct TopExpr
{
	ExprState  *state;
	ExprContext *econtext;
	TupleTableSlot *slot;
	int			nin;
	TopIn	   *in;
	Oid			rtype;
} TopExpr;
#define TOP_RESID	4			/* leaves the conditions of a lazy filter may read */
#define TOP_GKEYS 4
typedef struct TopKey
{
	int			leaf;
	bool		desc;
	bool		nulls_first;
	SiftSrcVecType vtype;
	/* a key of byte strings (text, uuid, bytea, inet; vtype SCV_CODE): an entry holds a KeyBlob of the dictionary entry, compared as PostgreSQL's sort does */
	bool		blob;
	uint8		bkind;			/* BK_*: how two entries compare */
	Oid			typid;			/* the type of the column of the sort when the rows go to a tuplesort */
	Oid			sortop;
	Oid			collid;
	FmgrInfo   *cmp;			/* BK_FMGR: the btree comparison function of the type */
	int16		typlen;			/* BK_FMGR: the length of the type, -1 for a varlena (an interval is 16 bytes, padded in the dictionary) */
	int			xk;				/* TXK_*: what the key does to its leaf's value (agg/topn.h) */
	int64		xc;				/* TXK_COALESCE: the constant */
	Oid			xcoll;			/* TXK_LOWER, TXK_UPPER: the collation of the call */
	FmgrInfo   *xfn;
	struct TopExpr *x;			/* TXK_EXPR: the evaluator of the sort expression */
	SiftSrcVec	xv;				/* ... its result for the rows of the unit */
	KeyBlob   **xmemo;			/* TXK_LOWER, TXK_UPPER: the result for each code of the dictionary (xid, xbytes, xn) once it is known (NULL: not yet) */
	const void *xid;
	const char *xbytes;
	uint32		xn;
	/* a key of an enum: the OIDs of its values (ascending) and the position of each in the order of the type, which is the key */
	uint32		nenum;
	Oid		   *eoid;
	uint32	   *erank;
} TopKey;
#define BK_TEXT		0			/* the bytes of a text: varstr_cmp under the collation of the query (byte order under C, strcoll then byte order else) */
#define BK_UUID		1			/* 16 bytes: memcmp, as uuid_cmp */
#define BK_BYTEA	2			/* the varlena image: memcmp of the contents, then the length, as byteacmp */
#define BK_FMGR		3			/* any other datum image (inet): the type's own comparison function, on an aligned copy */
#define KEY_BLOB(v)		((const KeyBlob *) (uintptr_t) (v))
/* A key's value as a position in the sort order: NULLs in front of or behind the values, the values by the direction (class -1: no bound) */
typedef struct Rank
{
	int			cls;
	uint64		ord;
} Rank;
typedef struct TopEnt
{
	int64		k[SIFT_TOPN_MAX_KEYS];
	uint64		tie;			/* the TID's key in the result heap, the row in a unit's */
	uint32		row;
	uint32		oi;				/* the place of the entry's output columns in SiftTopN.ov (set by res_push) */
	uint8		nul;			/* bit j: key j is NULL */
	bool		hasv;			/* the output columns are in ov: the row is not fetched from the heap */
	ItemPointerData tid;
	uint16		part;			/* the partition of the row's heap (a partitioned table), else 0 */
} TopEnt;
/* The rows of a partition are fetched through its own slot and index fetch, its columns by the attribute numbers of the partition */
typedef struct TopFetch
{
	TupleTableSlot *slot;
	IndexFetchTableData *fetch;
	int		   *attno;
} TopFetch;
/*
 * The order code of a dictionary of a first key that compares as bytes (text under C, uuid, bytea): ord[code] puts the entries in the key's order, so that a
 * row is compared with the bar as two integers. exact: ord is 1 + the position of the entry in byte order (no two codes tie); else it is the first 8 bytes of the
 * entry, big endian, and entries that share them tie and are compared as bytes. A prefix is made once the rows compared with the dictionary as bytes reach
 * ORD_PREFIX_ROWS of its entries (making it costs as many compares: the table of ranks as well as the prefixes), and dropped for the dictionary when more than a quarter of the comparisons of a unit
 * tie. An exact code is made when the dictionary has its order (SiftSrcDict.order): stored, or sorted after the scans of a backend reached a few times its
 * entries. A dictionary of more than ORD_MAX_ENTRIES entries has none.
 */
#define ORD_CACHE		8
#define ORD_MAX_ENTRIES	(1 << 20)
#define ORD_PREFIX_ROWS	0.5
#define ORD_FORCE_ROWS	1000		/* rows per entry that make a dictionary sort at once (cdict_order) */
typedef struct OrdCode
{
	const void *id;
	const char *bytes;
	uint32		n;
	uint64		rows;			/* rows compared with the dictionary as bytes so far */
	uint64	   *ord;			/* NULL: not made */
	bool		exact;
	bool		useless;		/* a prefix that most rows tie on: the rows are compared as bytes */
} OrdCode;
/* The range of the key that a condition keeps: the ends that it bounds */
typedef struct GBox
{
	bool		empty;
	bool		has_lo;
	bool		has_hi;
	int64		lo;
	int64		hi;
} GBox;
struct SiftTopN
{
	int			limit;			/* rows the heap holds; 0: none wanted (a LIMIT that leaves nothing) */
	int			want;			/* TOPN_TIES: LIMIT + OFFSET, the rows that count; limit is more */
	bool		incomplete;		/* TOPN_TIES: the heap is full and its last row ties with the one that counts */
	bool		totals;			/* the aggregates over every row that passes are computed in this scan (TOPN_TOTALS): all units are read */
	bool		all;			/* no bound the heap can hold (NULL, past the cap, more keys): every row that passes is kept, sorted by the keys and the TID at the end */
	int64	   *mem;			/* ... in memory while they fit work_mem: nkey keys, the NULL flags (a bit for each key), the TID's number, for each row */
	int64		nmem;
	int64		capmem;
	int			stride;			/* int64s of a row of mem */
	Tuplesortstate *sort;		/* ... else a tuplesort of them (work_mem, then disk) */
	TupleTableSlot *sin,
			   *sout;			/* the rows going in and out of sort: the keys, then the TID as a number */
	int64		nsort;
	int			pos;			/* the next row of res */
	int			nkey;
	TopKey	   *key;
	int			nblob;			/* keys of byte strings */
	int			nexpr;			/* keys that are sort expressions */
	MemoryContext ucxt;			/* the copies of the unit's entries that the rows of one unit hold (reset at the next unit) */
	MemoryContext ocxt;			/* the order codes of the dictionaries (unit_ord) */
	struct OrdCode *oc;			/* ORD_CACHE of them */
	int			oc_next;
	int64		nord[3];		/* units compared by an exact order code, by a prefix, as bytes */
	int64		nties;			/* comparisons of the unit that a prefix could not decide */
	int			nout;
	int		   *attno;
	int			nov;			/* output columns the units give (the heap holds no row to fetch: all of them are leaves), else 0 */
	int		   *oleaf;			/* ... their leaves, and the types their values are given as */
	Oid		   *otyp;
	bool	   *owide;			/* ... int64 (else int32) vectors */
	int		   *okey;			/* ... the sort key that is the same column (the entry holds its value), else -1 */
	int			nread;			/* ... columns that are not a key: read from the unit */
	int64	   *ov;				/* limit entries of nov values each, and whether each is NULL: the entries of heap take a place each (TopEnt.oi) */
	bool	   *ovn;
	int64	   *uv;				/* the values of the rows of lh */
	bool	   *uvn;
	int			gwant;			/* LIMIT + OFFSET (at most INT_MAX), whether or not a heap can hold them: the rows the walk of the order array gives (0: LIMIT NULL) */
	int64		skip;			/* the first rows the Limit above throws away (OFFSET): given as rows of NULLs, not read from the heap */
	int64		emitted;
	TopEnt	   *heap;			/* the best rows so far, the worst at heap[0] */
	int			n;
	TopEnt	   *lh;				/* the best rows of the unit */
	int			ln;
	uint32	   *crow;			/* the rows of the unit that pass, their first sort key and whether it is NULL */
	int64	   *ck;
	uint8	   *cnul;
	int			ccap;
	TopEnt	   *ce;				/* the rows whose first key is not worse than the n-th of the unit's and of the heap */
	uint32	   *cand;			/* the rows of ce */
	uint32	   *rows;
	int64	   *skey;			/* sort: the keys of the rows of a unit, nkey for each row, and whether each is NULL */
	bool	   *snull;
	uint32	   *rwin;			/* a window of a sorted run (sift_topn_unit) */
	uint32	   *rpass;			/* the rows of the window that pass, whose keys are read */
	uint32	   *rmrow;			/* lazy: those of them that the key does not decide, whose other columns are read */
	uint16	   *rslot;			/* the place of the key of each entry of the window in that read */
	uint16	   *rmay;			/* ... their entries of the window */
	int			budget;			/* rows a unit gives in the first pass of a walk through sorted runs (0: all it can), sift_topn_run_budget */
	bool		revisit;		/* the second pass: only the units the first left unfinished */
	bool		yielded;		/* trunc[rpos] was given out and is not yet done */
	struct RunTrunc *trunc;
	int			ntrunc,
				captrunc,
				rpos;
	ItemPointerData *tids;		/* the TIDs of the rows of a unit (sort), or of the heap rows of a unit */
	TopEnt	   *res;			/* after sift_topn_finish: the heap in order */
	/* the visit order */
	uint32		nsb;
	bool		reverse;			/* the superblocks are visited from the last */
	bool		follows;			/* the first sort column follows the superblocks (as far as the probes say): the visit starts at its best end */
	uint32		left;				/* superblocks not yet in a window */
	uint32		size;				/* superblocks of the next window */
	uint32		nprobe;
	struct
	{
		uint32		sb;
		int64		best;
	}			probe[TOPN_PROBES];
	/* the walk (sift_topn_walk_*): the zone and the number of units of every superblock, and the superblocks by their best bound */
	int			wkeys;				/* leading sort keys whose zones bound the walk */
	const struct WalkBase *wb;	/* the bounds of the superblocks and their order (kept between queries when the source can tell what they depend on) */
	uint32	   *whead;			/* the superblocks that hold rows of a write buffer, which the walk visits first: their zones are not exact */
	int			nhead;
	int			hpos;
	uint32	   *wfull;			/* the whole order, for a parallel scan, when there is a head */
	uint32		wpos;
	Snapshot	snap;
	SiftSrcSource *src;
	Relation	rel;			/* the table the rows are fetched from: partitioned, or the heap */
	int			nfetch;			/* 1, or the partitions of a partitioned table: the fetches are made when a row of the partition is the first */
	TopFetch   *pf;				/* the heap row of a TID: the version an index scan finds, along a HOT chain */
	int64		skipped;
	int64		nrun;			/* units read through their sorted run */
	int64		nlazy;			/* ... of them with no filter run (sift_topn_lazy) */
	int64		nrhead;			/* units answered from the head of their sorted run */
	int64		nmono;			/* units whose best rows were read from an end of a key chunk that follows the row order (mono_unit) */
	bool		wkeep;			/* the write buffers' superblocks are known (whead): a superblock outside them has no row in memory */
	int64		fetched;
	GBox		kbox;			/* the range of the first key that the conditions on it keep (sift_topn_key_range): a sorted run is read from its first entry in it */
	const SiftTopGPred *resid;	/* sift_topn_lazy: the conditions of the scan, which a unit read through its sorted run tests on the rows it takes (its filters are not run) */
	int			nresid;
	int			rleaf[TOP_RESID];	/* ... the leaves they read, and whether each is an int64 (else int32) vector */
	bool		rwide[TOP_RESID];
	bool		nogord;			/* TOPN_NOGORD: the plan counts on the heads of the sorted runs */
	bool		gtext;			/* ... the one key is a text column, ranked in the arrays by its collation (key[0].collid) */
	bool		gok;			/* every key is a plain integer or time column of the heap (col/gorder.h can order the first, the others are read from the heap row): gatt and gtyp are theirs */
	int			gatt[TOP_GKEYS];
	Oid			gtyp[TOP_GKEYS];
	struct GRow *gq;			/* sift_topn_gorder with more keys: the rows of the whole groups of the first key that make the n, in order */
	int			gqn;
	int			gqpos;
	const SiftTopGPred *gcond;	/* sift_topn_gorder: what the key of an entry must satisfy */
	struct SiftGOrd *go;		/* the global order of the key read in place of the units (sift_topn_gorder): rows come from the heap in the order of the key */
	pg_atomic_uint64 *shared;	/* parallel scan: the first-key rank of the n-th row of any participant (sift_topn_share) */
};
typedef struct EnumItem
{
	Oid			oid;
	float4		order;
	uint32		rank;
} EnumItem;
/* A unit whose run the first pass left at the group of rank next: the rows before it are taken */
typedef struct RunTrunc
{
	uint64		id;
	uint32		sb;
	Rank		next;
	bool		done;
} RunTrunc;
/*
 * The order of the walk, kept by the backend: the bounds of all the superblocks and the order of their best rows depend on the zones of the stored units alone,
 * and a query that sorts by the same keys of the same units (the same file and version of the directory) walks the same order. Reading every zone and sorting
 * the superblocks cost O(units) for each such query; the kept order costs the superblocks it visits. The superblocks that hold rows of a write buffer are
 * not in the zones (the buffers change with no new version): each query lists them (sift_colsrc_walk_stamp) and visits them first, as their bound, which is
 * none, puts them.
 */
#define WALK_CACHED		4			/* orders kept */
#define WALK_MEM_SBS	64			/* superblocks with write buffer rows beyond which an order is not kept */
#define WALK_KEEP_BYTES	((Size) 16 << 20)
typedef struct WalkKey
{
	SiftSrcWalkStamp st;
	uint32		nsb;
	int			wkeys;
	struct
	{
		int			leaf;
		int			vtype;
		int			xk;
		int64		xc;
		bool		desc;
		bool		nulls_first;
	}			k[SIFT_TOPN_MAX_KEYS];
} WalkKey;
typedef struct WalkBase
{
	WalkKey		key;
	uint32		nsb;
	Rank	   *bound;			/* wkeys per superblock */
	uint32	   *units;			/* the units of the stored segments in the superblock */
	uint32	   *ord;			/* the superblocks by their best bound */
	uint32	   *pos;			/* the place of each superblock in ord */
	uint32	   *tail;			/* nsb + 1: the units of ord[i ..] */
	MemoryContext cx;
	uint64		used;
} WalkBase;
/* a row of a group of the first key: its address and the values of the keys, which the heap row gave */
typedef struct GRow
{
	ItemPointerData tid;
	bool		isnull[TOP_GKEYS];
	int64		key[TOP_GKEYS];
} GRow;
extern KeyBlob * xform_blob(SiftTopN *t, const TopKey *k, const char *p, uint32 len);

extern bool row_val(SiftTopN *t, int j, const SiftSrcVec *v, int i, int64 *out);
extern SiftSrcVecType expr_vtype(Oid typid);
extern TopExpr * expr_open(List *xs, const SiftSrcLayout *lay, Oid rtype, ExprContext *outer, MemoryContext cxt);
extern void expr_eval_unit(SiftTopN *t, TopKey *k, SiftTopLeaf leaf, void *ctx, const uint64 *rmask, int nrow);
extern void key_rows(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int j, int n, SiftSrcVec *v);
extern void res_push(SiftTopN *t, const TopEnt *e, const int64 *v, const bool *vn);
extern void heap_push(const SiftTopN *t, TopEnt *h, int *n, const TopEnt *e);
extern int bytes_cmp(const TopKey *k, const char *a, uint32 la, const char *b, uint32 lb);
extern int key_cmp_n(const SiftTopN *t, const TopEnt *a, const TopEnt *b, int nk);
extern void sort_rows(uint32 *data, size_t n);
extern int mem_cmp(const void *a, const void *b, void *arg);

extern Tuplesortstate * sort_begin(SiftTopN *t);
extern void grow_unit_buffers(SiftTopN *t, int nrow);
extern void finish_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, int nc);
extern bool bar_rank(const SiftTopN *t, Rank *bar);
extern void share_bar(SiftTopN *t);
extern void sort_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow);

extern bool run_ready(const SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, uint32 *nvals, uint32 *nnull, uint64 *id);
extern bool run_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow, bool lazy);
extern bool heads_ok(const SiftTopN *t);

extern bool head_unit(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const uint64 *rmask, int nrow);
extern void unit_blob(SiftTopN *t, SiftSrcSource *src, const SiftSrcUnit *unit, const SiftSrcVec *key0, const uint64 *rmask, int nrow);
extern bool walk_in_head(const SiftTopN *t, uint32 sb);

extern SiftSrcZone key_zone(const TopKey *k, const SiftSrcZone *z);

extern bool next_tid(SiftTopN *t, ItemPointerData *tid, uint16 *part);

extern bool gcond_test(const SiftTopGPred *p, TupleTableSlot *slot);
extern int gcond_entry(const SiftTopGPred *p, int gl, bool top, int64 key, bool isnull);
extern bool skipped_row(SiftTopN *t, TupleTableSlot *slot);

extern void fetch_begin(SiftTopN *t, int part);

static inline KeyBlob *
blob_new(MemoryContext cxt, const char *p, uint32 len)
{
	KeyBlob    *b = MemoryContextAlloc(cxt, offsetof(KeyBlob, data) + len);

	b->len = len;
	memcpy(b->data, p, len);
	return b;
}

static inline int
key_cmp(const SiftTopN *t, const TopEnt *a, const TopEnt *b)
{
	return key_cmp_n(t, a, b, t->nkey);
}

static inline int
ent_cmp(const SiftTopN *t, const TopEnt *a, const TopEnt *b)
{
	int			c = key_cmp(t, a, b);

	return c != 0 ? c : SIFT_CMP3(a->tie, b->tie);
}

/* the position of the enum value oid in the order of its type */
static inline int64
enum_rank(const TopKey *k, Oid oid)
{
	uint32		lo = 0,
				hi = k->nenum;

	SIFT_BISECT(uint32, mid, lo, hi, k->eoid[mid] < oid);
	if (lo == k->nenum || k->eoid[lo] != oid)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("enum value %u was added while the scan was running", oid)));
	return k->erank[lo];
}

/* A float's place in the sort order as an integer (the order of float8: -0 is +0, NaN is the largest) and back (any NaN key reads as a NaN) */
static inline int64
f8_key(double d)
{
	int64		b;

	if (isnan(d))
		return PG_INT64_MAX;
	d = d == 0 ? 0.0 : d;
	memcpy(&b, &d, sizeof(b));
	return b < 0 ? b ^ PG_INT64_MAX : b;
}

static inline double
f8_of_key(int64 k)
{
	double		d;

	k = k < 0 ? k ^ PG_INT64_MAX : k;
	memcpy(&d, &k, sizeof(d));
	return d;
}

/* the key of row i; floats map to integers of the same order (NaN last); a numeric NaN is PG_INT64_MAX in the vector (SiftSrcVec.ovf) */
static inline bool
row_key(const TopKey *k, const SiftSrcVec *v, int i, int64 *out)
{
	if (v->isnull && v->isnull[i])
	{
		*out = k->xc;
		return k->xk == TXK_COALESCE;
	}
	switch (k->vtype)
	{
		case SCV_I32:
			*out = k->nenum ? enum_rank(k, ((const int32 *) v->data)[i]) : ((const int32 *) v->data)[i];
			break;
		case SCV_I64:
			*out = ((const int64 *) v->data)[i];
			break;
		default:
			*out = f8_key(((const double *) v->data)[i]);
	}
	return true;
}

/*
 * The entry `code` of a dictionary as the bytes of key j: a datum image (bytea, inet) is stored padded, the length the key means is its varlena
 * header's; a lower() or upper() key is the call's result, evaluated once for an entry of a unit
 */
static inline void
key_entry(SiftTopN *t, int j, const SiftSrcDict *d, uint32 code, const char **p, uint32 *len)
{
	TopKey	   *k = &t->key[j];

	*p = d->bytes + d->off[code];
	*len = d->off[code + 1] - d->off[code];
	if (k->bkind == BK_BYTEA || (k->bkind == BK_FMGR && k->typlen < 0))
	{
		varattrib_4b h;

		memcpy(&h, *p, sizeof(uint32));
		*len = VARSIZE_4B(&h);
	}
	else if (k->bkind == BK_FMGR)
		*len = k->typlen;
	if (k->xfn)
	{
		KeyBlob    *b;

		if (k->xid != d->id || k->xbytes != d->bytes || k->xn != d->n)
		{
			k->xmemo = MemoryContextAllocZero(t->ucxt, Max(d->n, 1) * sizeof(KeyBlob *));
			k->xid = d->id;
			k->xbytes = d->bytes;
			k->xn = d->n;
		}
		if ((b = k->xmemo[code]) == NULL)
			b = k->xmemo[code] = xform_blob(t, k, *p, *len);
		*p = b->data;
		*len = b->len;
	}
}

/* the place of row in the ascending rows[0, n), which holds it */
static inline uint32
row_at(const uint32 *rows, int n, uint32 row)
{
	int			lo = 0,
				hi = n;

	SIFT_BISECT(int, mid, lo, hi, rows[mid] < row);
	return lo;
}

static inline uint64
tid_key(const ItemPointerData *tid)
{
	return ((uint64) ItemPointerGetBlockNumber(tid) << 16) | ItemPointerGetOffsetNumber(tid);
}

static inline void *
grow(SiftTopN *t, void *p, Size sz)
{
	return p ? repalloc(p, sz) : MemoryContextAlloc(GetMemoryChunkContext(t), sz);
}

/* The partition of the unit the source is at: the TIDs it gives are those of that partition's heap */
static inline uint16
unit_part(const SiftTopN *t, SiftSrcSource *src)
{
	return t->rel->rd_rel->relkind == RELKIND_PARTITIONED_TABLE ? sift_aggpart_current(src) : 0;
}

/* The key a row has in the order of ties: its TID, those of the partitions one after the other */
static inline uint64
tie_key(const ItemPointerData *tid, uint16 part)
{
	return tid_key(tid) | (uint64) part << 48;
}

static inline void
tie_tid(int64 tie, ItemPointerData *tid, uint16 *part)
{
	ItemPointerSet(tid, (tie >> 16) & PG_UINT32_MAX, tie & 0xFFFF);
	*part = tie >> 48;
}

static inline Rank
rank_of(const TopKey *k, bool isnull, int64 v)
{
	Rank		r = {isnull ? (k->nulls_first ? 0 : 2) : 1, 0};

	if (!isnull)
		r.ord = k->desc ? ~((uint64) v ^ UINT64CONST(0x8000000000000000)) : (uint64) v ^ UINT64CONST(0x8000000000000000);
	return r;
}

static inline bool
rank_after(Rank a, Rank b)
{
	return a.cls != b.cls ? a.cls > b.cls : a.ord > b.ord;
}

static inline Rank
ent_rank(const SiftTopN *t, const TopEnt *e)
{
	return rank_of(&t->key[0], e->nul & 1, e->k[0]);
}

/* A row of mem: the keys (0 for a NULL), the NULL flags, the TID's number */
static inline int64 *
mem_row(const SiftTopN *t, int64 i)
{
	return t->mem + i * t->stride;
}

/* does a value of the column pass the comparison: the one test of a heap row (gcond_test) and of a row of a unit (resid_test) */
static inline bool
gcond_cmp(int op, int64 key, int64 val)
{
	switch (op)
	{
		case SCMP_EQ:
			return key == val;
		case SCMP_NE:
			return key != val;
		case SCMP_LT:
			return key < val;
		case SCMP_LE:
			return key <= val;
		case SCMP_GT:
			return key > val;
		default:
			return key >= val;
	}
}

/* The value of the key as the entries of the global order hold it */
static inline int64
heap_key(Oid typid, Datum d)
{
	switch (typid)
	{
		case INT2OID:
			return DatumGetInt16(d);
		case INT4OID:
		case DATEOID:
			return DatumGetInt32(d);
		default:
			return DatumGetInt64(d);
	}
}

/* The sign of a row of the first key (a NULL, or the entry `code` of dictionary d) against the entry bar, by the first key alone: < 0 before it, > 0 after it */
static inline int
vs_bar(SiftTopN *t, const SiftSrcDict *d, bool isnull, uint32 code, const TopEnt *bar)
{
	const TopKey *k = &t->key[0];
	bool		bn = bar->nul & 1;
	const char *p;
	uint32		len;
	int			c;

	if (isnull != bn)
		return isnull == k->nulls_first ? -1 : 1;
	if (isnull)
		return 0;
	key_entry(t, 0, d, code, &p, &len);
	c = bytes_cmp(k, p, len, KEY_BLOB(bar->k[0])->data, KEY_BLOB(bar->k[0])->len);
	return k->desc ? -c : c;
}

/* Is the row strictly after the bar? The bar only moves toward the front, so an entry that was after it stays after it: worse[code] remembers it, and a unit of few distinct strings compares each once */
static inline bool
after_bar(SiftTopN *t, uint8 *worse, const SiftSrcDict *d, bool isnull, uint32 code, const TopEnt *bar)
{
	if (isnull)
		return vs_bar(t, d, true, 0, bar) > 0;
	if (worse[code])
		return true;
	if (vs_bar(t, d, false, code, bar) <= 0)
		return false;
	worse[code] = 1;
	return true;
}

#endif							/* SIFT_AGG_TOPN_DIR_INT_H */
