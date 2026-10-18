/*
 * sift_col_am.c - the sift_col index access method: handler, options, the per-relation leaf list, tuple to row conversion and
 * aminsert (docs/COL.md). The build is in sift_col_build.c, VACUUM and maintenance in sift_col_maint.c, the read side in sift_col_scan.c.
 */
#include "postgres.h"

#include "access/amapi.h"
#include "access/htup_details.h"
#include "access/reloptions.h"
#include "access/table.h"
#include "access/visibilitymap.h"
#include "catalog/index.h"
#include "catalog/pg_am.h"
#include "catalog/pg_index.h"
#include "catalog/pg_type_d.h"
#include "commands/defrem.h"
#include "commands/vacuum.h"
#include "fmgr.h"
#include "nodes/makefuncs.h"
#include "optimizer/optimizer.h"
#include "utils/builtins.h"
#include "utils/guc.h"
#include "utils/lsyscache.h"
#include "utils/numeric.h"
#include "utils/memutils.h"
#include "utils/rel.h"

#include "sift.h"
#include "sift_col_am.h"
#include "sift_col_build.h"
#include "sift_col_seg.h"
#include "sift_cpu.h"

bool		sift_enable_col = true;

static relopt_kind col_relopt_kind;

/* Every entry of the option is a policy, optionally prefixed by "column =" */
void
sift_col_validate_compression(const char *value)
{
	char	   *list = pstrdup(value),
			   *save = NULL;

	for (char *e = strtok_r(list, ";", &save); e; e = strtok_r(NULL, ";", &save))
	{
		SiftCcPolicy p;
		const char *eq = strchr(e, '='),
				   *err = sift_cc_policy_parse(eq ? eq + 1 : e, &p);

		if (err)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid compression option \"%s\": %s", value, err)));
	}
	pfree(list);
}

void
sift_col_init(void)
{
	DefineCustomBoolVariable("sift.enable_col",
							 "Enables the column scans over sift_col indexes.",
							 NULL, &sift_enable_col, true,
							 PGC_USERSET, 0, NULL, NULL, NULL);
#ifdef SIFT_TESTING
	DefineCustomIntVariable("sift.test_col_workers",
							"Testing: parallel workers the last sift_col build of this session launched.",
							NULL, &sift_test_col_workers, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_fail_sb",
							"Testing: the participant of a sift_col build that takes this superblock (1 = the first) fails (0 = none).",
							NULL, &sift_test_col_fail_sb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE, NULL, NULL, NULL);
	DefineCustomIntVariable("sift.test_col_segment_kb",
							"Testing: kB of memory a sift_col build's segment writer holds before the segment is cut (0 = by maintenance_work_mem).",
							NULL, &sift_test_col_segment_kb, 0, 0, INT_MAX, PGC_USERSET, GUC_NOT_IN_SAMPLE | GUC_UNIT_KB, NULL, NULL, NULL);
#endif
	col_relopt_kind = add_reloption_kind();
	add_int_reloption(col_relopt_kind, "mutable_limit",
					  "Mutable segment size in kB before it is sealed for promotion",
					  SIFT_DEFAULT_MUTABLE_LIMIT, 64, 1048576, ShareUpdateExclusiveLock);
	add_string_reloption(col_relopt_kind, "paths",
						 "jsonb paths shredded into columns, by index attribute name",
						 "", NULL, AccessExclusiveLock);
	add_string_reloption(col_relopt_kind, "compression",
						 "chunk compression: auto, none, lz4, zstd, encodings; per column: 'auto; col=zstd:9'",
						 "", sift_col_validate_compression, ShareUpdateExclusiveLock);
}

/* ---------- options ---------- */

static bytea *
col_amoptions(Datum reloptions, bool validate)
{
	static const relopt_parse_elt tab[] = {
		{"mutable_limit", RELOPT_TYPE_INT, offsetof(SiftColOptions, mutable_limit)},
		{"paths", RELOPT_TYPE_STRING, offsetof(SiftColOptions, paths)},
		{"compression", RELOPT_TYPE_STRING, offsetof(SiftColOptions, compression)},
	};

	return (bytea *) build_reloptions(reloptions, validate, col_relopt_kind, sizeof(SiftColOptions), tab, lengthof(tab));
}

