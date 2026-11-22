/*
 * col/scope_v6/int.h - what the files of col/scope_v6/ share: the ring and month-map accumulators of a build, and the final writer of the object.
 * Private to col/scope_v6/. Portable.
 */
#ifndef SIFT_COL_SCOPE_V6_INT_H
#define SIFT_COL_SCOPE_V6_INT_H

#include "col/scope_v6/meta.h"

/* The records of a dim or a rollup: nk keys, the row count, then one SiftScopeGroupM for each measure (nk 1 is a SiftScopeGroup) */
static inline Size
sift_rec_size(const SiftScopeShape *s, uint32 nk)
{
	return (Size) (nk + 1) * sizeof(int64) + (Size) s->nmeas * sizeof(SiftScopeGroupM);
}

static inline char *
sift_dim_rec(const SiftScopeShape *s, const SiftScopeDim *d, uint32 i)
{
	return d->groups + (Size) i * sift_rec_size(s, d->nk);
}

static inline int64 *
sift_rec_key(char *rec)
{
	return (int64 *) rec;
}

static inline uint64 *
sift_rec_cnt(char *rec, uint32 nk)
{
	return (uint64 *) (rec + (Size) nk * sizeof(int64));
}

static inline SiftScopeGroupM *
sift_rec_m(char *rec, uint32 nk)
{
	return (SiftScopeGroupM *) (rec + (Size) (nk + 1) * sizeof(int64));
}

/* The dsets are merged in lazily: the ids of a declared set, ascending, distinct */
extern const uint32 *sift_scube_dset_ids(const SiftScopeCubeAcc *a, uint32 d, uint32 *n);
extern void sift_scube_dset_append(SiftScopeCubeAcc *a, uint32 d, const uint32 *ids, uint32 n);
/* The cube's groups need the same shape as the cube it is merged or compared with */
extern bool sift_scope_shape_equal(const SiftScopeShape *a, const SiftScopeShape *b);
/* The dim or rollup a group record belongs to: where its bytes begin, how many it holds at most */
extern uint32 sift_dim_limit(const SiftScopeDim *d);
extern void sift_dim_size(SiftScopeCubeAcc *a, SiftScopeDim *d, uint32 ngroups);

/* cube_dim.c: the group tables of a cube; every reader in key order calls sift_sdim_sort first */
typedef void (*SiftGroupFn) (const SiftScopeCubeAcc *a, char *drec, const char *srec, uint32 nk);
typedef uint32 (*SiftGroupSubFn) (const SiftScopeCubeAcc *a, char *drec, const char *srec, uint32 nk);
extern uint32 sift_sdim_search(const SiftScopeShape *s, const SiftScopeDim *d, const int64 *key, bool *found);
extern char *sift_sdim_group_for(const SiftScopeShape *s, SiftScopeDim *d, const int64 *key);
extern void sift_sdim_sort(const SiftScopeShape *s, SiftScopeDim *d);
extern void sift_sdim_release(SiftScopeDim *d);
extern void sift_sdim_overflow(SiftScopeDim *d);
extern void sift_sdim_merge(const SiftScopeCubeAcc *a, SiftScopeDim *dd, SiftScopeDim *sd, SiftGroupFn fn);
extern uint32 sift_sdim_sub(const SiftScopeCubeAcc *a, SiftScopeDim *dd, SiftScopeDim *sd, SiftGroupSubFn fn);
/* An index of the entries of an array that start with an int32 id: slot + 1 in an open-addressed table of power-of-two cap (at most 65535 entries) */
extern void sift_idix_rebuild(uint16 **ht, uint32 *cap, const void *base, Size stride, uint32 n);
extern void sift_idix_note(uint16 *ht, uint32 cap, const void *base, Size stride, uint32 slot);
extern int32 sift_idix_find(const uint16 *ht, uint32 cap, const void *base, Size stride, int32 id);

