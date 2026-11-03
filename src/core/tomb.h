/*
 * core/tomb.h - the tombstone log of a relation: an append-only segment of 32-byte entries (the key of a row version, the transaction and command
 * that ended it) with the sorted copy a backend keeps, the lookups a reader makes and the rewrite VACUUM does.
 *
 * A sift_table keeps the log of its deleted, updated and locked row versions (docs/COL-TAM.md section 2.4); a heap sift_col index with track_updates
 * keeps the log of the entries that stopped being live. Both read it through the same cache and the same visibility rule.
 */
#ifndef SIFT_CORE_TOMB_H
#define SIFT_CORE_TOMB_H

#include "core/host.h"

#include "utils/snapshot.h"

#include "core/lane.h"
#include "core/store.h"

#define SIFT_TOMB_MAGIC			0x544F4D42	/* "TOMB" */

#define SIFT_TOMB_LOCK_BLKNO	4	/* LockPage target: appenders share, the tombstone rewrite of VACUUM excludes them. Not SIFT_WORKER_LOCK_BLKNO: a worker holds that one for a whole pass, and every delete would wait for it */
StaticAssertDecl(SIFT_TOMB_LOCK_BLKNO != SIFT_WORKER_LOCK_BLKNO && SIFT_TOMB_LOCK_BLKNO != SIFT_ACTIVE_LOCK_BLKNO &&
				 SIFT_TOMB_LOCK_BLKNO != SIFT_MAINT_LOCK_BLKNO && SIFT_TOMB_LOCK_BLKNO != SIFT_DIRECTORY_BLKNO,
				 "the lock targets of a relation are distinct");

/*
 * What an index log says of its own completeness (a heap sift_col index with track_deletes; zero for a table, whose log is complete by construction): flags, the
 * transaction from which the log is complete, and the identity (the xmin of the catalog row) of the two statement triggers that feed it.
 */
#define SIFT_TOMB_COMPLETE		0x0001

typedef struct SiftTombChan
{
	uint32		flags;
	TransactionId base_xid;
	uint32		witness[2];
} SiftTombChan;

/* Tombstone segment header page: SiftWbufHeaderData then this; data pages: SiftTombPage then SiftTomb[] */
typedef struct SiftTombHdr
{
	uint32		magic;
	uint32		pad;
	uint64		nentries;		/* appended so far: the version of the log */
	SiftTombChan chan;
} SiftTombHdr;

typedef struct SiftTombPage
{
	BlockNumber next;
	uint16		nent;
	uint16		flags;
} SiftTombPage;

#define SIFT_TOMB_HDR(page) \
	((SiftTombHdr *) (SiftPageGetData(page) + MAXALIGN(sizeof(SiftWbufHeaderData))))
#define SIFT_TOMB_PAGE(page)	((SiftTombPage *) SiftPageGetData(page))
#define SIFT_TOMB_PAGE_INIT		MAXALIGN(sizeof(SiftTombPage))
#define SIFT_TOMB_ENTS(page)	((SiftTomb *) (SiftPageGetData(page) + SIFT_TOMB_PAGE_INIT))

typedef enum SiftTombKind
{
	SIFT_TOMB_DELETE = 1,		/* the version (an index: the entry) is not live from the entry's transaction on */
	SIFT_TOMB_UPDATE,			/* succ is the new version */
	SIFT_TOMB_LOCK				/* row lock: never deletes */
} SiftTombKind;

/* UPDATE entries: the new version is a member of the chain of this one, no index has an entry for it */
#define SIFT_TOMB_HOT		0x0001

typedef struct SiftTomb
{
	uint64		key;			/* SiftKey of the row version */
	TransactionId xid;
	CommandId	cid;
	uint64		succ;			/* UPDATE: key of the new version */
	uint16		kind;
	uint16		flags;
	uint32		seq;			/* position in the log (not stored: set when loaded) */
} SiftTomb;

