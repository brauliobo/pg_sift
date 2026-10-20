/*
 * bm25/eval_int.h - shared between the files of the evaluator (eval.c, wbind.c, phrase.c, iter.c, bounds.c, rank.c, count.c).
 */
#ifndef SIFT_EVAL_INT_H
#define SIFT_EVAL_INT_H

#include "bm25/eval.h"
#include "bm25/kernels.h"
#include "bm25/wbuf.h"
#include "bm25/segment.h"

typedef struct LeafTerm
{
	char	   *term;
	int			len;
	SiftTermInfo info;
	const uint8 *blob;			/* a wide prefix's term: its blob in memory (NULL: inline); else read from the segment */
	float4		w;				/* scoring leaves: (boost * idf) * (k1 + 1), idf of the df over all segments */
} LeafTerm;

/* A query leaf and its dictionary terms in every segment */
typedef struct EvalLeaf
{
	const SiftQNode *node;
	SiftTermMatcher *m;			/* NULL for a plain term */
	uint32		hash;			/* plain term: sift_term_hash */
	bool		scoring;		/* a positive leaf: counts in the score */
	const SiftQNode *phrase;	/* the phrase this leaf is a term of, or NULL */
	bool		optional;		/* phrase leaf that a match need not satisfy */
	char	   *wide;			/* prefix with more terms than sift.max_expansions: the term it reads as (indexed_leaf's) */
	int			widelen;
	double		tail_none;		/* estimate: P(no document has any term of the prefix past the limit) over all segments */
	double		tail_postings;
	double		tail_terms;
	int		   *nterms;			/* [nsegs] */
	LeafTerm  **terms;			/* [nsegs] */
	int			nuniq;			/* distinct terms over all segments, ascending, with their weights */
	LeafTerm  **uniq;
	HTAB	   *mtab;			/* scoring: df of the leaf's terms in the write buffer docs (WbufTermEnt) */
} EvalLeaf;

/* A term of a scoring leaf found in write buffer docs, zero padded */
typedef struct WbufTermEnt
{
	char		term[SIFT_MAX_TERM_BYTES + 1];
	uint32		df;
} WbufTermEnt;

/* A live matching doc of a write buffer; scoring plans keep the doc to score it once the weights are known */
typedef struct WbufHit
{
	SiftKey		key;
	float4		score;
	SiftWbufDoc *doc;
} WbufHit;

typedef enum EvalKind
{
	EN_NOTHING,
	EN_LEAF,
	EN_AND,
	EN_OR,
	EN_NOT						/* only as a child of EN_AND */
} EvalKind;

typedef struct EvalNode
{
	EvalKind	kind;
	int			leaf;
	int			nchild;
	struct EvalNode **child;
	int			phrase;			/* the phrase (PhraseCheck) this subtree is the relaxation of, -1: none */
} EvalNode;

/*
 * A phrase the index decides from the positions of its terms where its segments have them: leaf[i] is
 * the leaf of the i-th term, order the terms by ascending df (the rarest term narrows the candidates
 * first). A required phrase (on a path of ANDs from the root) refines the final matches; an optional
 * one (under an OR) is a cursor of its own that yields the docs which hold it.
 */
typedef struct PhraseCheck
{
	int			nterms;
	int		   *leaf;
	int		   *order;
	bool		required;
} PhraseCheck;

