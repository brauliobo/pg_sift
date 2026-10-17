/*
 * engine.h - the C interface of the sift search engine for MariaDB (no PostgreSQL or MariaDB types).
 *
 * An index is a directory: MANIFEST (the atomic commit point), segment files seg-<gen>.sift (immutable, the page layout of
 * pg_sift) and an event log events-<n>.log. A document is identified by the sequence number of the queue row that
 * carried it plus the index's docbase ("docid"); its row key is an integer (the primary key, or the dense key of the
 * key table for text and composite keys). All calls return 0, or -1 with a message in err, and may be made from any
 * number of threads: queries share an index, a drain or a merge only locks it to publish its result.
 *
 * What the host (the SQL layer) adds to a call: the rows of the queue not yet in the segments ("pending", read at query
 * time, so committed DML is seen at once), the current text of a row (the recheck of a phrase the index cannot decide)
 * and whether a row still exists (so a deleted row never takes a place in a LIMIT). Without a host a call answers from
 * the segments alone.
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

/* A queue row: op 'I' (row inserted or updated), 'D' (deleted); 'B' (barrier) rows never reach the host's callers */
typedef struct SxEvent
{
	int64_t		seq;
	int64_t		pk;
	char		op;
} SxEvent;

/* The current text of a row (malloc'd by the host); text NULL when the row does not exist or has no text */
typedef struct SxRow
{
	int64_t		pk;
	char	   *text;
	size_t		len;
} SxRow;

typedef struct SxHost
{
	void	   *ctx;
	/* queue rows with seq > after, ascending, barriers excluded; *ev is malloc'd */
	int			(*queue) (void *ctx, SiftIdx *ix, int64_t after, SxEvent **ev, size_t *n, char *err, size_t errlen);
	/* the rows of pks, in that order */
	int			(*fetch) (void *ctx, SiftIdx *ix, const int64_t *pks, size_t n, SxRow *rows, char *err, size_t errlen);
	/* which of pks exist in the base table */
	int			(*exists) (void *ctx, SiftIdx *ix, const int64_t *pks, size_t n, uint8_t *present, char *err, size_t errlen);
} SxHost;

/* Keys a result may be restricted to: ascending, no duplicates */
typedef struct SxIdSet
{
	const int64_t *ids;
	size_t		n;
} SxIdSet;

typedef struct SiftIdxStats
{
	int			nsegs;
	int64_t		ndocs;			/* documents in the segments, dead ones included */
	int64_t		ndead;			/* superseded or deleted documents not yet merged away */
	int64_t		nlive;
	int64_t		drained;		/* highest queue sequence number applied (the watermark) */
	int64_t		docbase;
	int64_t		bytes;			/* segment files */
	int64_t		dpp;
	int64_t		merges;			/* merges done since the index was opened */
	uint32_t	format;
} SiftIdxStats;

/* Base directory of the indexes (default: the current directory) and of the temporary files */
extern void sx_set_dirs(const char *base, const char *tmp);

/* The version string of this build: engine format, sift page format, MariaDB the UDF was built for */
extern const char *sx_version(void);

/*
 * Creates the directory base/name from cfg, lines of key=value: the options k1, b, stemmer, case_folding, accent_folding,
 * positions, prefix_index, fields, analyzer (text|keyword), dpp, merge_fanin, and what the host needs to reach the rows
 * (queue, keyexpr, source, textexpr, fresh, recheck), kept verbatim in the manifest. An index of the same name is an error.
 */
extern int	sx_create(const char *name, const char *cfg, char *err, size_t errlen);
extern int	sx_exists(const char *name);
extern int	sx_drop(const char *name, char *err, size_t errlen);

/* Opens an index once per process; later calls return the same handle */
extern SiftIdx *sx_open(const char *name, char *err, size_t errlen);

/* A value of the index's configuration (NULL when absent), valid for the life of the process */
extern const char *sx_cfg(SiftIdx *ix, const char *key);

/* Ranked top-k of a SiftQL query; *n is set to the hits found (at most k); flt restricts the keys that may be returned */
extern int	sx_topk(SiftIdx *ix, const SxHost *h, const char *q, int qlen, int k, const SxIdSet *flt, SxHit *hits, int *n,
					char *err, size_t errlen);
extern int	sx_count(SiftIdx *ix, const SxHost *h, const char *q, int qlen, const SxIdSet *flt, int64_t *count, char *err,
					 size_t errlen);

/* All matches: key of each into *pks (malloc'd, caller frees) */
extern int	sx_matches(SiftIdx *ix, const SxHost *h, const char *q, int qlen, const SxIdSet *flt, int64_t **pks, int64_t *n,
					   char *err, size_t errlen);

/*
 * Feeds a drain: queue rows in ascending seq, op 'I' (row inserted or updated: text, or NULL), 'D' (deleted) or 'B' (a
 * barrier: every row below its seq is committed, it moves the watermark). Rows at or below the watermark are skipped (a
 * drain interrupted after the commit is repeated). The index is unchanged until sx_feed_end commits it atomically.
 * A load (rebuild) feeds op 'R' rows (pk, text) of the whole table, whatever their seq: sx_feed_end then replaces the
 * documents of the index by them, and the watermark becomes the one given to sx_feed_begin.
 */
extern SiftFeed *sx_feed_begin(SiftIdx *ix, int64_t watermark, char *err, size_t errlen);
extern int	sx_feed_add(SiftFeed *f, int64_t seq, char op, int64_t pk, const char *text, size_t len, char *err, size_t errlen);
extern int	sx_feed_end(SiftFeed *f, int64_t *rows, char *err, size_t errlen);
extern void sx_feed_abort(SiftFeed *f);

/* Merges the segments the tier policy picks (full: all of them, dropping every dead document); compacts the event log */
extern int	sx_merge(SiftIdx *ix, int full, char *err, size_t errlen);

extern int	sx_stats(SiftIdx *ix, SiftIdxStats *st, char *err, size_t errlen);

/* Analyzer of the index: tokens of text, space separated into out (malloc'd) */
extern int	sx_tokens(SiftIdx *ix, const char *text, size_t len, char **out, char *err, size_t errlen);

#endif							/* SIFT_MDB_ENGINE_H */
