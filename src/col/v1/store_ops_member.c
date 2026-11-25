/*
 * col/v1/store_ops_member.c - the float8 of a member of a map of a document (`(doc -> 'a' ->> 'k')::float8`, the map `doc.a.*` of the paths option) as a column of a Format 1 source, read from
 * the leaves of the map (col/v1/store_ops.h): the group leaf of the number of members of each row, and its two children, the key (a dictionary code) and the value of each member.
 *
 * A row's member is the value of its leaf when the group is NULL (the document, the map or the key is no object member: `->>` of it is NULL), when the map has no member of the key, or when the member's
 * value is a number the leaf holds or a JSON null (NULL). The rows the leaves cannot hold are poison, and the source reads their value from the heap row, which gives PostgreSQL's answers and
 * errors: a map the shredder cut or whose shape it did not take (the group has an overflow bit), a member whose key it could not intern, and the member itself when its value is no number or out of range.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement, not the rows of the table */
#include "postgres.h"

#include "fmgr.h"
#include "utils/builtins.h"
#include "utils/fmgrprotos.h"

#include "col/v1/store_ops.h"

struct SiftV1Member
{
	int			leaf[3];		/* the group, its key and its value */
	int			slot[3];		/* where the source reads them */
	const List *path;
	const char *key;
	const SiftSrcVec *vec[3];
	const void *dict_id;
	bool		coded;			/* the key is in the dictionary of the unit, as code */
	uint32		code;
	uint64		poison[SIFT_VEC_WORDS];
};

static inline bool
bit_at(const uint64 *bits, uint32 i)
{
	return bits != NULL && ((bits[i >> 6] >> (i & 63)) & 1);
}

static int
child_of(const SiftSrcLayout *lay, int group, const char *name)
{
	for (int i = 0; i < lay->nleaves; i++)
		if (lay->leaf[i].parent == group && strcmp(lay->leaf[i].name, name) == 0)
			return i;
	return -1;
}

int
sift_v1_member_value_leaf(const SiftSrcLayout *lay, int att, const List *path)
{
	for (int g = 0; g < lay->nleaves; g++)
	{
		const SiftSrcLeaf *l = &lay->leaf[g];
		const int	key = child_of(lay, g, "key"),
					value = child_of(lay, g, "value");

		if (l->kind != SCL_OBJECT || l->attno != att || l->parent >= 0 || list_length(l->path) != list_length(path) - 1 || !equal(list_copy_head(path, list_length(path) - 1), l->path))
			continue;
		if (key >= 0 && value >= 0 && lay->leaf[key].vtype == SCV_CODE && lay->leaf[value].kind == SCL_SCALAR && lay->leaf[value].vtype == SCV_F64 &&
			lay->leaf[value].typid == FLOAT8OID && !lay->leaf[value].wide && !lay->leaf[value].astext)
			return value;
	}
	return -1;
}

SiftV1Member *
sift_v1_member_new(MemoryContext cx, const SiftSrcLayout *lay, int value_leaf, const List *path)
{
	SiftV1Member *m = MemoryContextAllocZero(cx, sizeof(SiftV1Member));
	const int	group = lay->leaf[value_leaf].parent;

	m->leaf[0] = group;
	m->leaf[1] = child_of(lay, group, "key");
	m->leaf[2] = value_leaf;
	m->path = path;
	m->key = strVal(llast(path));
	return m;
}

int
sift_v1_member_leaf(const SiftV1Member *m, int part)
{
	return m->leaf[part];
}

void
sift_v1_member_bind(SiftV1Member *m, int part, int slot)
{
	m->slot[part] = slot;
}

/* the code of the key in the dictionary of the unit that is read: the dictionary is the same for the units that share it */
static void
find_code(SiftV1Member *m, const SiftSrcDict *dict)
{
	const size_t len = strlen(m->key);

	if (dict->id == m->dict_id && m->dict_id != NULL)
		return;
	m->dict_id = dict->id;
	m->coded = false;
	for (uint32 c = 0; c < dict->n && !m->coded; c++)
	{
		char		buf[SIFT_SRC_DICT_ENTRY];
		uint32		n;
		const char *s = sift_srcdict_str(dict, c, &n, buf);

		if (n == len && memcmp(s, m->key, len) == 0)
		{
			m->coded = true;
			m->code = c;
		}
	}
}

void
sift_v1_member_unit(SiftV1Member *m, const SiftSrcVec *read)
{
	for (int p = 0; p < 3; p++)
		m->vec[p] = &read[m->slot[p]];
	find_code(m, m->vec[1]->dict);
}

void
sift_v1_member_batch(SiftV1Member *m)
{
	memset(m->poison, 0, sizeof(m->poison));
}

/* the row at ordinal o of the unit has its member in the leaves: *valid says whether it has a value; false when the leaves cannot hold the row */
static bool
row_value(const SiftV1Member *m, uint32 o, double *value, bool *valid)
{
	const SiftSrcVec *group = m->vec[0],
			   *key = m->vec[1],
			   *val = m->vec[2];

	*valid = false;
	if (bit_at(group->ovf, o))
		return false;
	if (group->isnull != NULL && group->isnull[o])
		return true;
	if (group->eoff == NULL)
		return false;
	for (uint32 el = group->eoff[o]; el < group->eoff[o + 1]; el++)
	{
		if ((key->isnull != NULL && key->isnull[el]) || bit_at(key->ovf, el))
			return false;
		if (!m->coded || ((const uint32 *) key->data)[el] != m->code)
			continue;
		if (bit_at(val->ovf, el))
			return false;
		*valid = val->isnull == NULL || !val->isnull[el];
		*value = *valid && ((const double *) val->data)[el] != 0 ? ((const double *) val->data)[el] : 0;
		return true;
	}
	return true;
}

void
sift_v1_member_put(SiftV1Member *m, uint32 o, uint32 n, double *data, uint64 *valid)
{
	double		v = 0;
	bool		has;

	if (!row_value(m, o, &v, &has))
		m->poison[n >> 6] |= UINT64CONST(1) << (n & 63);
	data[n] = v;
	valid[n >> 6] |= (uint64) has << (n & 63);
}

const uint64 *
sift_v1_member_poison(const SiftV1Member *m)
{
	return m->poison;
}

bool
sift_v1_member_doc(const SiftV1Member *m, Datum doc, double *value)
{
	LOCAL_FCINFO(fcinfo, 2);
	Datum		d = doc;
	int			at = 0;

	foreach_node(String, k, m->path)
	{
		const bool	last = ++at == list_length(m->path);

		InitFunctionCallInfoData(*fcinfo, NULL, 2, InvalidOid, NULL, NULL);
		fcinfo->args[0] = (NullableDatum) {.value = d};
		fcinfo->args[1] = (NullableDatum) {.value = PointerGetDatum(cstring_to_text(strVal(k)))};
		d = last ? jsonb_object_field_text(fcinfo) : jsonb_object_field(fcinfo);
		if (fcinfo->isnull)
			return false;
	}
	*value = DatumGetFloat8(DirectFunctionCall1(float8in, CStringGetDatum(text_to_cstring(DatumGetTextPP(d)))));
	return true;
}
