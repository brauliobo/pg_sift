/*
 * col/f2/f2_journal.h - the key journal of a parent index of format major 2 (SIFT_PT_KEYJRNL = 46): the keys whose entry a transaction ended or whose table received a row, which tells a value
 * derived from the parent (the dimension map of a join) whether the parent still is what it was derived from (the rule of col/v1/conn.h, whose verdicts and stamps this keeps).
 *
 * The metapage names a header page (jrnl_blk) that holds the lineage of the journal (the id of the file's journal: a rebuilt index has another), the first transaction id the records are
 * complete from (trim), the identities of the triggers that hear the table (witness) and the blocks of SIFT_KJ_SLOTS record pages. A record names a key (a CHG record: a version of the row of the
 * key was ended by the transaction) or counts rows (an ADD record: the transaction inserted that many into the table). A transaction appends its records to the page its id hashes to, in one
 * logged record of generic WAL, when it is about to commit and before it reads the journal; a transaction that aborts leaves records that no reader counts. A page that is full is emptied and
 * its trim says up to which transaction its records were dropped: a stamp older than that is not vouched for, the reader joins the parent as it did before the journal.
 */
#ifndef SIFT_COL_F2_JOURNAL_H
#define SIFT_COL_F2_JOURNAL_H

#include "postgres.h"

#include "access/htup.h"
#include "utils/rel.h"

#include "col/am/conn_route.h"
#include "core/page.h"

#define SIFT_KJ_MAGIC		0x4B4A524EU		/* "KJRN" */
#define SIFT_KJ_VERSION		1
#define SIFT_KJ_SLOTS		16
#define SIFT_KJ_AGE_MAX		(1U << 30)		/* a record of a transaction this many ids behind is dropped: ids compare modulo 2^32 */

#define SIFT_KJ_CHG			1				/* a version of the row of the key was ended */
#define SIFT_KJ_ADD			2				/* key is the number of rows inserted */

typedef struct SiftKeyJrnlHeader
{
	uint32		magic;
	uint16		version;
	uint16		nslots;
	uint32		crc32c;
	uint32		flags;
	TransactionId trim;			/* the records of the transactions from this id on are complete */
	uint32		witness[2];		/* the identities of the triggers (core/track_hear.h) the journal was made under */
	uint32		pad;
	uint64		lineage;
	BlockNumber slot_blk[SIFT_KJ_SLOTS];
} SiftKeyJrnlHeader;

typedef struct SiftKeyJrnlRec
{
	int64		key;
	TransactionId xid;
	uint32		kind;
} SiftKeyJrnlRec;

typedef struct SiftKeyJrnlPage
{
	uint32		magic;
	uint16		version;
	uint16		nrec;
	uint32		crc32c;
	TransactionId trim;			/* the records of the transactions before it were dropped from this page */
	uint64		lineage;
	SiftKeyJrnlRec rec[FLEXIBLE_ARRAY_MEMBER];
} SiftKeyJrnlPage;

#define SIFT_KJ_CAP		((SIFT_PAGE_PAYLOAD - offsetof(SiftKeyJrnlPage, rec)) / sizeof(SiftKeyJrnlRec))

StaticAssertDecl(offsetof(SiftKeyJrnlPage, crc32c) == 8, "the checksum of a page sits where sift_page_seal puts it");
StaticAssertDecl(offsetof(SiftKeyJrnlHeader, crc32c) == 8, "the checksum of a page sits where sift_page_seal puts it");
StaticAssertDecl(sizeof(SiftKeyJrnlRec) == 16, "a journal record is 16 bytes");

extern const SiftColJournalOps sift_f2_journal_ops;

/* Is the journal of the index kept (the lineage is read once for each relcache entry) */
extern bool sift_f2_journal_on(Relation index);

/*
 * The journal of the index, made when the identities of the triggers it was made under are not the ones now (a new lineage, the records start at the next transaction id): the caller holds the
 * writers of the table out (ShareLock on it). The index must track a key column that is an int2, int4 or int8.
 */
extern uint64 sift_f2_journal_make(Relation heap, Relation index);
/* ... for a connection: made under the lock that holds the writers out when it is not there yet */
extern uint64 sift_f2_journal_enable(Relation heap, Relation index);

/* The writes of the transaction: old is a version of a row of the table that a statement ended, att the column of the table that is the key of the index (sift_f2_journal_key); a row was inserted. Held until sift_f2_journal_flush */
extern AttrNumber sift_f2_journal_key(Relation index);
extern void sift_f2_journal_ended(Oid index, AttrNumber att, Relation heap, HeapTuple old);
extern void sift_f2_journal_added(Relation index);
/* Writes what the transaction holds (at its commit and before it reads a journal) */
extern void sift_f2_journal_flush(void);

/* VACUUM and maintenance: drops the records of aborted transactions and those older than SIFT_KJ_AGE_MAX, raising the trim first */
extern void sift_f2_journal_trim(Relation index);

/* sift.check: the header and every record page are what they should be */
extern void sift_f2_journal_check(Relation index);

#endif							/* SIFT_COL_F2_JOURNAL_H */
