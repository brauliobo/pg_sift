/*
 * col/scope_rows.c - the rows of the relations of a scope for the price of a read (col/scope.h)
 */
#include "postgres.h"

#include "access/table.h"
#include "access/xact.h"
#include "storage/bufmgr.h"
#include "storage/sinval.h"
#include "utils/rel.h"

#include "col/am.h"
#include "col/conn.h"
#include "col/scope.h"

/*
 * The rows of the relations of a scope, and of those whose pairs only their btree lists, for the price of a read: the planner's statistics of each (its pages now, the density of its last
 * ANALYZE). Kept for the catalog state (SharedInvalidMessageCounter) and the pages, not in a transaction with an xid (its own DDL is not counted before it commits): finding the relations by name costs more.
 */
#define ROWS_MEMO_N 8
static struct RowsMemo
{
	Oid			index;
	int			ord;
	uint64		gen;
	int			nrel;
	Oid			rel[3];
	BlockNumber pages[3];
	double		rows,
				scanned;
} rows_memo[ROWS_MEMO_N];
static int	rows_memo_next;

static bool
rows_memo_fresh(const struct RowsMemo *m)
{
	for (int i = 0; i < m->nrel; i++)
	{
		Relation	rel = RelationIdGetRelation(m->rel[i]);
		bool		same = rel != NULL && RelationGetNumberOfBlocks(rel) == m->pages[i];

		if (rel != NULL)
			RelationClose(rel);
		if (!same)
			return false;
	}
	return true;
}

double
sift_scope_relations_rows(Relation heap, Relation index, const char *key_name, double *scanned)
{
	const SiftScopeSpecs *sp = sift_scope_specs(index);
	int			ord = 0;
	const SiftScopeSpec *s;
	bool		keep = !TransactionIdIsValid(GetTopTransactionIdIfAny());
	struct RowsMemo m;

	while (strcmp(sp->s[ord]->key_name, key_name) != 0)
		ord++;
	s = sp->s[ord];
	m = (struct RowsMemo) {RelationGetRelid(index), ord, SharedInvalidMessageCounter};
	for (int i = 0; keep && i < ROWS_MEMO_N; i++)
		if (rows_memo[i].index == m.index && rows_memo[i].ord == ord && rows_memo[i].gen == m.gen && rows_memo_fresh(&rows_memo[i]))
		{
			sift_catalog_memo_hits++;
			*scanned = rows_memo[i].scanned;
			return rows_memo[i].rows;
		}
	for (int hop = 0; hop <= (s->joined != NULL ? 2 : 0); hop++)
	{
		Relation	rel;
		AttrNumber	a,
					b;
		Oid			btree = hop == 0 ? sift_scope_relation_index(heap, index, s, &rel, &a, &b) : sift_scope_hop_index(heap, index, s, hop, &rel, &a, &b);
		double		n = sift_conn_rows_of(rel);

		m.rows += n;
		m.scanned += OidIsValid(hop == 2 ? sift_scope_pairs_journal(rel, b, a, btree) : sift_scope_pairs_journal(rel, a, b, btree)) ? 0 : n;
		m.rel[m.nrel] = RelationGetRelid(rel);
		m.pages[m.nrel++] = RelationGetNumberOfBlocks(rel);
		table_close(rel, NoLock);
	}
	if (keep)
		rows_memo[rows_memo_next++ % ROWS_MEMO_N] = m;
	*scanned = m.scanned;
	return m.rows;
}
