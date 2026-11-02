/*
 * col/rowkey.c - the option track_updates of a heap column index: the column that identifies a row, the rule that makes the index able to say which older entry
 * an updated row replaces. The checks run at CREATE INDEX and at ALTER INDEX; 'auto' is the primary key and is written back as the column.
 */
#include "postgres.h"

#include "access/htup_details.h"
#include "access/table.h"
#include "catalog/pg_attribute.h"
#include "catalog/pg_index.h"
#include "utils/lsyscache.h"
#include "utils/rel.h"
#include "utils/syscache.h"

#include "col/am.h"

bool
sift_col_row_key_on(Relation index)
{
	const char *rk = sift_col_options_row_key(index);

	return rk[0] != '\0' && strcmp(rk, "off") != 0 && strcmp(rk, "auto") != 0;
}

/* A valid, immediate unique index of the table on exactly this column, not partial, not on an expression */
static bool
unique_on(Relation heap, AttrNumber att, bool primary_only, AttrNumber *pk)
{
	List	   *oids = RelationGetIndexList(heap);
	bool		found = false;

	foreach_oid(ioid, oids)
	{
		HeapTuple	tp = SearchSysCache1(INDEXRELID, ObjectIdGetDatum(ioid));
		Form_pg_index ix;

		if (!HeapTupleIsValid(tp))
			continue;
		ix = (Form_pg_index) GETSTRUCT(tp);
		if (ix->indisvalid && ix->indisunique && ix->indimmediate && ix->indnkeyatts == 1 && ix->indnatts == 1 && ix->indkey.values[0] != 0 &&
			heap_attisnull(tp, Anum_pg_index_indpred, NULL) && (!primary_only || ix->indisprimary))
		{
			if (pk != NULL && ix->indisprimary)
				*pk = ix->indkey.values[0];
			found |= att == InvalidAttrNumber || ix->indkey.values[0] == att;
		}
		ReleaseSysCache(tp);
	}
	list_free(oids);
	return found;
}

/* The primary key of the table when it is one column, else InvalidAttrNumber */
static AttrNumber
primary_key_att(Relation heap)
{
	AttrNumber	pk = InvalidAttrNumber;

	(void) unique_on(heap, InvalidAttrNumber, true, &pk);
	return pk;
}

static void
check_column(Relation heap, Relation index, const char *name)
{
	AttrNumber	att = get_attnum(RelationGetRelid(heap), name);
	bool		covered = false;

	if (att == InvalidAttrNumber)
		ereport(ERROR, (errcode(ERRCODE_UNDEFINED_COLUMN), errmsg("track_updates: table \"%s\" has no column \"%s\"", RelationGetRelationName(heap), name)));
	for (int i = 0; i < IndexRelationGetNumberOfKeyAttributes(index); i++)
		covered |= index->rd_index->indkey.values[i] == att;
	if (!covered)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("track_updates: column \"%s\" is not a plain column of the index", name),
						errhint("Name a column the index holds, or add it to the column list of the index.")));
	if (!TupleDescAttr(RelationGetDescr(heap), att - 1)->attnotnull)
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("track_updates: column \"%s\" can be NULL", name),
						errhint("A row is found by its key: declare the column NOT NULL, or use its primary key.")));
	if (!unique_on(heap, att, false, NULL))
		ereport(ERROR, (errcode(ERRCODE_INVALID_PARAMETER_VALUE), errmsg("track_updates: column \"%s\" has no unique index of its own", name),
						errdetail("The index replaces the older entries of a key when the row is updated, which is exact only when no two live rows share the key."),
						errhint("Create a UNIQUE or PRIMARY KEY constraint on the column alone: not deferrable, not partial, not on an expression, not on several columns.")));
}

/* ALTER INDEX and CREATE INDEX: the option names a column the index can use */
void
sift_col_row_key_check(Relation index)
{
	Relation	heap;

	if (!sift_col_row_key_on(index))
		return;
	heap = table_open(index->rd_index->indrelid, AccessShareLock);
	check_column(heap, index, sift_col_options_row_key(index));
	table_close(heap, NoLock);
}

/*
 * Before a build: 'auto' becomes the primary key (or off, with a NOTICE, when the table has none of one column), the column is checked, and the point directory of
 * heap addresses that finds its entries lists it (the option point_tid_columns gets the column when it does not name it).
 */
void
sift_col_row_key_prepare(Relation heap, Relation index)
{
	const char *rk = sift_col_options_row_key(index);
	const char *tid;
	char	   *list;
	StringInfoData v;
	bool		named = false;

	if (rk[0] == '\0' || strcmp(rk, "off") == 0)
		return;
	if (strcmp(rk, "auto") == 0)
	{
		AttrNumber	pk = primary_key_att(heap);

		if (pk == InvalidAttrNumber)
		{
			ereport(NOTICE, (errmsg("track_updates = auto: table \"%s\" has no primary key of one column, so the index keeps no row key", RelationGetRelationName(heap)),
							 errhint("Name a column that has a unique index of its own: WITH (track_updates = 'id').")));
			sift_col_set_option(index, "track_updates", "off");
			return;
		}
		sift_col_set_option(index, "track_updates", get_attname(RelationGetRelid(heap), pk, false));
		rk = sift_col_options_row_key(index);
	}
	check_column(heap, index, rk);
	tid = sift_col_options_point_tid_columns(index);
	list = pstrdup(tid);
	for (char *tok = strtok(list, ", "); tok != NULL; tok = strtok(NULL, ", "))
		named |= strcmp(tok, rk) == 0;
	if (named)
		return;
	initStringInfo(&v);
	/* an empty or automatic choice becomes the key alone: the key is what the supersede reads; the user's own list keeps its columns and gains it */
	if (tid[0] == '\0' || strncmp(tid, "auto", 4) == 0)
		appendStringInfoString(&v, rk);
	else
		appendStringInfo(&v, "%s, %s", tid, rk);
	sift_col_set_option(index, "point_tid_columns", v.data);
}