const SiftColOptions *
sift_col_options(Relation index)
{
	static const SiftColOptions defaults = {.mutable_limit = SIFT_DEFAULT_MUTABLE_LIMIT};

	return index->rd_options ? (const SiftColOptions *) index->rd_options : &defaults;
}

const char *
sift_col_options_paths(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->paths ? (const char *) o + o->paths : "";
}

const char *
sift_col_options_compression(Relation index)
{
	const SiftColOptions *o = sift_col_options(index);

	return o->compression ? (const char *) o + o->compression : "";
}

/* ---------- the leaves of an index ---------- */

bool
sift_col_leaf_store(Oid typid, SiftColStore *store)
{
	switch (typid)
	{
		case INT2OID:
		case INT4OID:
		case INT8OID:
		case BOOLOID:
		case DATEOID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
		case NUMERICOID:
			*store = SIFT_COLS_INT;
			return true;
		case FLOAT4OID:
			*store = SIFT_COLS_F4;
			return true;
		case FLOAT8OID:
			*store = SIFT_COLS_F8;
			return true;
		case TEXTOID:
		case VARCHAROID:
			*store = SIFT_COLS_STR;
			return true;
	}
	return false;
}

/* The scale of numeric(p, s); the INT class holds the number times 10^s exactly while p <= 18 (docs/COL.md 2.3) */
bool
sift_col_numeric_scale(Form_pg_attribute att, uint8 *scale)
{
	int32		tm = att->atttypmod - VARHDRSZ;
	int			prec = (tm >> 16) & 0xFFFF,
				s = ((tm & 0x7FF) ^ 1024) - 1024;

	if (att->atttypmod < VARHDRSZ || prec > SIFT_COL_MAX_SCALE || s < 0)
		return false;
	*scale = s;
	return true;
}

static uint8
numeric_scale_of(Form_pg_attribute att)
{
	uint8		scale;

	if (!sift_col_numeric_scale(att, &scale))
		ereport(ERROR,
				(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
				 errmsg("sift_col holds numeric(p, s) with p <= %d and s >= 0, not the type of column \"%s\"", SIFT_COL_MAX_SCALE,
						NameStr(att->attname)),
				 errhint("Declare the column as numeric(%d, s) or smaller; the values are stored as integers scaled by 10^s.", SIFT_COL_MAX_SCALE)));
	return scale;
}

static SiftColRel *
build_rel(Relation index)
{
	MemoryContext old = MemoryContextSwitchTo(index->rd_indexcxt);
	SiftColRel *r = palloc0(sizeof(SiftColRel));
	TupleDesc	desc = RelationGetDescr(index);
	int			natts = IndexRelationGetNumberOfKeyAttributes(index);
	SiftColDef *defs = palloc0(SIFT_COL_MAX_COLS * sizeof(SiftColDef));
	int			n = 0;

	if (index->rd_index->indnatts != natts)
		ereport(ERROR, (errcode(ERRCODE_FEATURE_NOT_SUPPORTED), errmsg("sift_col indexes do not support INCLUDE columns")));
	r->natts = natts;
	r->first = palloc((natts + 1) * sizeof(int));
	r->nleaves = palloc((natts + 1) * sizeof(int));
	r->json = palloc0((natts + 1) * sizeof(bool));
	r->shred = sift_col_shred_create(index->rd_indexcxt, index);
	for (int a = 1; a <= natts; a++)
	{
		Oid			typid = TupleDescAttr(desc, a - 1)->atttypid;
		SiftColStore store = SIFT_COLS_INT;
		uint8		scale = typid == NUMERICOID ? numeric_scale_of(TupleDescAttr(desc, a - 1)) : 0;
		int			k = typid == JSONBOID ? (r->shred ? sift_col_shred_nleaves(r->shred, a) : 0) : 1;

		if (typid != JSONBOID && !sift_col_leaf_store(typid, &store))
			ereport(ERROR,
					(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
					 errmsg("sift_col does not support the type %s of column \"%s\"", format_type_be(typid),
							NameStr(TupleDescAttr(desc, a - 1)->attname)),
					 errhint("Supported: smallint, integer, bigint, boolean, date, timestamp, timestamptz, numeric(p, s) with p <= 18, real, double precision, text, varchar and jsonb with paths.")));
		if (typid == JSONBOID && k == 0)
			ereport(ERROR,
					(errcode(ERRCODE_INVALID_PARAMETER_VALUE),
					 errmsg("the jsonb column \"%s\" of a sift_col index needs paths", NameStr(TupleDescAttr(desc, a - 1)->attname)),
					 errhint("Declare them with the paths option: WITH (paths = '%s.* float4')", NameStr(TupleDescAttr(desc, a - 1)->attname))));
		if (n + k > SIFT_COL_MAX_COLS)
			ereport(ERROR, (errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED), errmsg("a sift_col index holds at most %d columns", SIFT_COL_MAX_COLS)));
		r->first[a] = n;
		r->json[a] = typid == JSONBOID;
		r->nleaves[a] = k;
		if (typid != JSONBOID)
			defs[n] = (SiftColDef) {.id = n, .parent = -1, .kind = SIFT_COLK_SCALAR, .store = store, .attno = a, .typid = typid, .path = "", .scale = scale};
		else
			for (int i = 0; i < k; i++)
			{
				SiftColDef *d = &defs[n + i];

				sift_col_shred_leaf(r->shred, a, i, d);
				d->id = n + i;
				d->attno = a;
				d->parent = d->parent < 0 ? -1 : n + d->parent;
			}
		n += k;
	}
	r->schema = sift_cschema_create(index->rd_indexcxt, defs, n);
	r->policy = palloc(n * sizeof(SiftCcPolicy));
	for (int i = 0; i < n; i++)
	{
		const char *name = defs[i].path[0] ? defs[i].path : NameStr(TupleDescAttr(desc, defs[i].attno - 1)->attname);
		const char *err = sift_cc_policy_for_column(sift_col_options_compression(index), name, &r->policy[i]);

		if (err)
			ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("invalid compression option: %s", err)));
	}
	r->meta = (SiftColIndexMeta) {.indexoid = RelationGetRelid(index), .heapoid = index->rd_index->indrelid, .natts = natts,
	.ncols = n,.cols = r->schema->cols};
	r->rowcxt = AllocSetContextCreate(index->rd_indexcxt, "sift cache col insert", ALLOCSET_DEFAULT_SIZES);
	MemoryContextSwitchTo(old);
	return r;
}