struct SiftPlan
{
	SiftIndexState *st;
	MemoryContext mcxt;
	int			nsegs;
	int		   *segno;			/* directory index of each immutable segment */
	SiftSegment **seg;
	int			nwbuf;
	int		   *wbufno;			/* directory index of each write buffer */
	WbufHit	   *mhit;			/* live matches of the write buffers, ascending */
	int64		nmhit;
	uint64		seg_ndocs;		/* live documents and tokens of the immutable segments */
	uint64		seg_sum_dl;
	uint64		wbuf_ndocs;		/* live documents and tokens of the write buffers */
	uint64		wbuf_sum_dl;
	int			nleaves;
	int			maxleaves;
	EvalLeaf   *leaves;
	EvalNode   *root;
	EvalNode   *sure;			/* unranked plans with a phrase: the matches the bitmaps decide alone, NULL when none */
	SiftQuery **queries;
	int			nqueries;
	bool		recheck;		/* ranked scans: a phrase that is not required, every candidate is rechecked in the heap */
	bool		hardrecheck;	/* unranked scans: a negated phrase, every candidate is rechecked in the heap */
	PhraseCheck *phr;			/* phrases decided by positions where the segment has them; the heap decides the other candidates */
	int			nphr;
	int		   *pleaf;			/* the distinct leaves of the required phrases */
	int			npleaf;
	bool	   *phok;			/* [nsegs * nphr] the segment holds the positions of all terms of the phrase */
	bool	   *segheap;		/* [nsegs] some phrase is not decided by the segment's positions */
	bool		heap;			/* some candidates need the heap recheck */
	bool		rescore;		/* a phrase the score depends on: the exact score comes from the text */
	bool		capped;			/* estimate: stop expanding at the limit */
	bool		scoring;
	const SiftFilter *filter;	/* ranked search: the tuples worth scoring */
	SiftBm25	bm;
	SiftPlanStats stats;
};

typedef struct SharedHit
{
	SiftKey		key;
	float4		score;
} SharedHit;

/* What follows the header: nmhit SharedHit, then for every leaf a count and that many WbufTermEnt */
typedef struct SharedBind
{
	SiftDirSnapshot dir;
	uint64		seg_ndocs;
	uint64		seg_sum_dl;
	uint64		wbuf_ndocs;
	uint64		wbuf_sum_dl;
	int64		nmhit;
	int			nleaves;
} SharedBind;

/*
 * Decides the required phrases of a doc from the positions of their terms. The caller points the
 * context at the terms' groups of the doc's SB (posctx_group) and names the doc (phrases_hold).
 */
typedef struct PosCtx
{
	const SiftPlan *plan;
	MemoryContext mcxt;
	SiftPostingCursor **pc;		/* [leaf] the term's cursor and its group in the SB */
	const sift_grp **g;
	int		   *wpage;			/* [leaf] the walk over the group's pages: the term has widx pages below wpage, */
	uint32	   *widx;
	uint32	   *wrank;			/* ... holding wrank postings */
	SiftPosBuf *pb;				/* [leaf] positions in the current doc */
	uint64	   *stamp;			/* [leaf] the doc pb belongs to */
	uint64		doc;
	int			page;			/* the current doc: page of the SB and offset - 1 */
	int			off;
	uint32	   *start;			/* scratch: the phrase starts that fit the terms so far */
	uint32		startcap;
} PosCtx;

/*
 * What the iterator keeps of a segment's tree: the leaves' cursors, from which the phrase walkers
 * read the groups of the matches, and the context that decides phrases
 */
typedef struct IterCurs
{
	SiftPostingCursor ***leafpc;	/* [leaf] -> the cursors of the leaf's terms in this segment */
	PosCtx	   *pos;
} IterCurs;

struct SiftMatchIter
{
	SiftPlan   *plan;
	sift_cur  **root;			/* per segment, NULL: no matches */
	sift_cur  **sure;			/* per segment: the matches that need no recheck, NULL: none (plan->sure) */
	const sift_grp **grp;		/* scratch: current group per segment */
	int64		mpos;			/* next write buffer match */
	uint32		next_sb;
	uint32		sb_end;			/* the visited range ends before this superblock */
	SiftNextRangeFn next_range;	/* parallel: where the next range comes from */
	void	   *range_arg;
	uint32		sb;
	int			page;			/* next page of the loaded SB to return */
	uint64		pages[SIFT_PAGEMASK_WORDS];
	uint64	  (*om)[SIFT_OFFMASK_WORDS];	/* merged offset masks by page of the SB */
	PosCtx	   *pos;
	IterCurs   *curs;				/* [segment] */
	uint64		heap[SIFT_PAGEMASK_WORDS];	/* pages of the SB whose candidates the heap must recheck */
	uint64		spages[SIFT_PAGEMASK_WORDS];	/* the same for the matches that need no recheck */
	uint64	  (*som)[SIFT_OFFMASK_WORDS];
};

