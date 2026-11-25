/*
 * core/quarantine/int.h - the registry of core/quarantine/quarantine.c as the SQL functions of core/quarantine/sql.c read it.
 */
#ifndef SIFT_CORE_QUARANTINE_INT_H
#define SIFT_CORE_QUARANTINE_INT_H

#include "core/host.h"

#include "storage/spin.h"
#include "utils/timestamp.h"

#include "core/maint/maint.h"

#define QUAR_ENTRIES	256
#define QUAR_LOCAL		8
#define QUAR_REASON_LEN	SIFT_MAINT_ERRMSG_LEN

/* One index file. Counters are what the backend that skipped it has not handed over yet (an entry of the registry holds the totals) */
typedef struct QuarKey
{
	Oid			db;
	Oid			spc;
	RelFileNumber rel;
} QuarKey;

typedef struct QuarEntry
{
	bool		used;
	bool		confirmed;
	bool		persisted;
	bool		unflaggable;	/* the metapage cannot be written (it is damaged): the registry alone holds the index out of service, and no worker is asked for it again */
	QuarKey		file;
	Oid			indexoid;
	TimestampTz detected_at;
	TimestampTz asked_at;
	int			sqlerrcode;
	char		reason[QUAR_REASON_LEN];
	uint32		detections;
	uint64		skipped_writes;
	uint64		skipped_vacuums;
	uint64		refused_scans;
} QuarEntry;

typedef struct QuarShm
{
	slock_t		mutex;
	pg_atomic_uint32 active_count;	/* the entries in force: the test that costs nothing while no index is quarantined */
	pg_atomic_uint64 gen;		/* bumped, under the mutex, whenever the set of files in force changes: a backend's copy of the set is good while it equals this */
	QuarEntry	entry[QUAR_ENTRIES];
} QuarShm;

extern QuarEntry sift_quar_local[QUAR_LOCAL];

extern QuarShm *sift_quar_table(void);
extern int	sift_quar_entries(void);
extern void sift_quar_publish(QuarShm *q);
extern bool sift_quar_is_sift_index(Relation rel);

#endif
