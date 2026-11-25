/*
 * col/f2/store/schema.c - the schema, the option declarations and the operator cache of a sift_table store, and the scaled integer of numeric(p <= 18, s) (col/f2/store/schema.h).
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/stratnum.h"
#include "catalog/pg_am.h"
#include "catalog/pg_type.h"
#include "commands/defrem.h"
#include "utils/builtins.h"
#include "utils/fmgroids.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/f2/store/schema.h"
#include "col/pg.h"

bool
sift_f2_attr_num64(Form_pg_attribute att, uint8 *scale)
{
	int32		typmod = att->atttypmod;
	int			p,
				s;

	if (att->atttypid != NUMERICOID || typmod < (int32) VARHDRSZ)
		return false;
	p = ((typmod - VARHDRSZ) >> 16) & 0xFFFF;
	s = (((typmod - VARHDRSZ) & 0x7FF) ^ 1024) - 1024;
	if (p > SIFT_COL_MAX_SCALE || s < 0 || s > p)
		return false;
	*scale = (uint8) s;
	return true;
}

void
sift_f2_num64_cell(Datum d, uint8 scale, MemoryContext row_cx, SiftCell *c)
{
	MemoryContext old = MemoryContextSwitchTo(row_cx);
	Numeric		num = DatumGetNumeric(d);

	if (!numeric_is_nan(num))
		c->i = sift_col_numeric_key(scale, d, SIFT_COL_ROUND_NEAREST);
	else
	{
		c->xp = (const char *) num;
		c->xplen = VARSIZE_ANY(c->xp);
		c->isnull = true;
		c->tag = SIFT_COLTAG_UNSURE;
	}
	MemoryContextSwitchTo(old);
}

Datum
sift_f2_num64_datum(int64 key, uint8 scale)
{
	return sift_col_key_numeric(scale, key);
}

Datum
sift_f2_num64_exception_datum(const char *payload, uint32 len, MemoryContext cx)
{
	return PointerGetDatum(memcpy(MemoryContextAlloc(cx, len), payload, len));
}

/* ---- the schema ---- */

bool
sift_f2_attr_lossless_str(const SiftF2Store *store, Form_pg_attribute att)
{
	if (!store->headerless)
		return false;
	return att->atttypid == UUIDOID || (sift_f2_attr_bare_bytes(att) && (att->atttypid != BPCHAROID || att->atttypmod >= 0));
}

static SiftColStore
store_of(SiftCellKind kind, Form_pg_attribute att)
{
	if (kind == SIFT_CELL_INT)
		return SIFT_COLS_INT;
	if (kind == SIFT_CELL_F8)
		return att->atttypid == FLOAT4OID ? SIFT_COLS_F4 : SIFT_COLS_F8;
	return SIFT_COLS_STR;
}

static SiftColSchema *
make_schema(Relation rel, const SiftF2Store *s)
{
	TupleDesc	desc = RelationGetDescr(rel);
	SiftColDef *defs = palloc0(sizeof(SiftColDef) * Max(s->ncols, 1));

	for (int c = 0; c < s->ncols; c++)
	{
		Form_pg_attribute att = TupleDescAttr(desc, s->leaf_attno[c] - 1);

		if (sift_f2_leaf_is_path(s, c))
		{
			defs[c] = s->path_def[c];
			continue;
		}
		defs[c] = (SiftColDef) {
			.id = (int16) c,.parent = -1,.kind = SIFT_COLK_SCALAR,.store = store_of(s->kinds[c], att),.attno = s->leaf_attno[c],
			.type = s->hints[c] == SIFT_CHINT_NUM64 ? SIFT_COLT_NUMERIC : sift_col_type_of_oid(att->atttypid),.path = "",.scale = s->scales[c],
			.flags = s->kinds[c] == SIFT_CELL_STR && !sift_f2_attr_lossless_str(s, att) ? SIFT_COLD_LOSSY : 0,.name = pstrdup(NameStr(att->attname)),
		};
	}
	return sift_cschema_create(s->cx, defs, s->ncols);
}

/* ---- the options ---- */

static int
leaf_named(const SiftF2Store *s, Relation rel, const char *name)
{
	AttrNumber	a = name == NULL ? InvalidAttrNumber : get_attnum(RelationGetRelid(rel), name);

	return a > 0 && a <= s->natts ? s->att_first[a] : -1;
}

static int
first_time_leaf(const SiftF2Store *s)
{
	return s->spec.time_cols[0] - 1;
}

/* The wrap of the chunks of a part is one for the whole part: that of the default entry of the option (SiftRows.compress_alg: < 0 none, 0 by the tier, else SIFT_CC_ALG_*) */
static int
part_compress_alg(const char *option)
{
	SiftCcPolicy def;

	(void) sift_cc_policy_for_column(option, "", &def);
	return def.mode == SIFT_CCM_NONE ? -1 : def.mode == SIFT_CCM_LAYER ? def.alg : 0;
}

