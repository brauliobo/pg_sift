/*
 * bm25/eval/int.h - shared between the files of the evaluator (eval.c, wbind.c, phrase.c, iter.c, bounds.c, rank.c, count.c).
 */
#ifndef SIFT_EVAL_INT_H
#define SIFT_EVAL_INT_H

#include "bm25/eval/eval.h"
#include "bm25/kernels/kernels.h"
#include "bm25/hitspill.h"
#include "bm25/wbuf.h"
#include "bm25/segment/segment.h"
#include "core/cmp.h"
#include "core/tidbridge.h"

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
	uint8		field;			/* the index field the leaf searches (1..), 0: the whole text */
	SiftTermMatcher *m;			/* NULL for a plain term */
	uint32		hash;			/* plain term: sift_term_hash */
	bool		scoring;		/* a positive leaf: counts in the score */
	const SiftQNode *phrase;	/* the phrase this leaf is a term of, or NULL */
	bool		optional;		/* phrase leaf that a match need not satisfy */
	char	   *wide;			/* prefix with more terms than sift.max_expansions: the term it reads as (indexed_leaf's) */
	int			widelen;
	char	   *wide_key;		/* wide: the name of the leaf for the cache of unions (NULL when too long to be one) */
	int			wide_keylen;
	bool		deferred;		/* wide in a ranked plan: terms are the few that can matter to a search (the largest bounds first), the others are read when a K-th score does not settle them */
	float4		wide_w;			/* wide: the weight of the leaf */
	bool		warm;			/* deferred, weighed from the memo (bm25/wmemo.c): its top terms are not chosen, and the search of warm_k hits reads the terms that reach warm_theta */
	float4		warm_theta;
	int64		warm_k;
	uint8	   *tail_mb;		/* [nsegs] deferred: the largest bound of a term that is not in terms, 0 for none */
	uint8	   *all_mb;			/* [nsegs] deferred: the largest bound of any term of the leaf */
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

/* What a scoring plan keeps of a write buffer doc to score it once the weights are known: its lengths and the best tf of each scoring leaf it has */
typedef struct WbufTfs
{
	uint16		dl;
	uint8		nfields;		/* of field_dl, NULL for a doc written without */
	int32		n;				/* of leaf (ascending) and tf */
	const uint16 *field_dl;
	const int32 *leaf;
	const uint8 *tf;
} WbufTfs;

/* A live matching doc of a write buffer */
typedef struct WbufHit
{
	SiftKey		key;
	float4		score;
	WbufTfs    *tfs;
} WbufHit;

typedef enum EvalKind
{
	EN_NOTHING,
	EN_LEAF,
	EN_AND,
	EN_OR,
	EN_NOT,						/* the live documents the child does not match; a child of an EN_AND with a positive child is only subtracted */
	EN_ALL						/* every live document */
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
	int		   *offs;			/* position of term i from the first, NULL: adjacent */
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
	int			ndeferred;		/* deferred leaves (EvalLeaf.deferred) */
	EvalLeaf   *leaves;
	EvalNode   *root;
	EvalNode   *sure;			/* unranked plans with a phrase: the matches the bitmaps decide alone, NULL when none */
	SiftQuery **queries;
	int			nqueries;
	bool		recheck;		/* ranked scans: a phrase that is not required, every candidate is rechecked in the heap */
	bool		hardrecheck;	/* unranked scans: a negated phrase, every candidate is rechecked in the heap */
	PhraseCheck *phr;			/* phrases decided by positions where the segment has them; the heap decides the other candidates */
	int			nphr;
	int			phrcap;
	int		   *pleaf;			/* the distinct leaves of the required phrases */
	int			npleaf;
	bool	   *phok;			/* [nsegs * nphr] the segment holds the positions of all terms of the phrase */
	bool	   *segheap;		/* [nsegs] some phrase is not decided by the segment's positions */
	bool		heap;			/* some candidates need the heap recheck */
	bool		complement;		/* a branch matches documents that no leaf hits (NOT x alone, a NOT under an OR): the ranker walks the matches, not the bounds */
	bool		rescore;		/* a phrase the score depends on: the exact score comes from the text */
	bool		capped;			/* estimate: stop expanding at the limit */
	bool		no_reads;		/* SIFT_BIND_NO_READS */
	bool		scoring;
	const SiftFilter *filter;	/* ranked search: the tuples worth scoring */
	const SiftTidBridge *bridge;	/* ranked search: the tuples a structured scan kept, as page masks per superblock (a finalized bridge) */
	const SiftAttrTest *attrs;	/* ranked search: tests of INCLUDE columns the docs of a block pass before they are scored */
	int			nattrs;
	SiftBm25	bm;
	/*
	 * A leaf of a field is scored with the length of the field in the doc and the average of that length over the docs that keep it (the live docs of the
	 * segments of format SIFT_SEGFMT_FIELDDL and of the write buffers): the sums here, and bmf the parameters made of them. The segments of older formats
	 * do not keep it: their docs score the whole text's length.
	 */
	int			nfields;
	uint32		fmask;			/* the fields (bit f - 1) a scoring leaf searches */
	bool		fscored;		/* some doc has the lengths of its fields */
	uint64		seg_fdocs;
	uint64		seg_fsum[SIFT_MAX_FIELDS];
	uint64		wbuf_fdocs;
	uint64		wbuf_fsum[SIFT_MAX_FIELDS];
	SiftBm25	bmf[SIFT_MAX_FIELDS];
	SiftPlanStats stats;
};