/*
 * The cursors of one segment's tree. The ranker repositions them (posting
 * cursors rewind, operator cursors reset) and scores from the posting
 * cursors of the leaves; the iterator needs neither (cs NULL).
 */
typedef struct SegCurs
{
	List	   *posting;			/* SiftPostingCursor * */
	List	   *ops;				/* sift_cur * */
	SiftPostingCursor ***leafpc;	/* [leaf] -> the cursors of the leaf's terms in this segment */
	sift_cur  **leafor;				/* [leaf] -> its OR, for the multi-term scoring leaves the ranker narrows per unit */
} SegCurs;

#define BLK_SLOTS	(SIFT_BLK_PAGES * SIFT_KERN_SLOTS)

typedef uint64 BlockMask[SIFT_BLK_PAGES][SIFT_OFFMASK_WORDS];

typedef struct GidUB
{
	uint32		gid;
	float4		ub[SIFT_BLKS_PER_SB];
} GidUB;

/* Bounds of a leaf's postings per SB, ascending gid; pos walks them while the units are built */
typedef struct LeafUB
{
	int			n;
	int			pos;
	GidUB	   *list;
} LeafUB;

/* One dictionary term of a leaf in a segment and its group in the unit's SB */
typedef struct RkTerm
{
	SiftPostingCursor *pc;			/* the tree's cursor: bounds, clone source */
	SiftPostingCursor *sc;			/* own cursor for scoring: the tree's seeks skip groups */
	SiftPostingCursor *gc;			/* the cursor g belongs to: pc when the tree already decoded it, else sc */
	float4		w;
	const SiftGroupBounds *gb;		/* bounds per SB, ascending gid */
	int			ngb;
	float4		ubw;				/* bound weight: ub = ubw * blk_bound */
	float4		tub;				/* the largest ub of the term in the segment */
	const SiftGroupBounds *bb;		/* bounds in the unit's SB */
	bool		live;				/* may lift a document over theta somewhere in the unit */
	bool		act;				/* scores in the current block */
	uint32		pce;				/* RankSeg.epoch the cursors are positioned in */
	uint32		sce;
	const sift_grp *g;				/* NULL: no postings in this SB */
	uint8	   *tf;
	uint32		tfcap;
	bool		decoded;
	uint32		base[SIFT_BLKS_PER_SB + 1];	/* tf rank of each block's first posting */
} RkTerm;

typedef struct RankSeg
{
	int			s;
	SegCurs		cs;
	sift_cur   *root;
	int64		last;				/* last SB requested from the cursors */
	uint32		epoch;				/* bumped when the cursors go back to the start */
	LeafUB	   *lub;				/* [leaf] */
	float4	   *lmax;				/* [leaf] the largest ub of its terms */
	RkTerm	   *term;
	int			nterm;
	int		   *first;				/* [leaf] index of its first term */
} RankSeg;

typedef struct Unit
{
	int			seg;
	uint32		gid;
	float4		maxub;
	int			nblk;
	float4		ub[SIFT_BLKS_PER_SB];
} Unit;