static void
make_compression(SiftF2Store *s, Relation rel)
{
	struct SiftCompressPolicy *cp = MemoryContextAllocZero(s->cx, sizeof(*cp));
	const char *err = NULL;

	cp->pol = MemoryContextAlloc(s->cx, sizeof(SiftCcPolicy) * Max(s->ncols, 1));
	for (int c = 0; c < s->ncols && err == NULL; c++)
		err = sift_cc_policy_for_column(s->opts.compression, s->schema->cols[c].name, &cp->pol[c]);
	if (err != NULL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("compression of table \"%s\": %s", RelationGetRelationName(rel), err)));
	s->comp_policy = cp;
	s->spec.compress_alg = part_compress_alg(s->opts.compression);
}

static void
make_scopes(SiftF2Store *s, Relation rel)
{
	const char *err = NULL;
	SiftScopeSpecs *all = sift_scope_parse(s->opts.scope, &err);

	if (all == NULL)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("scope of table \"%s\": %s", RelationGetRelationName(rel), err)));
	s->scopes = MemoryContextAllocZero(s->cx, sizeof(struct SiftScopeDecl) * Max(all->n, 1));
	for (int i = 0; i < all->n; i++)
	{
		int			member = leaf_named(s, rel, all->s[i]->fk);

		if (member < 0)
			ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("scope of table \"%s\" names the column \"%s\", which it does not have", RelationGetRelationName(rel), all->s[i]->fk)));
		s->scopes[i] = (struct SiftScopeDecl) {.spec = all->s[i],.member_leaf = member,.time_leaf = first_time_leaf(s)};
	}
	s->nscopes = all->n;
}

static void
make_sketches(SiftF2Store *s, Relation rel)
{
	SiftColRel	by_attribute = {.first = s->att_first};
	struct SiftSketchPolicy *sp = MemoryContextAllocZero(s->cx, sizeof(*sp));

	sift_f2_kll_validate(s->opts.sketches);
	sift_f2_kll_columns(rel, &by_attribute, s->opts.sketches, &sp->kll);
	s->sketch_policy = sp;
	s->spec.kll = &sp->kll;
}

/* The default btree operator class of each attribute: the family and the equality operator the planner's quals are matched against, without an index to ask */
static void
make_operators(SiftF2Store *s, Relation rel)
{
	TupleDesc	desc = RelationGetDescr(rel);

	s->opfamily = MemoryContextAllocZero(s->cx, sizeof(Oid) * (s->natts + 1));
	s->eq_opno = MemoryContextAllocZero(s->cx, sizeof(Oid) * (s->natts + 1));
	for (int a = 1; a <= s->natts; a++)
	{
		Oid			type = TupleDescAttr(desc, a - 1)->atttypid;
		Oid			opclass = s->att_first[a] < 0 ? InvalidOid : GetDefaultOpClass(type, BTREE_AM_OID);

		if (!OidIsValid(opclass))
			continue;
		s->opfamily[a] = get_opclass_family(opclass);
		s->eq_opno[a] = get_opfamily_member(s->opfamily[a], get_opclass_input_type(opclass), get_opclass_input_type(opclass), BTEqualStrategyNumber);
	}
}

int
sift_f2_store_attr_leaves(const SiftF2Store *s, int att)
{
	int			n = 0;

	for (int c = s->att_first[att]; c >= 0 && c < s->ncols && s->leaf_attno[c] == att; c++)
		n++;
	return n;
}

static const char *
declared(const char *text, MemoryContext cx)
{
	return text[0] == '\0' ? NULL : MemoryContextStrdup(cx, text);
}

void
sift_f2_store_hydrate(Relation rel, SiftF2Store *store)
{
	const SiftF2StoreOpts *o = &store->opts;

	store->schema = make_schema(rel, store);
	make_compression(store, rel);
	if (o->scope[0] != '\0')
		make_scopes(store, rel);
	if (o->sketches[0] != '\0')
		make_sketches(store, rel);
	if (o->skip_index[0] != '\0')
	{
		store->skip_policy = MemoryContextAllocZero(store->cx, sizeof(struct SiftSkipPolicy));
		store->skip_policy->spec = declared(o->skip_index, store->cx);
	}
	if (o->paths[0] != '\0')
	{
		store->shred = MemoryContextAllocZero(store->cx, sizeof(struct SiftColShred));
		store->shred->spec = declared(o->paths, store->cx);
		store->shred->automatic = strcmp(o->paths, "auto") == 0;
	}
	make_operators(store, rel);
}