SiftColRel *
sift_col_rel(Relation index)
{
	if (index->rd_amcache == NULL)
		index->rd_amcache = build_rel(index);
	return index->rd_amcache;
}

const SiftColIndexMeta *
sift_col_index_defs(Relation index)
{
	SiftColRel *r = sift_col_rel(index);

	r->meta.valid = index->rd_index->indisvalid && index->rd_index->indisready && index->rd_index->indislive;
	r->meta.partial = !heap_attisnull(index->rd_indextuple, Anum_pg_index_indpred, NULL);
	return &r->meta;
}

Node *
sift_col_att_expr(Relation index, int attno)
{
	AttrNumber	heapatt = index->rd_index->indkey.values[attno - 1];

	if (heapatt != 0)
	{
		Oid			typid = TupleDescAttr(RelationGetDescr(index), attno - 1)->atttypid;
		int32		typmod = TupleDescAttr(RelationGetDescr(index), attno - 1)->atttypmod;

		return (Node *) makeVar(1, heapatt, typid, typmod, TupleDescAttr(RelationGetDescr(index), attno - 1)->attcollation, 0);
	}
	{
		List	   *exprs = RelationGetIndexExpressions(index);
		int			k = 0;

		for (int i = 0; i < attno - 1; i++)
			if (index->rd_index->indkey.values[i] == 0)
				k++;
		return (Node *) copyObject(list_nth(exprs, k));
	}
}

SiftColIndexMeta *
sift_col_index_meta(Relation index)
{
	const SiftColIndexMeta *d = sift_col_index_defs(index);
	SiftColIndexMeta *m = palloc(sizeof(SiftColIndexMeta));
	SiftDirSnapshot *snap = palloc(sizeof(SiftDirSnapshot));
	BlockNumber allvis;
	Relation	heap;
	BlockNumber relpages;

	*m = *d;
	sift_dir_read(index, snap);
	for (int i = 0; i < snap->nsegs; i++)
	{
		const SiftSegEntry *e = &snap->segs[i];

		if (e->kind == SIFT_SEG_IMMUTABLE)
		{
			m->stats.nrows += e->ndocs;
			m->stats.bytes += e->bytes;
			m->stats.nsegments++;
		}
		else
			m->stats.nmutable++;
	}
	m->stats.nunits = sift_cseg_dir_units(snap);
	heap = table_open(d->heapoid, AccessShareLock);
	relpages = RelationGetNumberOfBlocks(heap);
	visibilitymap_count(heap, &allvis, NULL);
	m->stats.heap_pages = relpages;
	m->stats.allvisible_frac = relpages > 0 ? (double) allvis / relpages : 0;
	table_close(heap, AccessShareLock);
	pfree(snap);
	return m;
}

