/*
 * col/v1/store_ops_elem.c - the elements of the shredded array of a Format 1 source (col/v1/store_ops.h): the leaf of the lengths (SCL_ARRAY) of an array column and the child leaves of the
 * columns of its elements, which a unit keeps per element after the rows (SiftSrcVec.eoff gives the first element of each row).
 *
 * A row's array is held by the leaves when its lengths leaf has a value that is no overflow (a document that is no array, an array cut by the limits of the shredder or a JSON null where the lengths
 * leaf is NULL are not) and none of its elements has an overflow value in a column read (a value the declared type cannot hold: another JSON type, a number out of range, a text too long, an
 * element that is no object). The rows the leaves cannot hold are poison: the consumer reads their array from the heap row, which also gives the NULL arrays and the errors of PostgreSQL. A row of a
 * NULL lengths leaf is poison too, since the leaf does not tell a NULL array from a JSON null, which PostgreSQL refuses.
 */
/* check-memory: allow-allocs every array here follows the columns of one statement or the rows of one batch, not the rows of the relation */
#include "postgres.h"

#include "col/v1/store_ops.h"

struct SiftV1Elems
{
	SiftV1ElemSpec spec;
	SiftSrcVecType vtype[SIFT_V1_MAX_ELEM_COLS];
	SiftV1ElemBatch out;
	uint32		offsets[SIFT_VEC_LEN + 1];
	uint32		lengths[SIFT_VEC_LEN];
	uint32		blocks[SIFT_VEC_WORDS + 1];
	uint64		poison[SIFT_VEC_WORDS];
	uint64		doc_valid[SIFT_VEC_WORDS];
};

static inline bool
bit_at(const uint64 *bits, uint32 i)
{
	return (bits[i >> 6] >> (i & 63)) & 1;
}

static inline void
bit_set(uint64 *bits, uint32 i)
{
	bits[i >> 6] |= UINT64CONST(1) << (i & 63);
}

void
sift_v1_strview(MemoryContext cx, SiftStrView *sv, const SiftSrcDict *dict, uint32 code)
{
	char		buf[SIFT_SRC_DICT_ENTRY];
	uint32		len;
	const char *p = sift_srcdict_str(dict, code, &len, buf);

	memset(sv, 0, sizeof(*sv));
	sv->len = len;
	memcpy(sv->prefix, p, Min(len, 4));
	if (len > 12)
		sv->u.ptr = memcpy(MemoryContextAlloc(cx, len), p, len);
	else if (len > 4)
		memcpy(sv->u.tail, p + 4, len - 4);
}

/* a column of the elements the leaf holds as the vector type says: an integer or a boolean as int8, a float as float8, a text as a string view of the codes of its dictionary */
static bool
child_holds(const SiftSrcLeaf *l, SiftVecType type, bool kind)
{
	if (l->kind != SCL_SCALAR || l->wide || l->scale != 0 || l->typid == NUMERICOID || l->arrhead || l->arrlen != 0)
		return false;
	if (kind)
		return type == SVEC_I64;
	switch (type)
	{
		case SVEC_I64:
			return l->vtype == SCV_I32 || l->vtype == SCV_I64;
		case SVEC_F64:
			return l->vtype == SCV_F64;
		case SVEC_STRVIEW:
			return l->vtype == SCV_CODE;
		default:
			return false;
	}
}

bool
sift_v1_elem_spec(const SiftSrcLayout *lay, int group_leaf, int nchild, const int *child_leaf, const SiftVecType *child_type, const bool *child_kind, SiftV1ElemSpec *out)
{
	const SiftSrcLeaf *g;

	if (group_leaf < 0 || group_leaf >= lay->nleaves || nchild < 1 || nchild > SIFT_V1_MAX_ELEM_COLS)
		return false;
	g = &lay->leaf[group_leaf];
	if ((g->kind != SCL_ARRAY && g->kind != SCL_OBJECT) || g->parent >= 0 || g->path != NIL)
		return false;
	*out = (SiftV1ElemSpec) {.group_leaf = group_leaf,.nchild = nchild};
	for (int c = 0; c < nchild; c++)
	{
		const bool	kind = child_kind != NULL && child_kind[c];

		if (child_leaf[c] < 0 || child_leaf[c] >= lay->nleaves || lay->leaf[child_leaf[c]].parent != group_leaf || !child_holds(&lay->leaf[child_leaf[c]], child_type[c], kind))
			return false;
		out->child_leaf[c] = child_leaf[c];
		out->child_type[c] = child_type[c];
		out->child_kind[c] = kind;
	}
	return true;
}

SiftV1Elems *
sift_v1_elems_new(MemoryContext cx, const SiftV1ElemSpec *spec, const SiftSrcLayout *lay)
{
	SiftV1Elems *e = MemoryContextAllocZero(cx, sizeof(SiftV1Elems));

	e->spec = *spec;
	for (int c = 0; c < spec->nchild; c++)
		e->vtype[c] = lay->leaf[spec->child_leaf[c]].vtype;
	return e;
}

/* whether one of the elements [from, to) of a leaf has an overflow value */
static bool
range_overflows(const uint64 *ovf, uint32 from, uint32 to)
{
	for (uint32 i = from; ovf != NULL && i < to;)
	{
		const uint32 w = i >> 6;
		const uint64 bits = ovf[w] >> (i & 63);
		const uint32 span = 64 - (i & 63);

		if (bits != 0 && (uint32) __builtin_ctzll(bits) < to - i)
			return true;
		i += Min(span, to - i);
	}
	return false;
}