#define SIFT_TOMB_CAP	((SIFT_PAGE_PAYLOAD - SIFT_TOMB_PAGE_INIT) / sizeof(SiftTomb))

StaticAssertDecl(sizeof(SiftTomb) == 32, "tombstone entries are 32 bytes");

/* What a backend keeps of a relation's log; dir is the directory copy the owner of the relation keeps (the log reads and refreshes it) */
typedef struct SiftTombLog
{
	MemoryContext cxt;
	SiftDirSnapshot *dir;
	/* tombstones, sorted by (key, seq) */
	uint64		gen;
	BlockNumber hdr;
	uint64		synced;
	BlockNumber last;
	uint32		lastcnt;
	SiftTombChan chan;			/* the completeness record of the header at the last sync */
	uint32		seqbase;		/* the entries of the logs read before the current one, which only follows them (tomb_follow) */
	bool		followed;
	SiftTomb   *ents;			/* sorted by (key, seq): the bulk of the log */
	uint32		nents;
	uint32		capents;
	SiftTomb   *recent;			/* sorted likewise: entries synced since the last merge into ents (small, cheap to insert into) */
	uint32		nrecent;
	uint32		caprecent;
	struct HTAB *hotm;			/* the keys that are the succ of a HOT entry: chain members (NULL until the log has one) */
	SiftTomb   *run;			/* scratch: the entries of one key found in both arrays */
	uint32		caprun;
} SiftTombLog;

/* The log of a relation that has none yet: cxt holds what it caches, dir is the owner's directory copy */
extern void sift_tomb_init(SiftTombLog *t, MemoryContext cxt, SiftDirSnapshot *dir);
/* The directory entry of the log segment in a directory copy (NULL when the relation has none) */
extern const SiftSegEntry *sift_tomb_find_seg(const SiftDirSnapshot *dir);
/* A new empty log segment of the kind and segment format the relation keeps it as (a table: WBUF_OPEN, an index: SIFT_SEG_TOMB): its directory entry, to be added by the caller's directory change */
extern SiftSegEntry sift_tomb_new_segment(Relation rel, SiftSegKind kind, uint8 format);
/* Forgets what was read (the cache of the log follows the log of the directory now) */
extern void sift_tomb_forget(SiftTombLog *t);
extern void sift_tomb_sync(Relation rel, SiftTombLog *t);
/* The entries appended to the log so far, from its header page alone: no page of the log is read and no entry sorted */
extern uint64 sift_tomb_entries(Relation rel, const SiftDirSnapshot *dir);
/* Writes the completeness record of the header */
extern void sift_tomb_set_chan(Relation rel, SiftTombLog *t, const SiftTombChan *c);
extern void sift_tomb_append(Relation rel, SiftTombLog *t, uint64 key, TransactionId xid, CommandId cid, SiftTombKind kind, uint16 flags, uint64 succ);
/* The same for n entries: written by the page, in one pass over the segment */
extern void sift_tomb_append_many(Relation rel, SiftTombLog *t, const SiftTomb *ents, int n);
/*
 * Rewrites the tombstone segment without the entries keep() refuses, under the lock that keeps appenders out, and switches the
 * new segment in (the old pages are released with a safexid). Returns the entries kept.
 */
typedef bool (*SiftTombKeep) (SiftTomb *e, void *arg);	/* may rewrite the entry it keeps */
extern uint64 sift_tomb_rewrite(Relation rel, SiftTombLog *t, SiftTombKeep keep, void *arg);

