/*
 * col/shmstatus.c - sift.shared_memory(): the tables of the directory of the shared memory (col/dsmreg.c) and why one is not shared.
 *
 * Why is kept in a table of the directory itself, so that one backend says it in the log for the whole postmaster (sift_shm_first) and the function can tell it to every
 * session. Where even that table cannot be made (the shortage is that of the whole of /dev/shm) every backend says its own reason, as before.
 */
#include "postgres.h"

#include <ctype.h>

#include "funcapi.h"
#include "storage/spin.h"
#include "utils/builtins.h"

#include "agg/agg_int.h"
#include "col/dictshm.h"
#include "col/dsmreg.h"
#include "col/leafshm.h"

PG_FUNCTION_INFO_V1(sift_shared_memory);

#define NOTES_NAME		"sift_shm_notes_1"
#define NOTES_MAX		24
#define NOTE_REASON_LEN	200

typedef struct ShmNote
{
	char		name[SIFT_DSM_NAME_LEN];
	char		reason[NOTE_REASON_LEN];
} ShmNote;

typedef struct ShmNotes
{
	slock_t		mutex;
	int			n;
	ShmNote		note[NOTES_MAX];
} ShmNotes;

static void
notes_init(SIFT_DSM_INIT_ARGS)
{
	ShmNotes   *s = ptr;

	memset(s, 0, sizeof(*s));
	SpinLockInit(&s->mutex);
}

bool
sift_shm_first(const char *name, const char *reason)
{
	ShmNotes   *s = sift_dsm_try(NOTES_NAME, sizeof(ShmNotes), notes_init);
	bool		first = true;
	int			i;

	if (s == NULL)
		return true;
	SpinLockAcquire(&s->mutex);
	for (i = 0; i < s->n && strcmp(s->note[i].name, name) != 0; i++)
		;
	if (i < s->n)
		first = strcmp(s->note[i].reason, reason) != 0;
	if (i < NOTES_MAX)
	{
		strlcpy(s->note[i].name, name, sizeof(s->note[i].name));
		strlcpy(s->note[i].reason, reason, sizeof(s->note[i].reason));
		s->n = Max(s->n, i + 1);
	}
	SpinLockRelease(&s->mutex);
	return first;
}

/* The tables of the directory carry the number of their layout in their name (sift_col_dict_5, sift_col_wbuf_arena_4_seg3): the rows do not */
static void
display_name(const char *table, char *out, Size len)
{
	const char *p;

	for (p = table; *p; p++)
		if (*p == '_' && isdigit((unsigned char) p[1]))
		{
			const char *q = p + 1;

			while (isdigit((unsigned char) *q))
				q++;
			if (*q == '\0' || strncmp(q, "_seg", 4) == 0)
				break;
		}
	if (*p == '\0')
		strlcpy(out, table, len);
	else
	{
		const char *q = p + 1;

		while (isdigit((unsigned char) *q))
			q++;
		snprintf(out, len, "%.*s%s", (int) (p - table), table, q);
	}
}

typedef struct ShmKnown
{
	const char *name;			/* as displayed */
	int			(*kb) (void);	/* the setting that sizes it, in kB; NULL: none, or not one number */
	bool		(*stat) (SiftShmStat *st);
} ShmKnown;

static int
dict_kb(void)
{
	return sift_col_dict_shared_cache_kb;
}

static int
leaf_kb(void)
{
	return sift_col_leaf_cache_kb;
}

static int
verdict_kb(void)
{
	return sift_verdict_cache_kb;
}

static int
dimmap_kb(void)
{
	return sift_agg_dimmap_kb;
}

static const ShmKnown known[] = {
	{"sift_col_dict_ring", dict_kb, sift_dictshm_stat},
	{"sift_col_leaf_stats", leaf_kb, sift_col_leafshm_stat},
	{"sift_agg_verdicts", verdict_kb, sift_verdict_shm_stat},
	{"sift_agg_dimmap", dimmap_kb, NULL},
};

static const ShmKnown *
find_known(const char *name)
{
	for (int i = 0; i < lengthof(known); i++)
		if (strcmp(known[i].name, name) == 0)
			return &known[i];
	return NULL;
}

/* One row for each table of the directory, then one for each table that a backend went without and that the directory does not hold (the reason is its note) */
Datum
sift_shared_memory(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	ShmNotes   *notes;
	ShmNote		copy[NOTES_MAX];
	int			ncopy = 0;
	bool		listed[NOTES_MAX] = {false};
	char		table[SIFT_DSM_NAME_LEN];
	uint64		size;

	InitMaterializedSRF(fcinfo, 0);
	notes = sift_dsm_find(NOTES_NAME, NULL);
	if (notes != NULL)
	{
		SpinLockAcquire(&notes->mutex);
		ncopy = notes->n;
		memcpy(copy, notes->note, ncopy * sizeof(ShmNote));
		SpinLockRelease(&notes->mutex);
	}
	for (int i = 0; sift_dsm_list(i, table, &size); i++)
	{
		char		name[SIFT_DSM_NAME_LEN];
		const ShmKnown *k;
		SiftShmStat st = {0};
		Datum		v[8] = {0};
		bool		nulls[8] = {false};
		const char *reason = NULL;

		if (table[0] == '\0' || strcmp(table, NOTES_NAME) == 0)
			continue;
		display_name(table, name, sizeof(name));
		k = find_known(name);
		for (int j = 0; j < ncopy; j++)
			if (strcmp(copy[j].name, table) == 0)
			{
				reason = copy[j].reason;
				listed[j] = true;
			}
		if (k != NULL && k->stat != NULL)
			k->stat(&st);
		v[0] = CStringGetTextDatum(name);
		nulls[1] = k == NULL || k->kb == NULL;
		if (!nulls[1])
			v[1] = Int64GetDatum((int64) k->kb() * 1024);
		v[2] = Int64GetDatum((int64) size);
		nulls[3] = !st.has_used;
		v[3] = Int64GetDatum(st.used);
		nulls[4] = nulls[5] = !st.has_counts;
		v[4] = Int64GetDatum(st.hits);
		v[5] = Int64GetDatum(st.misses);
		v[6] = BoolGetDatum(true);
		nulls[7] = reason == NULL;
		if (reason != NULL)
			v[7] = CStringGetTextDatum(reason);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	for (int j = 0; j < ncopy; j++)
	{
		char		name[SIFT_DSM_NAME_LEN];
		const ShmKnown *k;
		Datum		v[8] = {0};
		bool		nulls[8] = {true, true, true, true, true, true, false, false};

		if (listed[j])
			continue;
		display_name(copy[j].name, name, sizeof(name));
		k = find_known(name);
		v[0] = CStringGetTextDatum(name);
		nulls[0] = false;
		if (k != NULL && k->kb != NULL)
		{
			v[1] = Int64GetDatum((int64) k->kb() * 1024);
			nulls[1] = false;
		}
		v[6] = BoolGetDatum(false);
		v[7] = CStringGetTextDatum(copy[j].reason);
		tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	return (Datum) 0;
}
