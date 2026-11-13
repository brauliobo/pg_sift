/*
 * col/dsmreg.c - see dsmreg.h.
 *
 * The tables are dynamic shared memory segments that a directory, a segment of the registry of PostgreSQL with a name and a size that never change, keeps by
 * the name asked, the identity of the library that asks (SIFT_CODE_ID, a hash of its sources and of the options that change them: a library that is not
 * byte for byte the same code never shares a table, whatever the size of its struct) and the size. The first backend that asks makes the table (nothing
 * is allocated at postmaster start); the others map it.
 *
 * The registry of PostgreSQL could not do this: it keeps the name and size of its first request until the postmaster restarts, raises an error for the
 * name at another size, and shares the entry between libraries whose struct has the same size and another layout. A table here is pinned while it is the one
 * of its library and given back, by the backend that makes another table of the name, when no backend maps it: the directory counts the backends that map a
 * table (an entry of a library that was replaced under the server, or of a size that a setting left, lasts as long as the backends of that library, and
 * no longer). Every change of the library or of a size is a table of its own; none is an error.
 *
 * A cache must not turn into an error: whatever fails in sift_dsm_attach (no room in /dev/shm, no more segments) is caught in a subtransaction, which also
 * gives back the locks, and the backend goes without the table.
 */
#include "postgres.h"

#include "access/xact.h"
#include "port/atomics.h"
#include "storage/dsm.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "utils/memutils.h"
#include "utils/resowner.h"

#include "col/am.h"
#include "col/dsmreg.h"
#include "core/code_id.h"

/* The directory. Its name, its size and the layout of everything below are fixed forever (a new layout is a new name): it is the one segment that two libraries of any age ask at the same size */
#define DIR_NAME		"pg_sift_dsm_dir"
#define DIR_MAGIC		0x53494654
#define DIR_ENTRIES		1024

typedef struct DirEnt
{
	char		name[40];
	char		build[16];
	uint64		size;
	dsm_handle	handle;			/* DSM_HANDLE_INVALID: the entry is free */
	pg_atomic_uint32 users;		/* backends that map the table */
} DirEnt;

typedef struct Dir
{
	uint32		magic;
	uint32		pad;
	DirEnt		ent[DIR_ENTRIES];
} Dir;

StaticAssertDecl(sizeof(DirEnt) == 72 && sizeof(Dir) == 8 + 72 * DIR_ENTRIES, "the directory of the shared tables never changes its layout");

static void
dir_init(SIFT_DSM_INIT_ARGS)
{
	Dir		   *d = ptr;

	memset(d, 0, sizeof(Dir));
	d->magic = DIR_MAGIC;
	for (int i = 0; i < DIR_ENTRIES; i++)
		pg_atomic_init_u32(&d->ent[i].users, 0);
}

static const char *why = "";
static const char *why_hint = NULL;

const char *
sift_dsm_why(void)
{
	return why;
}

static Dir *
dir_segment(void)
{
	static Dir *dir;
	bool		found;

	if (dir == NULL)
#if PG_VERSION_NUM >= 190000
		dir = GetNamedDSMSegment(DIR_NAME, sizeof(Dir), dir_init, &found, NULL);
#else
		dir = GetNamedDSMSegment(DIR_NAME, sizeof(Dir), dir_init, &found);
#endif
	return dir;
}

/* The directory, or NULL when the segment of its name is not a directory of this layout (why says it) */
static Dir *
dir_get(void)
{
	Dir		   *d = dir_segment();

	if (d->magic == DIR_MAGIC)
		return d;
	why = "the directory of the shared tables is not one this library can read";
	why_hint = NULL;
	return NULL;
}

static const char *
build_id(void)
{
#ifdef SIFT_TESTING
	if (sift_dsm_test_build[0])
		return sift_dsm_test_build;
#endif
	return SIFT_CODE_ID;
}

static DirEnt *
dir_find(Dir *d, const char *name, const char *build, Size size)
{
	for (int i = 0; i < DIR_ENTRIES; i++)
	{
		DirEnt	   *e = &d->ent[i];

		if (e->handle != DSM_HANDLE_INVALID && e->size == size && strcmp(e->name, name) == 0 && strcmp(e->build, build) == 0)
			return e;
	}
	return NULL;
}

/* A free entry, after the tables of another library, and the ones of the name at another size, that no backend maps are given back; else an unmapped table is; NULL when all are mapped */
static DirEnt *
dir_claim(Dir *d, const char *name, const char *build)
{
	DirEnt	   *free = NULL;
	DirEnt	   *idle = NULL;

	for (int i = 0; i < DIR_ENTRIES; i++)
	{
		DirEnt	   *e = &d->ent[i];

		if (e->handle != DSM_HANDLE_INVALID && pg_atomic_read_u32(&e->users) == 0 &&
			(strcmp(e->build, build) != 0 || strcmp(e->name, name) == 0))
		{
			dsm_unpin_segment(e->handle);
			e->handle = DSM_HANDLE_INVALID;
		}
		if (free == NULL && e->handle == DSM_HANDLE_INVALID)
			free = e;
		if (idle == NULL && e->handle != DSM_HANDLE_INVALID && pg_atomic_read_u32(&e->users) == 0)
			idle = e;
	}
	if (free == NULL && idle != NULL)
	{
		dsm_unpin_segment(idle->handle);
		idle->handle = DSM_HANDLE_INVALID;
		free = idle;
	}
	return free;
}

static void
release_users(dsm_segment *seg, Datum users)
{
	pg_atomic_fetch_sub_u32((pg_atomic_uint32 *) DatumGetPointer(users), 1);
}