bool
sift_col_is_index(Relation index)
{
	return index->rd_rel->relam == sift_col_am_oid();
}

Oid
sift_col_am_oid(void)
{
	static Oid	am = InvalidOid;

	if (!OidIsValid(am))
		am = get_index_am_oid("sift_col", true);
	return am;
}

/* ---------- numeric ---------- */

/* 10^scale as a numeric, for the lifetime of the backend */
static Datum
numeric_pow10(int scale)
{
	static Datum pow10[SIFT_COL_MAX_SCALE + 1];

	if (pow10[scale] == (Datum) 0)
	{
		MemoryContext old = MemoryContextSwitchTo(TopMemoryContext);
		int64		p = 1;

		for (int i = 0; i < scale; i++)
			p *= 10;
		pow10[scale] = NumericGetDatum(int64_to_numeric(p));
		MemoryContextSwitchTo(old);
	}
	return pow10[scale];
}

int64
sift_col_numeric_key(int scale, Datum value, SiftColRound round)
{
	Datum		scaled = DirectFunctionCall2(numeric_mul, value, numeric_pow10(scale));

	if (round == SIFT_COL_ROUND_DOWN)
		scaled = DirectFunctionCall1(numeric_floor, scaled);
	else if (round == SIFT_COL_ROUND_UP)
		scaled = DirectFunctionCall1(numeric_ceil, scaled);
	return DatumGetInt64(DirectFunctionCall1(numeric_int8, scaled));
}

Datum
sift_col_key_numeric(int scale, int64 key)
{
	return NumericGetDatum(int64_div_fast_to_numeric(key, scale));
}

/* ---------- tuple to row ---------- */

static void
datum_value(const SiftColDef *def, Datum d, SiftColValue *v)
{
	v->isnull = v->overflow = false;
	v->len = 0;
	switch (def->typid)
	{
		case INT2OID:
			v->v.i = DatumGetInt16(d);
			break;
		case INT4OID:
		case DATEOID:
			v->v.i = DatumGetInt32(d);
			break;
		case INT8OID:
		case TIMESTAMPOID:
		case TIMESTAMPTZOID:
			v->v.i = DatumGetInt64(d);
			break;
		case BOOLOID:
			v->v.i = DatumGetBool(d);
			break;
		case NUMERICOID:
			if (numeric_is_nan(DatumGetNumeric(d)))
				v->isnull = v->overflow = true;
			else
				v->v.i = sift_col_numeric_key(def->scale, d, SIFT_COL_ROUND_NEAREST);
			break;
		case FLOAT4OID:
			v->v.f = DatumGetFloat4(d);
			break;
		case FLOAT8OID:
			v->v.f = DatumGetFloat8(d);
			break;
		default:
			{
				text	   *t = DatumGetTextPP(d);

				v->v.s = VARDATA_ANY(t);
				v->len = VARSIZE_ANY_EXHDR(t);
				if (v->len > SIFT_COL_MAX_STR)
					ereport(ERROR,
							(errcode(ERRCODE_PROGRAM_LIMIT_EXCEEDED),
							 errmsg("value of %u bytes is too long for a sift_col index (at most %d bytes)", v->len, SIFT_COL_MAX_STR),
							 errhint("Index left(column, %d) or a hash of it instead.", SIFT_COL_MAX_STR)));
			}
	}
}

void
sift_col_make_row(SiftColRel *r, ItemPointer tid, const Datum *values, const bool *isnull, SiftColRow *row, MemoryContext mcxt)
{
	MemoryContext old = MemoryContextSwitchTo(mcxt);
	int			ncols = r->schema->ncols;

	row->tid = *tid;
	row->ncols = ncols;
	row->val = palloc0(ncols * sizeof(SiftColValue));
	row->elem = palloc0(ncols * sizeof(SiftColValue *));
	for (int a = 1; a <= r->natts; a++)
	{
		const SiftColDef *def = &r->schema->cols[r->first[a]];

		if (r->json[a])
			continue;
		if (isnull[a - 1])
			row->val[def->id].isnull = true;
		else
			datum_value(def, values[a - 1], &row->val[def->id]);
	}
	if (r->shred)
	{
		int		   *first = r->first;

		sift_col_shred_row(r->shred, values, isnull, row, first, mcxt);
	}
	MemoryContextSwitchTo(old);
}

