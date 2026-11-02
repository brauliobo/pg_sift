/*
 * core/tomb.h - the tombstone log of a relation: an append-only segment of 32-byte entries (the key of a row version, the transaction and command
 * that ended it) with the sorted copy a backend keeps, the lookups a reader makes and the rewrite VACUUM does.
 *
 * A sift_table keeps the log of its deleted, updated and locked row versions (docs/COL-TAM.md section 2.4); a heap sift_col index with row_key
 * keeps the log of the entries that stopped being live (docs/COL-HEAPFREE.md). Both read it through the same cache and the same visibility rule.
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

/* Tombstone segment header page: SiftWbufHeaderData then this; data pages: SiftTombPage then SiftTomb[] */
typedef struct SiftTombHdr
{
	uint32		magic;
	uint32		pad;
	uint64		nentries;		/* appended so far: the version of the log */
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
extern void sift_tomb_append(Relation rel, SiftTombLog *t, uint64 key, TransactionId xid, CommandId cid, SiftTombKind kind, uint16 flags, uint64 succ);
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
/* Sets the bit of the offset of each row of block that a tombstone deletes for the snapshot (an MVCC one): the page's tombstones are read once, where a lookup bisects the log for each row. false, mask unusable, when the page has more than budget entries */
extern bool sift_tomb_deleted_offsets(const SiftTombLog *t, Snapshot snapshot, BlockNumber block, uint32 budget, uint64 *mask);

#ifdef SIFT_TESTING
/* Test only: the lookups of a block alone in the log (pos NULL) by this backend: a walk over the pages of a search makes none */
extern PGDLLIMPORT uint64 sift_tomb_hot_probes;
#endif

#endif							/* SIFT_CORE_TOMB_H */
