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
 * A cache must not turn into an error. The shortage of what a table is made of (no room in /dev/shm, no more segments, a limit of files or memory: SQLSTATE class 53, from the
 * creation of the segment) is caught where it is raised, in sift_dsm_try: no table, and the reason in sift_dsm_why; the callers go without it, in a parallel worker, in a
 * background worker and under a statement that writes, none of which can start the subtransaction of sift_dsm_attach, which catches whatever else fails and also gives back the locks.
 * The directory itself is a segment of the registry of PostgreSQL, made once by the first backend that asks: the room for it is made sure of first (dir_segment), so that a shortage there is "no
 * table" as well.
 */
#include "postgres.h"

#include "access/xact.h"
#include "miscadmin.h"
#include "port/atomics.h"
#include "storage/dsm.h"
#include "storage/dsm_registry.h"
#include "storage/lwlock.h"
#include "storage/shmem.h"
#include "utils/memutils.h"
#include "utils/resowner.h"
#include "utils/varlena.h"

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
static char why_text[256];

#ifdef SIFT_TESTING
/* sift.test_dsm_fail: parts of table names (comma separated) whose segment cannot be made, as when /dev/shm is full; "bgworker:" before a part: for the background workers only */
char	   *sift_test_dsm_fail = NULL;

static bool
test_fails(const char *name)
{
	List	   *parts = NIL;
	bool		fails = false;

	if (sift_test_dsm_fail == NULL || sift_test_dsm_fail[0] == '\0' || !SplitIdentifierString(pstrdup(sift_test_dsm_fail), ',', &parts))
		return false;
	foreach_ptr(char, part, parts)
	{
		bool		worker = strncmp(part, "bgworker:", 9) == 0;

		fails |= (!worker || MyBackendType == B_BG_WORKER) && strstr(name, part + (worker ? 9 : 0)) != NULL;
	}
	return fails;
}
#endif

const char *
sift_dsm_why(void)
{
	return why;
}

/*
 * The segment of a table, NULL where the system cannot make it (why says it). The attempt has a resource owner of its own: the descriptor that a failed creation leaves is released here, and
 * a transaction that commits does not report it as a leak. Any error that is not a shortage goes on.
 */
static dsm_segment *
create_segment(const char *name, Size size)
{
	MemoryContext cx = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner,
				attempt = ResourceOwnerCreate(owner, "sift shared table");
	dsm_segment *seg = NULL;
	uint32		holdoff = InterruptHoldoffCount;

	CurrentResourceOwner = attempt;
	PG_TRY();
	{
#ifdef SIFT_TESTING
		if (test_fails(name))
			ereport(ERROR, (errcode(ERRCODE_DISK_FULL), errmsg("could not resize shared memory segment \"/PostgreSQL.0\" to %zu bytes: No space left on device (test)", size)));
#endif
		seg = dsm_create(size, 0);
		dsm_pin_mapping(seg);
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(cx);
		e = CopyErrorData();
		if (ERRCODE_TO_CATEGORY(e->sqlerrcode) != ERRCODE_INSUFFICIENT_RESOURCES)
		{
			FreeErrorData(e);
			PG_RE_THROW();
		}
		FlushErrorState();
		InterruptHoldoffCount = holdoff;	/* errfinish zeroed it, and a caller may hold an lwlock (sift_dsm_try holds AddinShmemInitLock) */
		ResourceOwnerRelease(attempt, RESOURCE_RELEASE_BEFORE_LOCKS, false, false);
		strlcpy(why_text, e->message, sizeof(why_text));
		why = why_text;
		why_hint = "Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm).";
		FreeErrorData(e);
		seg = NULL;
	}
	PG_END_TRY();
	CurrentResourceOwner = owner;
	ResourceOwnerDelete(attempt);
	return seg;
}

/* Set once the registry holds the directory, in the shared memory of the server: the registry is not asked, a restart of the server (which makes both again) clears it */
static pg_atomic_uint32 *
dir_made(void)
{
	static pg_atomic_uint32 *made;
	bool		found;

	if (made == NULL)
		made = ShmemInitStruct("pg_sift_dsm_dir_made", sizeof(pg_atomic_uint32), &found);
	return made;
}

/*
 * The directory is made by the registry of PostgreSQL, which raises a shortage as an ordinary error with its locks held, and keeps an entry for the name that the failed creation never filled (every
 * later ask of the name is an error too, until the server restarts). So until the directory is made the room is made sure of first, with a segment of the directory's size that is given back at once
 * and a shortage caught as that of any table (create_segment): NULL, and why says it. Once it is made the registry only maps it, and no backend pays a segment for it.
 */