typedef struct RankWork
{
	float4		scores[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	float4		knorm[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	float4		leafbuf[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	float4		tmp[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	float4		vals[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	float4		part[BLK_SLOTS] pg_attribute_aligned(SIFT_SIMD_ALIGN);
	uint16		slot[BLK_SLOTS];
	BlockMask	res pg_attribute_aligned(SIFT_SIMD_ALIGN);
	BlockMask	tm pg_attribute_aligned(SIFT_SIMD_ALIGN);
	BlockMask	tmc pg_attribute_aligned(SIFT_SIMD_ALIGN);
	BlockMask	lu pg_attribute_aligned(SIFT_SIMD_ALIGN);
	BlockMask	flt pg_attribute_aligned(SIFT_SIMD_ALIGN);
	uint16		zero[SIFT_KERN_SLOTS];
	const uint16 *dl[SIFT_BLK_PAGES];
} RankWork;

struct SiftRanker
{
	SiftPlan   *plan;
	SiftVerifyFn verify;
	void	   *arg;
	MemoryContext tmpcx;
	RankSeg    *seg;
	Unit	   *unit;
	int			nunit;
	RankWork   *wk;
	float4	   *leafub;				/* [leaf] scratch: its ub in the current block */
	int		   *ord;				/* [leaf] scratch: the leaves present in the block, best bound first */
	float4	   *rest;				/* [leaf] scratch: the bounds of the leaves after ord[i] */
	int64		k;
	int64		K;					/* hits kept by the current search */
	SiftHit    *hit;				/* keyed by the visible tuple's ctid */
	HTAB	   *hot;				/* visible ctid -> index entry, where they differ */
	int64		nhit;
	int64		hitcap;				/* slots of hit */
	int64		pos;
	bool		searched;
	bool		complete;
	SiftPar    *par;				/* parallel: the first search is shared with the other participants */
	bool		par_active;			/* ... and is running now */
	PosCtx	   *phrpos;
	bool		posok;				/* the phrase walkers sit on the current unit's groups */
};

static inline int
leaf_term_cmp(const void *a, const void *b)
{
	const LeafTerm *x = *(LeafTerm *const *) a,
			   *y = *(LeafTerm *const *) b;
	int			c = memcmp(x->term, y->term, Min(x->len, y->len));

	return c ? c : x->len - y->len;
}

/* Statistics count the live docs of the write buffers too: those of the immutable ones are in p->seg_ndocs and p->seg_sum_dl */
static inline void
bind_stats(SiftPlan *p, uint64 wbuf_ndocs, uint64 wbuf_sum_dl)
{
	double		sum = p->seg_sum_dl + wbuf_sum_dl;

	p->stats.ndocs = p->seg_ndocs + wbuf_ndocs;
	p->stats.avgdl = p->stats.ndocs > 0 ? sum / p->stats.ndocs : 0;
}

static inline void
posctx_group(PosCtx *c, int leaf, SiftPostingCursor *pc, const sift_grp *g)
{
	c->pc[leaf] = pc;
	c->g[leaf] = g;
	c->wpage[leaf] = 0;
	c->widx[leaf] = 0;
	c->wrank[leaf] = 0;
}

static inline uint8 *
cursor_buf(MemoryContext cx)
{
	return MemoryContextAllocAligned(cx, SIFT_CUR_BUF, SIFT_SIMD_ALIGN, 0);
}

/* wbind.c */
extern void bind_wbuf(SiftPlan *p);
extern void import_wbuf(SiftPlan *p, const SharedBind *h);
extern void bind_scoring(SiftPlan *p);
extern void score_wbuf(SiftPlan *p);
extern const LeafTerm *leaf_weighed(const EvalLeaf *l, const char *term, int len);

/* phrase.c */
extern PosCtx *posctx_create(SiftPlan *p);
extern bool phrases_hold(PosCtx *c, int s, int page, int off);
extern sift_cur *phrase_cursor(SiftPlan *p, int s, int ph, sift_cur *kid, const IterCurs *ic, MemoryContext cx);
extern bool doc_pass(SiftPlan *p, text *doc, float4 *score);

/* iter.c */
extern sift_cur *seg_cursor(SiftPlan *p, int s, const EvalNode *n, SegCurs *cs, const IterCurs *ic, MemoryContext cx);
extern bool iter_find_sb(SiftMatchIter *it, uint32 *sbp);

#endif							/* SIFT_EVAL_INT_H */
