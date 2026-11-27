/*
 * col/v1_engine/store_ops_text.c - the tests a source of Format 1 runs on a unit before it reads it (col/v1/store_ops.c unit_next): the JSON types of the scan keys, and the text quals of the batch
 * quals of its fold (agg/text_qual.h) over a dictionary leaf, decided once for each string of the dictionary. A unit whose dictionary holds no string a qual passes is not read.
 */
/* check-memory: allow-allocs the flags follow the strings of one dictionary of a leaf, not the rows of the relation */
#include "postgres.h"

#include "agg/qual/bqual.h"
#include "agg/text_qual.h"
#include "col/v1/store_src.h"

/* the dictionary leaf of the input column of attribute attno that the batches give as strings, NULL when none */
static const SiftSrcLeaf *
text_leaf(const SiftV1Src *s, AttrNumber attno)
{
	for (int i = 0; i < s->in.ncols; i++)
	{
		const SiftSrcLeaf *l = s->col_leaf[i];

		if (s->in.col[i].attno == attno && s->in.col[i].type == SVEC_STRVIEW && s->in.col[i].path == NIL && l != NULL && l->vtype == SCV_CODE && l->parent < 0 &&
			l->path == NIL && !l->wide && !l->lossy)
			return l;
	}
	return NULL;
}

static void
bind_texts(SiftV1Src *s, const SiftV6Bq *bq)
{
	const SiftTextQual *q;
	AttrNumber	attno;

	for (int i = 0; s->ntexts < SIFT_V1_MAX_TEXT_TESTS && (q = sift_v6_bq_text(bq, i, &attno)) != NULL; i++)
	{
		const SiftSrcLeaf *l = text_leaf(s, attno);

		if (l != NULL)
			s->texts[s->ntexts++] = (SiftV1TextTest) {.leaf = l->id,.q = q};
	}
}

void
sift_v1_src_bind_tests(SiftV1Src *s, const SiftV1Scan *scan)
{
	SiftSrcStats st;

	memcpy(s->types, scan->types, scan->ntypes * sizeof(SiftV1TypeTest));
	s->ntypes = scan->ntypes;
	for (int t = 0; t < s->ntypes; t++)
		if (s->types[t].key)
		{
			s->rd->stats(s->rels.rel, &st);
			s->types[t].rootless = st.rootless[s->types[t].leaf] > 0;
		}
	bind_texts(s, scan->texts);
}

/* whether each string of dictionary d passes the qual of t, kept while the units share d */
static const bool *
dict_pass(SiftV1Src *s, SiftV1TextTest *t, const SiftSrcDict *d)
{
	char		buf[SIFT_SRC_DICT_ENTRY];
	uint32		len;

	if (t->dict == d->id)
		return t->pass;
	if (t->pass != NULL)
		pfree(t->pass);
	t->pass = MemoryContextAlloc(s->cx, Max(d->n, 1) * sizeof(bool));
	t->dict = d->id;
	for (uint32 c = 0; c < d->n; c++)
	{
		const char *str = sift_srcdict_str(d, c, &len, buf);

		t->pass[c] = sift_text_qual_holds(t->q, (const uint8 *) str, len);
	}
	return t->pass;
}

/* the rows of the unit whose string passes the qual of t; a NULL string passes none, a value the leaf cannot hold is left to the batch */
static bool
text_test(SiftV1Src *s, SiftV1TextTest *t)
{
	SiftSrcVec	v;
	const bool *pass;
	const uint32 *code;
	bool		left = false;

	s->rd->read(s->src, &s->unit, t->leaf, &v);
	if (v.dict == NULL)
		return true;
	pass = dict_pass(s, t, v.dict);
	code = v.data;
	for (uint32 r = 0; r < s->unit.nrow; r++)
	{
		if (!sift_src_bit(s->bits, r))
			continue;
		if ((v.ovf == NULL || !sift_src_bit(v.ovf, r)) && ((v.isnull != NULL && v.isnull[r]) || !pass[code[r]]))
			s->bits[r >> 6] &= ~(UINT64CONST(1) << (r & 63));
		else
			left = true;
	}
	return left;
}

bool
sift_v1_src_text_pass(SiftV1Src *s)
{
	for (int t = 0; t < s->ntexts; t++)
		if (!text_test(s, &s->texts[t]))
			return false;
	return true;
}