/* whether one of the elements [from, to) of a leaf has no value: a missing member and a JSON null look the same, and a kind column tells them apart */
static bool
range_has_null(const uint8 *isnull, uint32 from, uint32 to)
{
	for (uint32 i = from; isnull != NULL && i < to; i++)
		if (isnull[i])
			return true;
	return false;
}

/* an element of the array has a value in none of the columns read: the shredder keeps an element that is no object (a number, a string, a null, an array) as an object with no key, which is no object to PostgreSQL */
static bool
empty_element(const SiftV1Elems *e, const SiftSrcVec *const *child, uint32 el)
{
	for (int c = 0; c < e->spec.nchild; c++)
		if (child[c]->isnull == NULL || !child[c]->isnull[el])
			return false;
	return true;
}

/* the row at ordinal o of the unit has its array in the leaves, and none of its elements holds an overflow value of a column read or may be no object */
static bool
row_held(const SiftV1Elems *e, const SiftSrcVec *group, const SiftSrcVec *const *child, uint32 o)
{
	if ((group->isnull != NULL && group->isnull[o]) || (group->ovf != NULL && bit_at(group->ovf, o)))
		return false;
	for (int c = 0; c < e->spec.nchild; c++)
		if (range_overflows(child[c]->ovf, group->eoff[o], group->eoff[o + 1]) || (e->spec.child_kind[c] && range_has_null(child[c]->isnull, group->eoff[o], group->eoff[o + 1])))
			return false;
	for (uint32 el = group->eoff[o]; e->spec.objects && el < group->eoff[o + 1]; el++)
		if (empty_element(e, child, el))
			return false;
	return true;
}

static void
put_value(const SiftV1Elems *e, int c, const SiftSrcVec *v, uint32 src, void *data, uint32 dst, MemoryContext cx)
{
	if (e->spec.child_kind[c])
	{
		((int64 *) data)[dst] = SIFT_ELEM_KIND_VALUE;
		return;
	}
	switch (e->spec.child_type[c])
	{
		case SVEC_F64:
			((double *) data)[dst] = ((const double *) v->data)[src];
			break;
		case SVEC_STRVIEW:
			sift_v1_strview(cx, &((SiftStrView *) data)[dst], v->dict, ((const uint32 *) v->data)[src]);
			break;
		default:
			((int64 *) data)[dst] = sift_src_int(v->data, e->vtype[c], src);
			break;
	}
}

static size_t
cell_size(SiftVecType type)
{
	return type == SVEC_STRVIEW ? sizeof(SiftStrView) : sizeof(int64);
}

/* the elements of the row at ordinal o of the unit as the elements [at, at + count) of the columns of the batch */
static void
put_row(SiftV1Elems *e, const SiftSrcVec *group, const SiftSrcVec *const *child, uint32 o, uint32 at, void *const *data, uint64 *const *valid, MemoryContext cx)
{
	for (int c = 0; c < e->spec.nchild; c++)
		for (uint32 k = 0; k < group->eoff[o + 1] - group->eoff[o]; k++)
		{
			const uint32 src = group->eoff[o] + k;

			if (child[c]->isnull != NULL && child[c]->isnull[src])
				continue;
			bit_set(valid[c], at + k);
			put_value(e, c, child[c], src, data[c], at + k, cx);
		}
}

const SiftV1ElemBatch *
sift_v1_elems_batch(SiftV1Elems *e, const SiftSrcVec *group, const SiftSrcVec *const *child, const uint32 *ords, int n, const uint64 *mask, MemoryContext cx)
{
	uint32		total = 0;
	void	   *data[SIFT_V1_MAX_ELEM_COLS];
	uint64	   *valid[SIFT_V1_MAX_ELEM_COLS];

	memset(e->poison, 0, sizeof(e->poison));
	memset(e->doc_valid, 0, sizeof(e->doc_valid));
	for (int j = 0; j < n; j++)
	{
		e->offsets[j] = total;
		e->lengths[j] = 0;
		if (mask != NULL && !bit_at(mask, j))
			continue;
		if (group->eoff == NULL || !row_held(e, group, child, ords[j]))
		{
			bit_set(e->poison, j);
			continue;
		}
		bit_set(e->doc_valid, j);
		e->lengths[j] = group->eoff[ords[j] + 1] - group->eoff[ords[j]];
		total += e->lengths[j];
	}
	e->offsets[n] = total;
	for (int b = 0; b <= n / 64; b++)
		e->blocks[b] = e->offsets[Min(b * 64, n)];
	for (int c = 0; c < e->spec.nchild; c++)
	{
		const size_t w = cell_size(e->spec.child_type[c]);

		data[c] = MemoryContextAllocAligned(cx, Max(total, 1) * w + SIFT_VEC_PAD, SIFT_VEC_ALIGN, MCXT_ALLOC_ZERO);
		valid[c] = MemoryContextAllocZero(cx, ((total + 63) / 64 + 1) * sizeof(uint64));
		e->out.child_data[c] = data[c];
		e->out.child_valid[c] = valid[c];
	}
	for (int j = 0; j < n; j++)
		if (bit_at(e->doc_valid, j))
			put_row(e, group, child, ords[j], e->offsets[j], data, valid, cx);
	e->out.layout = (SiftArrayLayout) {.n_parents = n,.n_elems = total,.block_elem_off = e->blocks,.lengths = e->lengths,.offsets = e->offsets};
	e->out.poison = e->poison;
	e->out.doc_valid = e->doc_valid;
	return &e->out;
}