static Dir *
dir_segment(void)
{
	static Dir *dir;
	pg_atomic_uint32 *made = dir_made();
	dsm_segment *probe;
	bool		found;

	if (dir != NULL)
		return dir;
	if (pg_atomic_read_u32(made) == 0)
	{
		if ((probe = create_segment(DIR_NAME, sizeof(Dir))) == NULL)
			return NULL;
		dsm_detach(probe);
	}
#if PG_VERSION_NUM >= 190000
	dir = GetNamedDSMSegment(DIR_NAME, sizeof(Dir), dir_init, &found, NULL);
#else
	dir = GetNamedDSMSegment(DIR_NAME, sizeof(Dir), dir_init, &found);
#endif
	pg_atomic_write_u32(made, 1);
	return dir;
}

/* The directory, or NULL when the segment of its name is not a directory of this layout (why says it) */
static Dir *
dir_get(void)
{
	Dir		   *d = dir_segment();

	if (d == NULL)
		return NULL;
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

/* Gives back the tables that no backend maps and that this request makes stale: the ones of another library, and the ones of the name at another size (sift.shared_cache_kb changed) */
static void
dir_reap(Dir *d, const char *name, const char *build, Size size)
{
	for (int i = 0; i < DIR_ENTRIES; i++)
	{
		DirEnt	   *e = &d->ent[i];

		if (e->handle != DSM_HANDLE_INVALID && pg_atomic_read_u32(&e->users) == 0 &&
			(strcmp(e->build, build) != 0 || (strcmp(e->name, name) == 0 && e->size != size)))
		{
			dsm_unpin_segment(e->handle);
			e->handle = DSM_HANDLE_INVALID;
		}
	}
}

/* A free entry, else an unmapped table is given back; NULL when all are mapped */
static DirEnt *
dir_claim(Dir *d)
{
	DirEnt	   *free = NULL;
	DirEnt	   *idle = NULL;

	for (int i = 0; i < DIR_ENTRIES; i++)
	{
		DirEnt	   *e = &d->ent[i];

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

static DirEnt *last_held;		/* the table this backend mapped last (sift._test_leak('shared')) */

/* The segments of a backend are detached last to first, so the directory is still mapped when the count of a table goes down */
static void
hold(dsm_segment *seg, DirEnt *e)
{
	last_held = e;
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
	dir_reap(d, name, build, size);
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
		if ((e = dir_claim(d)) == NULL)
		{
			why = "the directory of the shared tables is full";
			why_hint = "Every table is mapped by a backend; the tables are given back when their sessions end.";
			LWLockRelease(AddinShmemInitLock);
			MemoryContextSwitchTo(cx);
			return NULL;
		}
		if ((seg = create_segment(name, size)) == NULL)
		{
			LWLockRelease(AddinShmemInitLock);
			MemoryContextSwitchTo(cx);
			return NULL;
		}
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

/* This backend lets go of a table it maps; the backend that lets go last gives the segment back to the system (the directory entry is free, nothing maps it) */
void
sift_dsm_unmap(const char *name, Size size)
{
	Dir		   *d = dir_get();
	DirEnt	   *e;
	dsm_segment *seg;

	if (d == NULL)
		return;
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	if ((e = dir_find(d, name, build_id(), size)) != NULL && (seg = dsm_find_mapping(e->handle)) != NULL)
	{
		dsm_detach(seg);
		if (pg_atomic_read_u32(&e->users) == 0)
		{
			dsm_unpin_segment(e->handle);
			e->handle = DSM_HANDLE_INVALID;
		}
	}
	LWLockRelease(AddinShmemInitLock);
}

/* A table of this library that exists, mapped in this backend and held like the ones it asks for; NULL where there is none (nothing is made) */
void *
sift_dsm_find(const char *name, Size *size)
{
	Dir		   *d = dir_get();
	const char *build = build_id();
	DirEnt	   *e = NULL;
	dsm_segment *seg = NULL;
	MemoryContext cx;

	if (d == NULL)
		return NULL;
	cx = MemoryContextSwitchTo(TopMemoryContext);
	LWLockAcquire(AddinShmemInitLock, LW_EXCLUSIVE);
	for (int i = 0; i < DIR_ENTRIES && e == NULL; i++)
		if (d->ent[i].handle != DSM_HANDLE_INVALID && strcmp(d->ent[i].name, name) == 0 && strcmp(d->ent[i].build, build) == 0)
			e = &d->ent[i];
	if (e != NULL && (seg = dsm_find_mapping(e->handle)) == NULL && (seg = dsm_attach(e->handle)) != NULL)
		hold(seg, e);
	if (seg != NULL && size != NULL)
		*size = e->size;
	LWLockRelease(AddinShmemInitLock);
	MemoryContextSwitchTo(cx);
	return seg ? dsm_segment_address(seg) : NULL;
}

/* The name and size of entry i of the tables of this library (false past the last, an empty name for an entry that is free or another library's) */
bool
sift_dsm_list(int i, char name[SIFT_DSM_NAME_LEN], uint64 *size)
{
	Dir		   *d = dir_get();
	const DirEnt *e;

	if (d == NULL || i >= DIR_ENTRIES)
		return false;
	e = &d->ent[i];
	LWLockAcquire(AddinShmemInitLock, LW_SHARED);
	if (e->handle != DSM_HANDLE_INVALID && strcmp(e->build, build_id()) == 0)
	{
		strlcpy(name, e->name, SIFT_DSM_NAME_LEN);
		*size = e->size;
	}
	else
		name[0] = '\0';
	LWLockRelease(AddinShmemInitLock);
	return true;
}

#ifdef SIFT_TESTING
char		sift_dsm_test_build[16];
SiftDsmDeferred sift_dsm_deferred[SIFT_DSM_DEFERRED_MAX];

bool
sift_dsm_entry(int i, const char **name, const char **build, uint64 *size, dsm_handle *handle, uint32 *users)
{
	DirEnt	   *e;

	Dir		   *d = dir_segment();

	if (d == NULL || i >= DIR_ENTRIES)
		return false;
	e = &d->ent[i];
	*name = e->name;
	*build = e->build;
	*size = e->size;
	*handle = e->handle;
	*users = pg_atomic_read_u32(&e->users);
	return true;
}

/* sift._test_leak('shared'): the table this backend mapped last counts one more backend that maps it, and none will give it back */
void
sift_dsm_test_leak_user(void)
{
	if (last_held == NULL)
		ereport(ERROR, (errcode(ERRCODE_OBJECT_NOT_IN_PREREQUISITE_STATE), errmsg("no shared table to leak a user of"), errhint("Call sift._test_dsm_join first.")));
	pg_atomic_fetch_add_u32(&last_held->users, 1);
}

/* sift._test_dsm_magic(magic): the magic of the directory, set to magic (a directory another library made); the one it had */
uint32
sift_dsm_test_magic(uint32 magic)
{
	Dir		   *d = dir_segment();
	uint32		was;

	SIFT_INVARIANT(d != NULL, "sift: the directory of the shared tables cannot be made: %s", why);
	was = d->magic;
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

/* The backend goes without the table for good; the first backend of the postmaster that has this reason for the table says why in the log (sift.shared_memory() has it for the rest) */
static void
shortage(bool *failed, const char *name, const char *what, const char *guc, const char *message, const char *hint)
{
	char		reason[200];

	*failed = true;
	strlcpy(reason, message, sizeof(reason));
	if (!sift_shm_first(name, reason))
		return;
	ereport(LOG,
			(errmsg("sift: %s are not shared with other backends: %s", what, reason),
			 hint ? errhint("%s", hint)
			 : guc ? errhint("Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm), or set %s to 0.", guc)
			 : errhint("Make room for dynamic shared memory (dynamic_shared_memory_type, the size of /dev/shm).")));
}

void *
sift_dsm_attach(const char *name, Size size, SiftDsmInit init, bool *failed, const char *what, const char *guc, bool test_fail)
{
	MemoryContext cx = CurrentMemoryContext;
	ResourceOwner owner = CurrentResourceOwner;
	void	   *volatile s = NULL;

	/* a subtransaction cannot start where there is no transaction or a parallel operation: sift_dsm_try catches the shortage itself, and the backend goes without the table for good, as after a failure below */
	if (!IsTransactionState() || IsInParallelMode())
	{
		if ((s = sift_dsm_try(name, size, init)) == NULL)
			shortage(failed, name, what, guc, why, why_hint);
		return s;
	}
	/* and one must not start under a write (sift_col_writing): its commit advances the command id under the statement; the next statement of the backend tries again */
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
		s = NULL;
		shortage(failed, name, what, guc, e->message, e->hint);
		FreeErrorData(e);
	}
	PG_END_TRY();
	return s;
}