/* the ring of a key under construction: a min-heap of the newest depth rows (0: SIFT_SCOPE_TOPK_CAP), its array grown 4, 16, depth as the key gets rows */
typedef struct SiftTopKHeap
{
	uint32		n;
	uint32		cap;
	uint32		depth;
	bool		dropped;		/* a row went by that the heap did not keep */
	SiftScopeNewest *e;
} SiftTopKHeap;

typedef struct SiftMonthMapEntry
{
	int32		month_id;
	uint32		n;
	uint32		cap;
	uint32	   *ord;
} SiftMonthMapEntry;

typedef struct SiftMonthMapAcc
{
	uint32		n;
	uint32		cap;
	uint32		last;			/* the entry the last add hit */
	bool		unsorted;		/* e is append-only and indexed by ht until sift_mmap_write */
	uint16	   *ht;
	uint32		ht_cap;
	SiftMonthMapEntry *e;		/* ascending month_id unless unsorted */
} SiftMonthMapAcc;

/* newest-first order: by time, then granule, then row */
extern int sift_newest_cmp(const SiftScopeNewest *a, const SiftScopeNewest *b);

/* Whether a row of time is kept; a row that is not marks the heap as having dropped one */
extern bool sift_topk_wants(SiftTopKHeap *h, int64 time);
extern void sift_topk_offer(SiftTopKHeap *h, int64 time, uint32 gidx, uint32 row_ord);
extern void sift_topk_release(SiftTopKHeap *h);
/* A ring that kept fewer than SIFT_SCOPE_TOPK_CAP rows and dropped one: the read must say it is partial, for a reader takes a full ring only at the cap */
extern bool sift_topk_truncated(const SiftTopKHeap *h);
extern void sift_topk_read(const char *slot, uint32 n, bool wide, SiftScopeNewest *out);
/* The set entries of the ring, compact or wide, newest first, and no more bytes; returns the entries */
extern uint32 sift_topk_write(const SiftTopKHeap *h, bool wide, StringInfo out);
extern uint32 sift_topk_entry_size(bool wide);

/* The rings of an object: the count its header says (1 for a header without a description) */
extern uint32 sift_hdr_rings(const SiftScopeMetaHeader *hdr);

static inline SiftScopeL2ExtraTopK
sift_root_extra(const SiftScopeL2Root *r)
{
	SiftScopeL2ExtraTopK x;

	memcpy(&x, &r->reserved1[0], sizeof(x));
	return x;
}

/* The rings of one key as one block, ring 0 first and each ring its set entries; sets topk_count, the extra counts and the flags of root. partial: bit r for ring r */
extern void sift_rings_write(const SiftTopKHeap *rings, uint32 nrings, uint32 partial, bool wide, StringInfo out, SiftScopeL2Root *root);
/* The entries set in ring of a root */
extern uint32 sift_ring_count(const SiftScopeL2Root *r, uint32 ring);
/* Where ring starts in the block of its key, and how long the block of nrings rings is: the sums of the counts of the rings before it */
extern uint32 sift_ring_start(const SiftScopeL2Root *r, uint32 ring, bool wide);
extern uint32 sift_rings_bytes(const SiftScopeL2Root *r, uint32 nrings, bool wide);

extern void sift_mmap_add(SiftMonthMapAcc *a, int32 month_id, uint32 ordinal);
extern void sift_mmap_free(SiftMonthMapAcc *a);
/* remap: the final ordinal of each arrival ordinal (NULL: the same) */
extern void sift_mmap_write(SiftMonthMapAcc *a, uint32 ngranules, const uint32 *remap, StringInfo out);

/* the bytes of one key, as the final writer takes them: the root, its level-1 nodes with their pool blocks, the rings, the month map, the level-2 block */
typedef struct SiftKeyNodeRec
{
	SiftScopeL1Node node;
	const char *block;			/* ref_off .. the end of the month directory, one run of the source pool */
	uint32		block_len;
	uint32		block_off;		/* node.ref_off in the source pool */
	const SiftScopeMeta *from;	/* the object the block is in, when it came from one (a refresh): its refs are read there */
} SiftKeyNodeRec;