/* The entries of key (sorted by log position), n of them; NULL when none */
extern const SiftTomb *sift_tomb_find(const SiftTombLog *t, uint64 key, int *n);
/* Is there a tombstone entry for a key between lo and hi? */
extern bool sift_tomb_any(const SiftTombLog *t, uint64 lo, uint64 hi);
/* Is the version a member of a chain (the next of a HOT update: no index has an entry for it); *pred: the version the update was of */
extern bool sift_tomb_hot_pred(const SiftTombLog *t, uint64 key, uint64 *pred);
#define sift_tomb_hot_member(t, key)	sift_tomb_hot_pred((t), (key), NULL)
/*
 * The offsets of the rows of block a HOT update entry names, as bits (SIFT_OFFMASK_WORDS words). pos: two indexes (the main and the recent array of the log)
 * of a walk over ascending blocks, zeroed at its start; NULL looks each block up alone
 */
extern void sift_tomb_hot_offsets(const SiftTombLog *t, BlockNumber block, uint32 *pos, uint64 *mask);
/* Does the tombstone e (not a LOCK) delete its row for the snapshot? Dirty snapshots also learn of an in-progress deleter */
extern bool sift_tomb_deletes(Snapshot snapshot, const SiftTomb *e);

/*
 * What became of a transaction, as far as the commit log still says. The log is truncated up to the oldest xid every table is frozen up to, and an index (unlike a heap)
 * may hold an xid past that: a tombstone, the transaction that made the log complete. UNKNOWN is such an xid, one the server could not have given out yet (a
 * wrapped one), and an invalid one: the commit log is not asked, and the caller reads the heap or does without the answer. Frozen and bootstrap xids are COMMITTED.
 */
typedef enum SiftXidState
{
	SIFT_XID_UNKNOWN,
	SIFT_XID_COMMITTED,
	SIFT_XID_OPEN,
	SIFT_XID_ABORTED
} SiftXidState;
extern SiftXidState sift_xid_state(TransactionId xid);

/* Does the tombstone e (not a LOCK) delete its row for an MVCC snapshot: UNKNOWN when its transaction is beyond the commit log (nothing is claimed) */
typedef enum SiftTombVerdict
{
	SIFT_TOMB_KEEPS,
	SIFT_TOMB_DELETES,
	SIFT_TOMB_UNKNOWN
} SiftTombVerdict;
extern SiftTombVerdict sift_tomb_verdict(Snapshot snapshot, const SiftTomb *e);

/* The verdicts of a few transactions for one MVCC snapshot, which a walk over many pages reuses: the tombstones of a page come from few transactions, and the clog is read for each otherwise */
typedef struct SiftTombMemo
{
	TransactionId xid[4];
	uint8		verdict[4];		/* SiftTombVerdict */
	uint8		next;
} SiftTombMemo;
/*
 * Do tombstones delete rows of the blocks [first, first + nblocks) for the snapshot (an MVCC one): sets the bit (block - first) of mask (nblocks / 64 rounded up words, zeroed
 * by the caller) for each such block, and for each block with a tombstone of a transaction beyond the commit log; mask NULL stops at the first. MEMO (NULL: none) belongs to one snapshot. A row of such a block is to be checked in the heap, any other is not deleted for the snapshot
 */
extern bool sift_tomb_deleted_blocks(const SiftTombLog *t, Snapshot snapshot, BlockNumber first, uint32 nblocks, uint64 *mask, SiftTombMemo *memo);
/* Sets the bit of the offset of each row of block that a tombstone deletes for the snapshot (an MVCC one): the page's tombstones are read once, where a lookup bisects the log for each row. false, mask unusable, when the page has more than budget entries or a tombstone of a transaction beyond the commit log */
extern bool sift_tomb_deleted_offsets(const SiftTombLog *t, Snapshot snapshot, BlockNumber block, uint32 budget, uint64 *mask, SiftTombMemo *memo);

#ifdef SIFT_TESTING
/* Test only: transactions older than this (not 0) are taken for older than the commit log keeps */
extern PGDLLIMPORT int sift_test_clog_horizon;
/* Test only: the lookups of a block alone in the log (pos NULL) by this backend: a walk over the pages of a search makes none */
extern PGDLLIMPORT uint64 sift_tomb_hot_probes;
#endif

#endif							/* SIFT_CORE_TOMB_H */
