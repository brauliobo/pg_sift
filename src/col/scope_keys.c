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
#include "utils/builtins.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/uuid.h"
#include "utils/varlena.h"

#include "col/am.h"
#include "col/conn.h"
#include "col/scope.h"
#include "col/vstore/vstore.h"

#ifdef SIFT_TESTING
int			sift_test_member_hash_bits = 0;
#endif

struct SiftKeyMap
{
	Oid			store;
	Oid			typid;
	Oid			collation;
	bool		write;
	SiftCDict  *pending;		/* collecting: the values the store lacks, to intern together */
	SiftCDict  *extra;			/* a reader's: the values the store lacks, as the transient codes -1, -2, ... */
	SiftVStore *held;			/* the store, open for a loop that asks for many codes (sift_keymap_hold) */
	int			holds;
	MemoryContext cx;
	SiftCcPolicy pol;
};

bool
sift_keymap_type(Oid typid)
{
	return typid == TEXTOID || typid == VARCHAROID || typid == UUIDOID;
}

/*
 * A column a scope joins by: an integer, or a text-like or uuid column of a deterministic collation (what sift_keymap_type names); the reason it is not, NULL when it is. The values of a string
 * column are equal when their bytes are, so a type whose equality is not that (blank padded, case folded, a domain that may add a check or a collation of its own) is refused, with the fix.
 */
const char *
sift_keymap_column_refusal(Oid typid, Oid collation)
{
	if (sift_conn_integer_type(typid))
		return NULL;
	if (typid == BPCHAROID)
		return "is a char(n) column, whose values are equal when they differ in blank padding: declare it text or varchar";
	if (getBaseType(typid) != typid)
		return "is a domain: declare the column with its base type, an integer, text, varchar or uuid";
	if (!sift_keymap_type(typid))
		return strcmp(format_type_be(typid), "citext") == 0 ? "is a citext column, whose values are equal when they differ in case: declare it text with a deterministic collation" :
			"is not an integer, text, varchar or uuid column";
	if (typid != UUIDOID && OidIsValid(collation) && !get_collation_isdeterministic(collation))
		return "has a nondeterministic collation, whose equal values may differ in their bytes: use a deterministic one (COLLATE \"C\")";
	return NULL;
}

