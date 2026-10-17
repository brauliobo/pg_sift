/*
 * engine.h - the C interface of the sift search engine for MariaDB (no PostgreSQL or MariaDB types).
 *
 * An index is a directory: MANIFEST (the atomic commit point), segment files seg-<gen>.sift (immutable, the page layout of
 * pg_sift) and an event log events-<n>.log. A document is identified by the sequence number of the queue row that
 * carried it ("docid"); its row key is the integer primary key. All calls return 0, or -1 with a message in err.
 */
#ifndef SIFT_MDB_ENGINE_H
#define SIFT_MDB_ENGINE_H

#include <stddef.h>
#include <stdint.h>

typedef struct SiftIdx SiftIdx;
typedef struct SiftFeed SiftFeed;

typedef struct SxHit
{
	int64_t		pk;
	double		score;
} SxHit;

typedef struct SiftIdxStats
{
	int			nsegs;
	int64_t		ndocs;			/* documents in the segments, dead ones included */
	int64_t		ndead;			/* superseded or deleted documents not yet merged away */
	int64_t		nlive;
	int64_t		drained;		/* highest queue sequence number applied */
	int64_t		bytes;			/* segment files */
	int64_t		dpp;
} SiftIdxStats;

/* Base directory of the indexes (default: the current directory) and of the temporary files */
extern void sx_set_dirs(const char *base, const char *tmp);

/* Creates the directory base/name (dpp documents per page: the granularity of the block bounds) and opens it */
extern int	sx_create(const char *name, int dpp, char *err, size_t errlen);
extern int	sx_drop(const char *name, char *err, size_t errlen);

/* Opens an index once per process; later calls return the same handle */
extern SiftIdx *sx_open(const char *name, char *err, size_t errlen);

/* Ranked top-k of a SiftQL query; *n is set to the hits found (at most k) */
extern int	sx_topk(SiftIdx *ix, const char *q, int qlen, int k, SxHit *hits, int *n, char *err, size_t errlen);
extern int	sx_count(SiftIdx *ix, const char *q, int qlen, int64_t *count, char *err, size_t errlen);

/* All matches: pk of each into *pks (malloc'd, caller frees) */
extern int	sx_matches(SiftIdx *ix, const char *q, int qlen, int64_t **pks, int64_t *n, char *err, size_t errlen);

/*
 * Applies the queue rows of a drain, in ascending seq: op 'I' (a row inserted or updated: text, or NULL), 'D' (deleted).
 * Rows at or below the index's watermark are skipped (a drain interrupted after the commit is repeated). The index is
 * unchanged until sx_feed_end commits it atomically.
 */
extern SiftFeed *sx_feed_begin(SiftIdx *ix, int64_t upto, char *err, size_t errlen);
extern int	sx_feed_add(SiftFeed *f, int64_t seq, char op, int64_t pk, const char *text, size_t len, char *err, size_t errlen);
extern int	sx_feed_end(SiftFeed *f, int64_t *rows, char *err, size_t errlen);
extern void sx_feed_abort(SiftFeed *f);

/* Merges all segments into one, dropping dead documents, and compacts the event log */
extern int	sx_merge(SiftIdx *ix, char *err, size_t errlen);

extern int	sx_stats(SiftIdx *ix, SiftIdxStats *st, char *err, size_t errlen);

/* Analyzer of the index: tokens of text, space separated into out (malloc'd) */
extern int	sx_tokens(SiftIdx *ix, const char *text, size_t len, char **out, char *err, size_t errlen);

#endif							/* SIFT_MDB_ENGINE_H */