typedef struct SiftKeyRec
{
	SiftScopeL2Root root;
	SiftKeyNodeRec *nodes;
	uint32		nnodes;
	const char *topk;
	bool		topk_wide;		/* the entries at topk are of the wide ring */
	const char *monthmap;
	const char *l2block;		/* the level-2 cubes of the key, one run of the source pool (SIFT_L2_SINGLE_L1: none) */
	uint32		l2block_len;
	uint32		l2block_off;
} SiftKeyRec;

typedef struct SiftEmit
{
	SiftScopeMetaHeader hdr;
	const SiftScopePartMapEntry *parts;
	const char *l0_pool;		/* the level-0 blob pool and the entries, as the builder or the old object hold them */
	uint64		l0_pool_len;
	const SiftScopeL0SliceEntry *l0_entries;
	uint32		l0_nentries;
	const char *dict;			/* SEC_SCOPE_DICT body */
	uint64		dict_len;
	const char *route;			/* SEC_SCOPE_ENT_ROUTE body: none when route_len is 0 */
	uint64		route_len;
} SiftEmit;

/* What a build of a clustered scope keeps of the entity runs: those of the granule in hand, and (clustered by primary key) the homes of every entity so far */
typedef struct SiftHomeSet
{
	MemoryContext mcxt;
	SiftEntHome *runs;
	uint32		nruns;
	SiftEntHome *homes;			/* repeats of the last one left out */
	uint32		nhomes;
	uint32		caphomes;
} SiftHomeSet;

extern void sift_homes_init(SiftHomeSet *s, MemoryContext mcxt);
/* Reads the runs of granule gidx (a granule of one entity and one key is its own run, read from its zones; else the source is called in the context caller) */
extern void sift_homes_runs(SiftHomeSet *s, const SiftScopeGranSource *src, uint32 gidx, const SiftScopeGranHead *h, MemoryContext caller);
/* Notes the homes of the runs in hand for the route */
extern void sift_homes_keep(SiftHomeSet *s);
/*
 * How the entities of the runs in hand fit the key ord of the pair map, the one their rows are stored under (UINT32_MAX: that key is not in the
 * pair map). A mapping edit moves an entity without moving its rows, so the stored key does not say who owns a granule any more:
 * SIFT_HOMES_OTHER: an entity has a key besides ord (the granule is cut for that key); SIFT_HOMES_LACK: an entity does not have ord (the granule
 * is not whole for ord and every key is cut from its rows).
 */
#define SIFT_HOMES_OTHER	1
#define SIFT_HOMES_LACK		2
extern uint32 sift_homes_fit(const SiftHomeSet *s, SiftScopePairMap *pm, uint32 ord);

/* The entity route: the homes of every entity, as the section body; the pairs are sorted in place and repeated pairs count once */
extern void sift_ent_route_write(StringInfo out, SiftEntHome *homes, uint32 n, uint64 max_part_gen);
/* The section is what its header says it is (sizes, fences); false for anything else */
extern bool sift_ent_route_check(const SiftScopeSection *s);
/* The route of an object of earlier parts and the route of the parts added since: the homes of both for every entity, up to max_part_gen */
extern void sift_ent_route_merge(const SiftScopeSection *a, const SiftScopeSection *b, uint64 max_part_gen, StringInfo out);

/* The refs of a level-1 node as bytes: the granule index (delta, zigzag) and the slice number + 1, varints */
extern void sift_put_refs(StringInfo out, const SiftScopeGranRef *refs, uint32 n);

/*
 * A refresh: the keys of plans get the nodes that hold a changed member (or a retired granule) built again, the others are copied.
 * What is known of a key before the rebuild: which of its old nodes go, the granules those nodes held (ring entries on them go), the level-0
 * slices they named (dead now), and the granules the rebuild takes.
 */
