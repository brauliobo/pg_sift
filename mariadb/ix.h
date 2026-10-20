/*
 * ix.h - the state of an index, shared by engine.c (lifecycle, configuration), persist.c (manifest, event log, slots), docs.c
 * (documents), query.c (live segment and queries), feed.c (drain, rebuild) and merge.c (tiered merge, background thread).
 *
 * Locks. `rw` guards what queries read: the manifest, the segment slots of `sr`, the document tables (ids/pks, dead,
 * latest), the cached live segment. A query holds it shared from taking its view to its last read; a writer takes it
 * exclusively only for the instant it swaps in what it prepared. `wmu` serializes the writers' publish steps (manifest
 * and log writes, slot reservation, generation numbers); the slow work (analysis, writing and merging segments) holds
 * neither. `mergemu` keeps one merge (or drop, or rebuild) at a time per index. Order: mergemu, wmu, rw.
 */
#ifndef SIFT_MDB_IX_H
#define SIFT_MDB_IX_H

#include "core/host.h"

#include <errno.h>
#include <pthread.h>

#include "core/page.h"
#include "bm25/segment.h"
#include "engine.h"
#include "store.h"

/* The last error of the engine on this thread: ereport(ERROR) unwinds to the SIFT_TRY of the call and leaves the message here */
extern __thread char sift_err_msg[1024];

/* Run body; returns 0, or -1 with sift_err_msg set when the engine raised an error */
#define SIFT_TRY(body) \
	({ int rc_ = 0; \
	   SQ_TRY(); body; SQ_CATCH(); snprintf(sift_err_msg, sizeof(sift_err_msg), "%s", sq_errmsg()); rc_ = -1; SQ_END(); \
	   rc_; })

#define MAN_MAGIC		"SIFTMDB2"
#define MDB_FORMAT		2
#define MAX_SEGS		120		/* of the manifest: the rest of SIFT_MAX_SEGMENTS is for the live segment */
#define NMETA			6
#define PAGE_LAYOUT_ID		((uint32) BLCKSZ << 8)

typedef struct Ev
{
	int64		docid;
	int64		pk;
	int64		op;				/* 'I' or 'D' */
} Ev;

typedef struct ManSeg
{
	SiftSegEntry e;
	uint32		slot;
	uint32		pad;
	char		file[48];
} ManSeg;

/* MANIFEST: this header, nsegs ManSeg, cfg_len bytes of configuration */
typedef struct Manifest
{
	char		magic[8];
	uint32		format;			/* MDB_FORMAT */
	uint32		sift_format;	/* SIFT_FORMAT_MAJOR << 16 | SIFT_FORMAT_MINOR of the pages */
	uint32		page_layout;	/* page size and layout version */
	uint32		dpp;
	uint64		version;
	uint64		next_gen;
	uint64		log_len;
	int64		drained;		/* queue rows up to this seq are in the segments */
	int64		docbase;		/* docid of the row with seq s is s + docbase */
	uint32		nsegs;
	uint32		logno;
	uint32		cfg_len;
	uint32		pad;
} Manifest;

typedef struct Slot				/* open addressing pk -> docid; 0 = no live document */
{
	int64		key;
	int64		val;
	uint8		used;
} Slot;

/* The live segment: what the queue holds that the segments do not, as an in-memory segment of its own */
typedef struct Delta
{
	int			refs;
	uint64		epoch;
	int64		after;			/* the watermark it was built over */
	uint64		sig;			/* of the queue rows it was built from */
	size_t		nev;
	int			g;				/* mapped slot of the reserved range, -1 without documents */
	uint32		blocks;
	SiftSegEntry e;
	int64	   *dead;			/* live docids of the segments that a queue row retires, ascending */
	size_t		ndead;
	int64	   *dids;			/* its documents, ascending docids */
	int64	   *dpks;
	char	  **text;
	size_t	   *tlen;
	size_t		nd;
} Delta;

