/*
 * col/scope_keys.c - the keys of a scope that are not integers (text, varchar, uuid): their codes (docs/COL-SCOPE.md "Key types")
 *
 * The scope machinery identifies a scope key by an int8: the derived leaf the units are cut by, the pairs, the keys of a read. A text or uuid key reaches it as its code in the value store of the
 * key column of the pair relation (col/vstore/vstore.h): a code is the position at which the writer of a scope segment (the build, a promotion, a merge) appended the key, and it never moves,
 * because the store of a scope key is pinned (no epoch begins: sift_scope_key_store_pinned). A writer interns the keys it expands; a reader looks them up and never appends (a standby reads):
 * a key the store has no code for has no stored unit, and the reader gives it a code of its own for the length of the statement (a negative one, which no stored unit has), so that its pairs, its
 * delta and its group are those of any other key. The map of a statement is one object: every code the statement uses comes from it and goes back to a value through it.
 * The equality of the values is that of their bytes, which the btree of the pairs, the store and the planner agree on for a deterministic collation: a column of another is refused.
 */
#include "postgres.h"

#include "access/table.h"
#include "catalog/pg_type_d.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/uuid.h"
#include "utils/varlena.h"

#include "col/am.h"
#include "col/conn.h"
#include "col/scope.h"
#include "col/vstore/vstore.h"

struct SiftKeyMap
{
	Oid			store;
	Oid			typid;
	Oid			collation;
	bool		write;
	SiftCDict  *pending;		/* collecting: the values the store lacks, to intern together */
	SiftCDict  *extra;			/* a reader's: the values the store lacks, as the transient codes -1, -2, ... */
	MemoryContext cx;
	SiftCcPolicy pol;
};

bool
sift_keymap_type(Oid typid)
{
	return typid == TEXTOID || typid == VARCHAROID || typid == UUIDOID;
}

/* A column a scope joins by: an integer, or a text-like or uuid column of a deterministic collation (what sift_keymap_type names); the reason it is not, NULL when it is */
const char *
sift_keymap_column_refusal(Oid typid, Oid collation)
{
	if (sift_conn_integer_type(typid))
		return NULL;
	if (!sift_keymap_type(typid))
		return "is not an integer, text, varchar or uuid column";
	if (typid != UUIDOID && OidIsValid(collation) && !get_collation_isdeterministic(collation))
		return "has a nondeterministic collation: equal values may differ in their bytes";
	return NULL;
}

static void
value_bytes(Oid typid, Datum d, const char **s, uint32 *len)
{
	if (typid == UUIDOID)
	{
		*s = (const char *) DatumGetUUIDP(d)->data;
		*len = UUID_LEN;
	}
	else
	{
		struct varlena *v = pg_detoast_datum_packed((struct varlena *) DatumGetPointer(d));

		*s = VARDATA_ANY(v);
		*len = VARSIZE_ANY_EXHDR(v);
	}
}

/* The store, open for one operation (its view is the backend's: the lock is held to the end of the transaction) */
static SiftVStore *
store_open(const SiftKeyMap *m)
{
	return sift_vstore_open(m->store, m->write ? RowExclusiveLock : AccessShareLock);
}

SiftKeyMap *
sift_keymap_open(Relation rel, AttrNumber att, bool write)
{
	Form_pg_attribute a = TupleDescAttr(RelationGetDescr(rel), att - 1);
	Oid			store;
	SiftKeyMap *m;
	SiftVStore *vs;

	if (!sift_keymap_type(a->atttypid))
		return NULL;
	store = sift_vstore_find(rel, att);
	if (!OidIsValid(store))
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_OBJECT), errmsg("column \"%s\" of \"%s\" has no value store for the codes of a scope key", NameStr(a->attname), RelationGetRelationName(rel)),
						errhint("REINDEX the scope index that declares it.")));
	m = palloc0(sizeof(SiftKeyMap));
	m->cx = CurrentMemoryContext;
	m->store = store;
	m->typid = a->atttypid;
	m->collation = a->attcollation;
	m->write = write;
	sift_cc_policy_default(&m->pol);
	vs = store_open(m);
	sift_vstore_refresh(vs);
	sift_vstore_close(vs);
	return m;
}

Oid
sift_keymap_collation(const SiftKeyMap *m)
{
	return m->collation;
}

Oid
sift_keymap_typid(const SiftKeyMap *m)
{
	return m->typid;
}

/* The values of batch have a code: the ones the store lacks are appended as one run */
static void
intern_batch(SiftKeyMap *m, SiftCDict *batch)
{
	SiftVStore *vs = store_open(m);
	uint32	   *map = palloc(Max(batch->n, 1) * sizeof(uint32));
	uint64		epoch,
				ncodes;

	(void) sift_vstore_intern(vs, batch, &m->pol, map, &epoch, &ncodes);
	sift_vstore_refresh(vs);
	sift_vstore_close(vs);
	pfree(map);
}

/*
 * A writer's pass that meets every key of a relation: the values the store lacks are remembered, not interned one at a time (each would be a run of the store), and a code of 0 stands for them until
 * the pass is over (collect false), when they are appended as one run.
 */
void
sift_keymap_collect(SiftKeyMap *m, bool on)
{
	Assert(m->write);
	if (on)
	{
		m->pending = sift_cdict_create(m->cx);
		return;
	}
	if (m->pending->n > 0)
		intern_batch(m, m->pending);
	m->pending = NULL;
}

bool
sift_keymap_code(SiftKeyMap *m, Datum d, int64 *code)
{
	const char *s;
	uint32		len;
	SiftVStore *vs = store_open(m);
	int64		c;

	value_bytes(m->typid, d, &s, &len);
	c = sift_vstore_find_code(vs, s, len);
	if (c < 0 && m->pending == NULL)
	{
		/* a code in a segment of the directory the statement read was appended before that segment was published: the store as it is now has it */
		sift_vstore_refresh(vs);
		c = sift_vstore_find_code(vs, s, len);
	}
	sift_vstore_close(vs);
	if (c < 0 && m->pending != NULL)
	{
		(void) sift_cdict_intern(m->pending, s, len);
		c = 0;
	}
	else if (c < 0 && m->write)
	{
		SiftCDict  *one = sift_cdict_create(CurrentMemoryContext);

		(void) sift_cdict_intern(one, s, len);
		intern_batch(m, one);
		vs = store_open(m);
		c = sift_vstore_find_code(vs, s, len);
		sift_vstore_close(vs);
	}
	else if (c < 0)
	{
		if (m->extra == NULL)
			m->extra = sift_cdict_create(m->cx);
		c = -1 - (int64) sift_cdict_intern(m->extra, s, len);
	}
	*code = c;
	return true;
}

Datum
sift_keymap_datum(SiftKeyMap *m, int64 code)
{
	uint32		len;
	const char *s;

	if (code < 0)
		s = sift_cdict_str(m->extra, (uint32) (-1 - code), &len);
	else
	{
		SiftVStore *vs = store_open(m);

		s = sift_vstore_string(vs, (uint32) code, &len);
		sift_vstore_close(vs);
	}
	if (m->typid == UUIDOID)
	{
		pg_uuid_t  *u = palloc(UUID_LEN);

		memcpy(u->data, s, UUID_LEN);
		return UUIDPGetDatum(u);
	}
	return PointerGetDatum(cstring_to_text_with_len(s, len));
}
