/*
 * core/ops.h - what operators need: the privilege check of the maintenance and diagnostic functions,
 * the LOG lines of promotion, merge and the maintenance worker (sift.log_min_duration), the module
 * version and the warning for ALTER INDEX changes that need a REINDEX.
 */
#ifndef SIFT_OPS_H
#define SIFT_OPS_H

#include "datatype/timestamp.h"
#include "utils/rel.h"

/* The extension version this library implements (the control file's default_version) */
#define SIFT_MODULE_VERSION		"6.0"

extern int	sift_log_min_duration;

/* Errors unless the caller may maintain the table of the index: its owner, or MAINTAIN on it (as for VACUUM and REINDEX) */
extern void sift_check_maintain(Relation index);

/* LOG line for an operation that started at start and took at least sift.log_min_duration; docs or bytes < 0 are left out */
extern void sift_log_maint(Relation index, const char *what, TimestampTz start, int64 docs, int64 bytes);

/* Warnings that say the same of an index again and again, one for each index and a minute in a backend, whatever the statements that find it (the count of what they report is kept elsewhere) */
typedef enum SiftWarnKind
{
	SIFT_WARN_QUARANTINED,
	SIFT_WARN_BUFFER_OPEN,
	SIFT_WARN_KINDS
} SiftWarnKind;
extern bool sift_warn_due(Relation index, SiftWarnKind kind);

/* LOG line for an insert that spent that long making room in the directory of the index (it merges when the maintenance worker lags) */
extern void sift_log_stall(Relation index, TimestampTz start);

/* Adds the index of a maintenance worker to the context of the errors it raises */
extern void sift_maint_context(ErrorContextCallback *cb, Oid indexoid);

/* Whether the maintenance lock is taken (sift.index_info, sift.col_index_info) */
extern bool sift_maintenance_busy(Relation index);

/* After ALTER INDEX ... SET / RESET (options) of a sift_col index: errors when the options do not fit its leaves (nothing for other indexes) */
extern void sift_col_alter_check(Relation index);

/* Whether the extension version of the database (the SQL objects) is at least since, e.g. "5.0"; for a feature that stays off until ALTER EXTENSION UPDATE (core/extver.c) */
extern bool sift_extension_at_least(const char *since);

extern void sift_ops_init(void);

#endif							/* SIFT_OPS_H */