/* ---------- aminsert ---------- */

bool
sift_col_aminsert(Relation index, Datum *values, bool *isnull, ItemPointer heap_tid, Relation heap,
				  IndexUniqueCheck checkUnique, bool indexUnchanged, IndexInfo *indexInfo)
{
	SiftColRel *r = sift_col_rel(index);
	SiftColRow	row;
	bool		crossed;

	sift_col_make_row(r, heap_tid, values, isnull, &row, r->rowcxt);
	crossed = sift_cmut_append(index, &r->hint, r->schema, &row);
	MemoryContextReset(r->rowcxt);
	if (crossed)
		sift_col_maint_after_seal(index);
	return false;
}

/* ---------- the rest of the AM ---------- */

static bool
col_amvalidate(Oid opclassoid)
{
	return true;
}

static void
col_amcostestimate(PlannerInfo *root, IndexPath *path, double loop_count, Cost *indexStartupCost,
						Cost *indexTotalCost, Selectivity *indexSelectivity, double *indexCorrelation, double *indexPages)
{
	/* the operator class has no operators, so no index path exists; the column scans use the index through sift_col.h */
	*indexStartupCost = *indexTotalCost = 1.0e30;
	*indexSelectivity = 1.0;
	*indexCorrelation = 0.0;
	*indexPages = 1.0;
}

static IndexScanDesc
col_ambeginscan(Relation index, int nkeys, int norderbys)
{
	ereport(ERROR,
			(errcode(ERRCODE_FEATURE_NOT_SUPPORTED),
			 errmsg("sift_col indexes cannot be scanned as ordinary indexes"),
			 errhint("They serve the column scans of the planner, not index scans.")));
	return NULL;
}

static void
col_amrescan(IndexScanDesc scan, ScanKey keys, int nkeys, ScanKey orderbys, int norderbys)
{
}

static void
col_amendscan(IndexScanDesc scan)
{
}

Datum
sift_col_handler(PG_FUNCTION_ARGS)
{
	IndexAmRoutine *am = makeNode(IndexAmRoutine);

	am->amstrategies = 0;
	am->amsupport = 1;
	am->amoptsprocnum = 0;
	am->amcanorder = false;
	am->amcanorderbyop = false;
	am->amcanbackward = false;
	am->amcanunique = false;
	am->amcanmulticol = true;
	am->amoptionalkey = true;
	am->amsearcharray = false;
	am->amsearchnulls = false;
	am->amstorage = false;
	am->amclusterable = false;
	am->ampredlocks = false;
	am->amcanparallel = false;
	am->amcanbuildparallel = true;
	am->amcaninclude = false;
	am->amusemaintenanceworkmem = true;
	am->amsummarizing = false;
	am->amparallelvacuumoptions = VACUUM_OPTION_PARALLEL_BULKDEL;
	am->amkeytype = InvalidOid;

	am->ambuild = sift_col_ambuild;
	am->ambuildempty = sift_col_ambuildempty;
	am->aminsert = sift_col_aminsert;
	am->aminsertcleanup = NULL;
	am->ambulkdelete = sift_col_ambulkdelete;
	am->amvacuumcleanup = sift_col_amvacuumcleanup;
	am->amcanreturn = NULL;
	am->amcostestimate = col_amcostestimate;
	am->amoptions = col_amoptions;
	am->ambuildphasename = sift_col_ambuildphasename;
	am->amvalidate = col_amvalidate;
	am->amadjustmembers = NULL;
	am->ambeginscan = col_ambeginscan;
	am->amrescan = col_amrescan;
	am->amgettuple = NULL;
	am->amgetbitmap = NULL;
	am->amendscan = col_amendscan;
	am->ammarkpos = NULL;
	am->amrestrpos = NULL;
	am->amestimateparallelscan = NULL;
	am->aminitparallelscan = NULL;
	am->amparallelrescan = NULL;
	SIFT_AM_NEW_FIELDS(am);

	PG_RETURN_POINTER(am);
}