/* Do two columns of a scope hold values that are equal when their bytes are, of one kind: text with varchar, uuid with uuid, an integer with an integer */
bool
sift_keymap_same_kind(Oid a, Oid b)
{
	return (sift_conn_integer_type(a) && sift_conn_integer_type(b)) || (a == UUIDOID && b == UUIDOID) ||
		(a != UUIDOID && b != UUIDOID && sift_keymap_type(a) && sift_keymap_type(b));
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

/*
 * The word a journal of the scope relation records for a text, varchar or uuid member: 64-bit FNV-1a of its bytes (the 16 of a uuid), the same on every platform and every release, because
 * the records are persisted, read on a standby and after a dump and restore. It is a filter, not an identity: two members with one word are both read as changed, which only widens what the
 * reader re-reads. sift.test_member_hash_bits keeps the low bits of it, to make that happen.
 */
uint64
sift_member_hash(Oid typid, Datum d)
{
	const char *s;
	uint32		len;

	value_bytes(typid, d, &s, &len);
	return sift_member_hash_bytes(s, len);
}

uint64
sift_member_hash_bytes(const char *s, uint32 len)
{
	uint64		h = UINT64CONST(0xcbf29ce484222325);

	for (uint32 i = 0; i < len; i++)
		h = (h ^ (unsigned char) s[i]) * UINT64CONST(0x100000001b3);
#ifdef SIFT_TESTING
	if (sift_test_member_hash_bits > 0 && sift_test_member_hash_bits < 64)
		h &= (UINT64CONST(1) << sift_test_member_hash_bits) - 1;
#endif
	return h;
}

/* The store, open for one operation (its view is the backend's: the lock is held to the end of the transaction) */
static SiftVStore *
store_open(const SiftKeyMap *m)
{
	return m->held != NULL ? m->held : sift_vstore_open(m->store, m->write ? RowExclusiveLock : AccessShareLock);
}

static void
store_close(const SiftKeyMap *m, SiftVStore *vs)
{
	if (vs != m->held)
		sift_vstore_close(vs);
}

/* A loop that asks for the code of every row it reads keeps the store open: opening it for each (a lock, the catalog) costs more than the lookup */
void
sift_keymap_hold(SiftKeyMap *m)
{
	if (OidIsValid(m->store) && m->holds++ == 0)
		m->held = store_open(m);
}

void
sift_keymap_release(SiftKeyMap *m)
{
	if (OidIsValid(m->store) && --m->holds == 0)
	{
		sift_vstore_close(m->held);
		m->held = NULL;
	}
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
	store_close(m, vs);
	return m;
}

/* A map of the members of a statement: no store, every value has a code of its own for the length of the map (a negative one, -1 for the first) */
SiftKeyMap *
sift_keymap_transient(Relation rel, AttrNumber att)
{
	Form_pg_attribute a = TupleDescAttr(RelationGetDescr(rel), att - 1);
	SiftKeyMap *m;

	if (!sift_keymap_type(a->atttypid))
		return NULL;
	m = palloc0(sizeof(SiftKeyMap));
	m->cx = CurrentMemoryContext;
	m->typid = a->atttypid;
	m->collation = a->attcollation;
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
	store_close(m, vs);
	pfree(map);
}

/*
 * A writer's pass that meets every key of a relation: the values the store lacks are remembered, not interned one at a time (each would be a run of the store), and a code of 0 stands for them until
 * the pass is over (collect false), when they are appended as one run.
 */
void
sift_keymap_collect(SiftKeyMap *m, bool on)
{
	if (!OidIsValid(m->store))
		return;
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

/* The code the store of the key has for the value, appended first by a writer (or, collecting, remembered to be: 0); -1 for a reader's value the store lacks */
static int64
stored_code(SiftKeyMap *m, const char *s, uint32 len)
{
	SiftVStore *vs = store_open(m);
	int64		c = sift_vstore_find_code(vs, s, len);

	if (c < 0 && m->pending == NULL)
	{
		/* a code in a segment of the directory the statement read was appended before that segment was published: the store as it is now has it */
		sift_vstore_refresh(vs);
		c = sift_vstore_find_code(vs, s, len);
	}
	store_close(m, vs);
	if (c < 0 && m->pending != NULL)
	{
		(void) sift_cdict_intern(m->pending, s, len);
		return 0;
	}
	if (c < 0 && m->write)
	{
		SiftCDict  *one = sift_cdict_create(CurrentMemoryContext);

		(void) sift_cdict_intern(one, s, len);
		intern_batch(m, one);
		vs = store_open(m);
		c = sift_vstore_find_code(vs, s, len);
		store_close(m, vs);
	}
	return c;
}

bool
sift_keymap_code(SiftKeyMap *m, Datum d, int64 *code)
{
	const char *s;
	uint32		len;

	value_bytes(m->typid, d, &s, &len);
	return sift_keymap_code_bytes(m, s, len, code);
}

bool
sift_keymap_code_bytes(SiftKeyMap *m, const char *s, uint32 len, int64 *code)
{
	int64		c;

	c = OidIsValid(m->store) ? stored_code(m, s, len) : -1;
	if (c < 0)
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
		store_close(m, vs);
	}
	if (m->typid == UUIDOID)
	{
		pg_uuid_t  *u = palloc(UUID_LEN);

		memcpy(u->data, s, UUID_LEN);
		return UUIDPGetDatum(u);
	}
	return PointerGetDatum(cstring_to_text_with_len(s, len));
}

/* The bytes of the value a transient code stands for (a map with no store) */
const char *
sift_keymap_bytes(SiftKeyMap *m, int64 code, uint32 *len)
{
	Assert(code < 0);
	return sift_cdict_str(m->extra, (uint32) (-1 - code), len);
}

/* The journal word of the member a transient code stands for */
uint64
sift_keymap_member_hash(SiftKeyMap *m, int64 code)
{
	uint32		len;
	const char *s = sift_keymap_bytes(m, code, &len);

	return sift_member_hash_bytes(s, len);
}

/* The code of a value the map has one for, as a reader's (false: no pair has the value, the map never gives it a code here) */
bool
sift_keymap_find_bytes(SiftKeyMap *m, const char *s, uint32 len, int64 *code)
{
	int64		i = m->extra != NULL ? sift_cdict_find(m->extra, s, len) : -1;

	*code = -1 - i;
	return i >= 0;
}

/*
 * The codes of the rows of a batch whose leaf holds the strings of dict (key: the codes of the dictionary, nulls: bit set = NULL, or NULL): one lookup for each value of the dictionary a row
 * holds, not for each row. intern: a value the map lacks gets a code (a writer's fill), else it has none (PG_INT64_MIN, as a NULL row has).
 */
void
sift_keymap_batch_codes(SiftKeyMap *m, const SiftCDict *dict, const int64 *key, const uint64 *nulls, uint32 nrows, bool intern, int64 *out)
{
	int64	   *memo = palloc(Max(dict->n, 1) * sizeof(int64));
	int64		none = PG_INT64_MIN + 1;

	for (uint32 i = 0; i < dict->n; i++)
		memo[i] = none;
	for (uint32 r = 0; r < nrows; r++)
	{
		int64		code = key[r];

		out[r] = PG_INT64_MIN;
		if (nulls != NULL && sift_col_bit_test(nulls, r))
			continue;
		if (memo[code] == none)
		{
			uint32		len;
			const char *s = sift_cdict_str(dict, (uint32) code, &len);

			if (intern)
				(void) sift_keymap_code_bytes(m, s, len, &memo[code]);
			else if (!sift_keymap_find_bytes(m, s, len, &memo[code]))
				memo[code] = PG_INT64_MIN;
		}
		out[r] = memo[code];
	}
	pfree(memo);
}