/* The segments of a backend are detached last to first, so the directory is still mapped when the count of a table goes down */
static void
hold(dsm_segment *seg, DirEnt *e)
{
	dsm_pin_mapping(seg);
	on_dsm_detach(seg, release_users, PointerGetDatum(&e->users));
	pg_atomic_fetch_add_u32(&e->users, 1);
}

void *
sift_dsm_try(const char *name, Size size, SiftDsmInit init)
{
	Dir		   *d = dir_get();
	const char *build = build_id();
	MemoryContext cx;
	dsm_segment *seg;
	DirEnt	   *e;

	if (d == NULL)
		return NULL;
	SIFT_INVARIANT(strlen(name) < sizeof(e->name) && strlen(build) < sizeof(e->build), "sift: the name or the build of the shared table %s is too long", name);
	cx = MemoryContextSwitchTo(TopMemoryContext);
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	if ((e = dir_find(d, name, build, size)) != NULL)
	{
		/* a mapping is a table this backend holds already */
		if ((seg = dsm_find_mapping(e->handle)) == NULL)
		{
			seg = dsm_attach(e->handle);
			SIFT_INVARIANT(seg != NULL, "sift: the pinned shared table %s is gone", name);
			hold(seg, e);
		}
	}
	else
	{
		if ((e = dir_claim(d, name, build)) == NULL)
		{
			why = "the directory of the shared tables is full";
			why_hint = "Every table is mapped by a backend; the tables are given back when their sessions end.";
			LWLockRelease(AddinShmemInitLock);
			MemoryContextSwitchTo(cx);
			return NULL;
		}
		seg = dsm_create(size, 0);
#if PG_VERSION_NUM >= 190000
		init(dsm_segment_address(seg), NULL);
#else
		init(dsm_segment_address(seg));
#endif
		dsm_pin_segment(seg);
		strlcpy(e->name, name, sizeof(e->name));
		strlcpy(e->build, build, sizeof(e->build));
		e->size = size;
		pg_atomic_write_u32(&e->users, 0);
		e->handle = dsm_segment_handle(seg);
		hold(seg, e);
	}
	LWLockRelease(AddinShmemInitLock);
	MemoryContextSwitchTo(cx);
	return dsm_segment_address(seg);
}

void *
sift_dsm_join(const char *name, Size size, SiftDsmInit init)
{
	void	   *p = sift_dsm_try(name, size, init);

	if (p == NULL)
		ereport(ERROR, (errcode(ERRCODE_CONFIGURATION_LIMIT_EXCEEDED), errmsg("sift: %s", why), why_hint ? errhint("%s", why_hint) : 0));
	return p;
}

#ifdef SIFT_TESTING
char		sift_dsm_test_build[16];
SiftDsmDeferred sift_dsm_deferred[SIFT_DSM_DEFERRED_MAX];

bool
sift_dsm_entry(int i, const char **name, const char **build, uint64 *size, dsm_handle *handle, uint32 *users)
{
	DirEnt	   *e;

	if (i >= DIR_ENTRIES)
		return false;
	e = &dir_segment()->ent[i];
	*name = e->name;
	*build = e->build;
	*size = e->size;
	*handle = e->handle;
	*users = pg_atomic_read_u32(&e->users);
	return true;
}

/* sift._test_dsm_magic(magic): the magic of the directory, set to magic (a directory another library made); the one it had */
uint32
sift_dsm_test_magic(uint32 magic)
{
	Dir		   *d = dir_segment();
	uint32		was = d->magic;

	d->magic = magic;
	return was;
}

/* The tables this backend went without because a statement was writing (sift._test_dsm_deferred): the test that a write statement reached an attach point */
static void
deferred_note(const char *name)
{
	for (int i = 0; i < SIFT_DSM_DEFERRED_MAX; i++)
		if (sift_dsm_deferred[i].n == 0 || strcmp(sift_dsm_deferred[i].name, name) == 0)
		{
			strlcpy(sift_dsm_deferred[i].name, name, sizeof(sift_dsm_deferred[i].name));
			sift_dsm_deferred[i].n++;
			return;
		}
}
#endif

void *
sift_dsm_attach(const char *name, Size size, SiftDsmInit init, bool *failed, const char *what, const char *guc, bool test_fail)
{
	MemoryContext cx = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	void	   *volatile s = NULL;

	/* a subtransaction cannot start where there is no transaction or a parallel operation, and must not under a write (sift_col_writing); the next statement of the backend tries again */
	if (!IsTransactionState() || IsInParallelMode())
		return NULL;
	if (sift_col_writing())
	{
#ifdef SIFT_TESTING
		deferred_note(name);
#endif
		return NULL;
	}
	BeginInternalSubTransaction(NULL);
	MemoryContextSwitchTo(cx);
	PG_TRY();
	{
		if (test_fail)
			ereport(ERROR, (errcode(ERRCODE_INSUFFICIENT_RESOURCES), errmsg("sift: test: the table of %s fails", what)));
		s = sift_dsm_join(name, size, init);
		ReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cx);
		CurrentResourceOwner = owner;
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(cx);
		e = CopyErrorData();
		FlushErrorState();
		RollbackAndReleaseCurrentSubTransaction();
		MemoryContextSwitchTo(cx);
		CurrentResourceOwner = owner;
		*failed = true;
		s = NULL;
		ereport(LOG,
				(errmsg("sift: %s are not shared with other backends: %s", what, e->message),
				 e->hint ? errhint("%s", e->hint)
				 : guc ? errhint("Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm), or set %s to 0.", guc)
				 : errhint("Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm).")));
		FreeErrorData(e);
	}
	PG_END_TRY();
	return s;
}
