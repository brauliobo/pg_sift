/*
 * sqstore.h - what the PostgreSQL storage layer looks like to the portable sift sources: a Relation is the
 * page file of one virtual table (8 KiB blocks, grouped in chunk rows of the shadow table, cached in a bounded
 * cache), a Buffer is a pointer to a page of a cached chunk. Pins keep a chunk in the cache; locks are no-ops.
 */
#ifndef SQSTORE_H
#define SQSTORE_H

typedef char *Page;
typedef Page Buffer;
typedef struct SqStore SqStore;

typedef struct RelFileLocator
{
	Oid			spcOid;
	Oid			dbOid;
	Oid			relNumber;			/* unique per store and per rollback epoch: names the segments of the wide prefix cache */
} RelFileLocator;

typedef struct RelationData
{
	SqStore    *store;
	char		relname[96];
	RelFileLocator rd_locator;
} RelationData;

typedef RelationData *Relation;
#define RelationGetRelationName(r)	((r)->relname)

typedef struct PageHeaderData
{
	uint64		pd_lsn;
	uint16		pd_checksum;
	uint16		pd_flags;
	uint16		pd_lower;
	uint16		pd_upper;
	uint16		pd_special;
	uint16		pd_pagesize_version;
	uint32		pd_prune_xid;
} PageHeaderData;

typedef PageHeaderData *PageHeader;
#define SizeOfPageHeaderData		sizeof(PageHeaderData)
#define PageGetSpecialPointer(p)	((char *) (p) + ((PageHeader) (p))->pd_special)
#define PageGetPageSize(p)			BLCKSZ

#define InvalidBuffer				((Buffer) NULL)
#define BUFFER_LOCK_UNLOCK			0
#define BUFFER_LOCK_SHARE			1
#define BUFFER_LOCK_EXCLUSIVE		2
#define BufferIsValid(b)			((b) != NULL)
#define BufferGetPage(b)			((Page) (b))
#define LockBuffer(b, mode)			((void) 0)
#define ReleaseBuffer(b)			sq_unpin(b)
#define UnlockReleaseBuffer(b)		sq_unpin(b)

/* A pinned buffer stays in the cache (and its address valid) until it is released; the engine releases what it reads */
extern Buffer ReadBuffer(Relation rel, BlockNumber blkno);
extern void sq_unpin(Buffer buf);
extern BlockNumber BufferGetBlockNumber(Buffer buf);

/* generic WAL: the page is edited in place, there is no log */
typedef struct GenericXLogState GenericXLogState;
extern GenericXLogState *GenericXLogStart(Relation rel);
extern Page GenericXLogRegisterBuffer(GenericXLogState *state, Buffer buf, int flags);
extern void GenericXLogFinish(GenericXLogState *state);
extern void GenericXLogAbort(GenericXLogState *state);

/* VACUUM callbacks (access/genam.h) */
typedef struct IndexBulkDeleteResult
{
	double		num_pages;
	double		num_index_tuples;
	double		tuples_removed;
	double		pages_newly_deleted;
	double		pages_deleted;
	double		pages_free;
} IndexBulkDeleteResult;

typedef bool (*IndexBulkDeleteCallback) (ItemPointer itemptr, void *state);

/* snapshots, executor slots and the bitmap heap filter: types only */
typedef struct SnapshotData *Snapshot;
typedef struct TupleTableSlot TupleTableSlot;
typedef struct TIDBitmap TIDBitmap;
struct IndexFetchTableData;

/* GUC enum table of src/sift_cpu.c */
struct config_enum_entry
{
	const char *name;
	int			val;
	bool		hidden;
};

#define PGC_USERSET			0
#define DefineCustomEnumVariable(name, short_desc, long_desc, valueAddr, bootValue, options, context, flags, check, assign, show) \
	(*(valueAddr) = (bootValue))

#endif							/* SQSTORE_H */