typedef struct SiftKeyPlan
{
	int64		key;
	const SiftScopeL2Root *old;	/* NULL: the key is new */
	bool	   *dirty_node;		/* for each node of old: it goes */
	uint32	   *drop;			/* ascending: the granules of the nodes that go */
	uint32		ndrop;
	uint32	   *dead_slices;
	uint32		ndead;
	uint32	   *mask;			/* ascending: the granules of the rebuild (the drop list, and the granules that hold a member that came in) */
	uint32		nmask;
} SiftKeyPlan;

typedef struct SiftRefreshJob
{
	const SiftScopeMeta *old;
	SiftScopeBuildSpec spec;
	SiftScopePairMap *sub;		/* the pairs of the keys of plans that have any */
	SiftKeyPlan *plans;			/* ascending by key */
	uint32		nplans;
	SiftScopePartMapEntry *parts;	/* the part map of the result: retired parts have part_gen 0, new parts follow */
	uint32		nparts;
	uint32		total_granules;
	uint64		max_part_gen;
	uint32		all_lo;			/* the granules every key of plans takes in the rebuild, besides its own mask: [all_lo, all_hi) */
	uint32		all_hi;
	const uint32 *pass;			/* ascending: the granules the rebuild passes over */
	uint32		npass;
	bool		route_rebuild;	/* the route is made again from the granules of the live parts: homes_pass are the ones pass does not name */
	const uint32 *homes_pass;	/* ascending */
	uint32		nhomes_pass;
} SiftRefreshJob;

/* Decodes the nodes that go of a plan: fills drop and dead_slices */
extern void sift_plan_drops(const SiftScopeMeta *old, SiftKeyPlan *p);
extern void sift_refresh_run(const SiftRefreshJob *job, const SiftScopeGranSource *src, StringInfo out, SiftScopeBuildStats *stats);

/* A max-heap over key ordinals 0..nkeys by size[ordinal] (the owner updates size, then calls set); top is UINT32_MAX when it is empty */
typedef struct SiftMaxHeap
{
	uint32		n;
	uint32	   *ord;
	uint32	   *pos;
	const Size *size;
	uint64		cmps;			/* comparisons so far: the cost of the moves, to show they are logarithmic */
} SiftMaxHeap;

extern void sift_mheap_init(SiftMaxHeap *h, uint32 nkeys, const Size *size);
extern void sift_mheap_free(SiftMaxHeap *h);
extern void sift_mheap_set(SiftMaxHeap *h, uint32 k);
extern void sift_mheap_remove(SiftMaxHeap *h, uint32 k);
extern uint32 sift_mheap_top(const SiftMaxHeap *h);

/* a split-block Bloom filter: blocks of 32 bytes, 10 bits a member */
extern uint32 sift_bloom_blocks(uint32 members);
extern void sift_bloom_add(char *bits, uint32 nblocks, int64 member);
extern bool sift_bloom_has(const char *bits, uint32 len, int64 member);

/* Writes a level-2 block into blk (the cubes of the key, then the month directory) and sets the offsets of root; the block is its own run of bytes, offsets from its start */
extern void sift_l2_block(StringInfo blk, SiftScopeL2Root *root, const SiftScopeCubeAcc *all, const SiftScopeMonthTable *months);
extern void sift_emit_object(StringInfo out, SiftEmit *em, SiftKeyRec *recs, uint32 nrecs);
extern uint32 sift_block_end(const SiftScopeL1Node *n);

/* The canonical dictionary of a build: text -> code, in the order of first use */
typedef struct SiftScopeDict
{
	uint32		n;
	uint32		cap;
	char	  **str;
	uint32	   *len;
	uint32	   *slot;			/* open addressing: code + 1, 0 empty */
	uint32		nslot;
	MemoryContext mcxt;
} SiftScopeDict;

extern void sift_sdict_init(SiftScopeDict *d, MemoryContext mcxt);
extern uint32 sift_sdict_intern(SiftScopeDict *d, const char *s, uint32 len);
extern void sift_sdict_write(const SiftScopeDict *d, StringInfo out);
extern bool sift_sdict_seed(SiftScopeDict *d, const char *p, uint64 len);

#endif							/* SIFT_COL_SCOPE_V6_INT_H */