typedef struct SharedHit
{
	SiftKey		key;
	float4		score;
} SharedHit;

/* What follows the header: the directory snapshot (dir_len bytes), then nmhit SharedHit, then for every leaf a count and that many WbufTermEnt */
typedef struct SharedBind
{
	Size		dir_len;
	uint64		seg_ndocs;
	uint64		seg_sum_dl;
	uint64		wbuf_ndocs;
	uint64		wbuf_sum_dl;
	uint64		seg_fdocs;
	uint64		seg_fsum[SIFT_MAX_FIELDS];
	uint64		wbuf_fdocs;
	uint64		wbuf_fsum[SIFT_MAX_FIELDS];
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
	int64		flt_pos;		/* the filter's superblock at or after next_sb */
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
	const SiftGroupBounds *bb;		/* bounds in the unit's SB, NULL when the term has no posting there (valid once located) */
	bool		located;			/* bb is known for the unit's SB */
	bool		sought;				/* the term's group of the unit's SB was looked up: g is it, or NULL for none */
	bool		live;				/* may lift a document over theta somewhere in the unit */
	bool		act;				/* scores in the current block */
	uint32		pce;				/* RankSeg.epoch the cursors are positioned in */
	uint32		sce;
	const sift_grp *g;				/* NULL: no postings in this SB */
	uint8	   *tf;
	uint32		tfcap;
	bool		based;				/* all_tf1 and base are known for the group of the unit */
	bool		tf_whole;			/* tf holds the tf of every posting of the group */
	int			nblk_tf;			/* blocks of the group whose tf was decoded on their own */
	bool		all_tf1;			/* tf is 1 in the whole group: no tf stream, the scorers take NULL */
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
	const sift_grp **ug;			/* [kid of an OR root] the group each kid has in the superblock being ranked, NULL for none */
	int			nug;				/* the kids of the root when it is an OR, else 0 */
	int		   *kidleaf;			/* [kid of an OR root] the scoring leaf the kid is the cursor of, -1 for none */
	bool	   *kidskip;			/* [kid of an OR root] left out of the candidates of the block being scored */
	bool	   *leaf_kid;			/* [leaf] some kid of the root is its cursor */
	bool		inverted;			/* the bridge has far fewer superblocks of the segment than the leaves: its superblocks are the units, no leaf list is merged */
	int		   *ordpos;				/* [leaf] scratch of kids_skip: its place in the leaves by bound, -1 for none */
	struct UnitStream *stream;		/* the walk over the segment's units a streamed search makes (rank_stream.c) */
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
	float4	   *fknorm[SIFT_MAX_FIELDS];	/* the same from the length of a field, for the fields the plan searches (rank_field_knorm) */
	const float4 *lknorm[SIFT_MAX_FIELDS];	/* what the leaves of a field are scored with in the block: fknorm, or knorm for a segment without field lengths */
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
	BlockMask	cand pg_attribute_aligned(SIFT_SIMD_ALIGN);
	uint8		blk_tf[BLK_SLOTS + SIFT_KERN_PAD];	/* tf of the postings of one block of one term, decoded for the scorer that reads it next */
	uint16		zero[SIFT_KERN_SLOTS];
	const uint16 *dl[SIFT_BLK_PAGES];
	uint8		attab[SIFT_ATTR_TABLE + 1 + SIFT_KERN_PAD];	/* the codes of a block that pass an IN test */
	const SiftAttrTest *attab_test;	/* the test, and the base and code count of the block, that attab was built for (a superblock has one of each) */
	int64		attab_base;
	uint64		attab_unk;
	int			zone_checks;	/* superblocks tested against their INCLUDE ranges, and the ones that dropped (sift_seg_attr_excludes) */
	int			zone_drops;
} RankWork;