struct SiftIdx
{
	char		name[64];
	char		dir[512];
	pthread_rwlock_t rw;
	pthread_mutex_t wmu;
	pthread_mutex_t mergemu;
	pthread_mutex_t dmu;		/* the cached live segment */
	Relation	rel;
	SiftRel    *sr;
	Manifest	man;
	ManSeg		segs[SIFT_MAX_SEGMENTS];
	char	   *cfg;
	const char *meta[NMETA];	/* queue, keyexpr, source, textexpr, ... (values of cfg) */
	bool		fresh;			/* queries read the queue */
	bool		recheck;		/* queries check the rows still exist */
	int			fanin;
	int64	   *ids,			/* docid -> key of every document in the segments, ascending docids */
			   *pks;
	size_t		ndoc,
				capdoc;
	int64	   *dead;			/* ascending */
	size_t		ndead,
				capdead;
	Slot	   *latest;
	size_t		caplatest,
				nlatest;
	int64		bytes;
	uint64		epoch;			/* bumped when the documents are replaced (rebuild) */
	int64		merges;
	bool		slot_reserved[SIFT_NSLOTS];
	Delta	   *delta;			/* the cached live segment (dmu) */
	bool		feeding;
	bool		dropped;
	bool		want_merge;
	char		merge_err[256];	/* the last error of a background merge */
};

/* engine.c, docs.c, persist.c */
extern void thread_init(void);
extern void fail(const char *fmt,...) pg_attribute_printf(1, 2) pg_attribute_noreturn();
extern int	finish_call(int rc, char *err, size_t errlen);
extern MemoryContext op_begin(void);
extern void op_end(MemoryContext cx);
extern void path_of(const SiftIdx *ix, char *out, size_t n, const char *file);
extern void sync_dir(const char *dir);
extern void write_all(int fd, const void *p, size_t n, const char *what);
extern size_t lower_bound(const int64 *a, size_t n, int64 v);
extern bool in_sorted(const int64 *a, size_t n, int64 v);
extern bool is_dead(const SiftIdx *ix, int64 docid);
extern int64 latest_get(const SiftIdx *ix, int64 pk);
extern int64 pk_of(const SiftIdx *ix, int64 docid);
extern void apply_event(SiftIdx *ix, int64 docid, int64 pk, int64 op);
extern void finish_events(SiftIdx *ix);
extern void reset_docs(SiftIdx *ix);
extern void write_manifest(SiftIdx *ix, const Manifest *m, const ManSeg *segs);
extern void append_events(SiftIdx *ix, const Ev *ev, size_t n);
extern void write_compact_log(SiftIdx *ix, uint32 logno, const int64 *ids, const int64 *pks, size_t n, uint64 *len);
extern void log_path(const SiftIdx *ix, uint32 logno, char *out, size_t n);
extern int	slot_reserve(SiftIdx *ix);
extern void slot_release(SiftIdx *ix, int slot);
extern uint64 gen_take(SiftIdx *ix);
extern void rename_segment(SiftIdx *ix, SiftWriteTarget *wt, uint64 gen, ManSeg *ms, int *g, uint32 *blocks);
extern void delta_drop(SiftIdx *ix);
extern void delta_release(Delta *d);
extern SiftIdx *registered(const char *name);
extern void registry_each(void (*fn) (SiftIdx *ix, void *arg), void *arg);
extern void rethrow(void) pg_attribute_noreturn();
extern void init_rel(SiftIdx *ix);
extern void do_open(SiftIdx *ix);

/* merge.c */
extern void merge_kick(SiftIdx *ix);
extern void merge_stop(void);
extern void merge_sets(SiftIdx *ix, bool full);

/* Runs body with the writers' mutex held and passes its error on */
#define LOCKED(ix, body) \
	do { \
		int lrc_; \
		pthread_mutex_lock(&(ix)->wmu); \
		lrc_ = SIFT_TRY(body); \
		pthread_mutex_unlock(&(ix)->wmu); \
		if (lrc_) \
			rethrow(); \
	} while (0)

#define RD_LOCK(ix)		pthread_rwlock_rdlock(&(ix)->rw)
#define RD_UNLOCK(ix)	pthread_rwlock_unlock(&(ix)->rw)
#define WR_LOCK(ix)		pthread_rwlock_wrlock(&(ix)->rw)
#define WR_UNLOCK(ix)	pthread_rwlock_unlock(&(ix)->rw)

#endif							/* SIFT_MDB_IX_H */