struct SiftRanker
{
	SiftPlan   *plan;
	SiftVerifyFn verify;
	void	   *arg;
	MemoryContext tmpcx;
	RankSeg    *seg;
	Unit	   *unit;				/* the units, when a search that orders or shares them has asked for them */
	int			nunit;
	bool		units_built;
	float4		floor;				/* the K-th score of the search a widened one repeats: the repeat finds hits above it only (-infinity: none) */
	bool		deferred;			/* the plan's wide leaves read their top terms only: a search whose K-th score does not settle them widens the plan and goes again */
	int			mode;				/* SiftRankMode */
	bool		by_maxub;			/* unit is sorted by descending bound (else by segment, ascending gid) */
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
	SiftHitSpill *spill;			/* an unbounded search that outgrew work_mem keeps its hits here, sorted, instead of in hit (nhit counts them) */
	bool		spill_ok;			/* the search may spill: no limit, not shared with other participants, and the host has a sorter */
	bool		spill_ready;		/* spill_head is the hit at pos */
	SiftHit		spill_head;
	bool		searched;
	int64		fb_read0;			/* ... and what it had read and kept when the last search ended */
	int64		fb_kept0;
	bool		dropping;			/* the consumer drops hits: the next search asks for what its rows take (sift_rank_feedback) */
	int64		fb_read;
	int64		fb_kept;
	int64		fb_want;
	bool		complete;
	bool		ties;				/* the search keeps every hit that ties with its worst one; nothing is asked after them (sift_rank_ties) */
	float4	   *group_floor;		/* the first search starts above this k-th score, which the scans of the other partitions of the LIMIT raised (sift_rank_group_floor) */
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

	return sift_term_cmp(x->term, x->len, y->term, y->len);
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

/* eval_fields.c: the averages of the lengths of the fields, and the norms a block's leaves of a field are scored with */
extern SIFT_INTERNAL void bind_field_totals(SiftPlan *p, const void *shared);
extern SIFT_INTERNAL void bind_field_params(SiftPlan *p, float4 k1, float4 b);
extern SIFT_INTERNAL void rank_field_knorm(SiftPlan *p, SiftSegment *seg, const SiftDocChunk *dc, int block, uint32 want, RankWork *wk);

/* wbind.c */
extern SIFT_INTERNAL void bind_wbuf(SiftPlan *p);
extern SIFT_INTERNAL void import_wbuf(SiftPlan *p, const SharedBind *h);
extern SIFT_INTERNAL void bind_scoring(SiftPlan *p);

/* The terms a deferred leaf keeps (at most WIDE_TOP_MAX): the ones with the largest bounds, then the largest df, then the smallest bytes */
typedef struct TopTerm
{
	char	   *term;
	int			len;
	uint8		mb;				/* the largest bound over the segments */
	double		ldf;
	SiftTermInfo *info;			/* [segment] the term's entry, ldf 0 where it has none */
} TopTerm;

typedef struct TopTerms
{
	int			e;
	int			n;
	TopTerm	   *t;
} TopTerms;

/* The most terms a wide leaf reads one by one: the ones its weighing keeps, and the ones a search widens it to before it reads them as one union */
#define WIDE_TOP_MAX	64

/* bm25/eval/eval.c: the terms of leaf l in segment s with a bound of at least min_bound; true when there are more than limit (the leaf keeps the first limit). left_out: the largest bound of the others */
extern SIFT_INTERNAL bool expand_tail(SiftPlan *p, EvalLeaf *l, int s, int limit, int min_bound, uint8 *left_out);
/* bm25/tail.c: wide leaves: the name of the leaf, its one union term in every segment, and its deferral */
extern SIFT_INTERNAL void tail_wide_names(SiftPlan *p, EvalLeaf *l);
extern SIFT_INTERNAL void tail_widen_leaf(SiftPlan *p, EvalLeaf *l);
extern SIFT_INTERNAL void tail_defer_leaf(SiftPlan *p, EvalLeaf *l);
extern SIFT_INTERNAL int tail_wbuf_terms(EvalLeaf *l, LeafTerm **out);
extern SIFT_INTERNAL double tail_wide_blended_df(SiftPlan *p, EvalLeaf *l, TopTerms *tt);
extern SIFT_INTERNAL double tail_weigh_top(SiftPlan *p, EvalLeaf *l, TopTerms *tt);
extern SIFT_INTERNAL void tail_top_install(SiftPlan *p, EvalLeaf *l, TopTerms *tt, float4 w);
extern SIFT_INTERNAL void score_wbuf(SiftPlan *p);
extern SIFT_INTERNAL const LeafTerm *leaf_weighed(const EvalLeaf *l, const char *term, int len);

/* phrase.c */
extern SIFT_INTERNAL PosCtx *posctx_create(SiftPlan *p);
extern SIFT_INTERNAL bool phrases_hold(PosCtx *c, int s, int page, int off);
extern SIFT_INTERNAL sift_cur *phrase_cursor(SiftPlan *p, int s, int ph, sift_cur *kid, const IterCurs *ic, MemoryContext cx);
extern SIFT_INTERNAL bool doc_pass(SiftPlan *p, text *doc, float4 *score);

/* iter.c */
extern SIFT_INTERNAL sift_cur *seg_cursor(SiftPlan *p, int s, const EvalNode *n, SegCurs *cs, const IterCurs *ic, MemoryContext cx);
extern SIFT_INTERNAL bool iter_find_sb(SiftMatchIter *it, uint32 *sbp);
extern SIFT_INTERNAL const SiftFilterSB *iter_filter_sb(const SiftMatchIter *it);
extern SIFT_INTERNAL bool filter_page_mask(const SiftFilter *f, const SiftFilterSB *fs, int page, uint64 *mask);

#endif							/* SIFT_EVAL_INT_H */
