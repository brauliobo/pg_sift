/*
 * core/store/ops.c - multi-format SiftStoreOps registry (Spec 86).
 */
#include "postgres.h"

#include "core/catalog.h"
#include "core/store/ops.h"

static const SiftStoreOps *registered_ops[SIFT_STORE_OPS_MAX];
static int	nregistered_ops = 0;

void
sift_store_ops_register(const SiftStoreOps *ops)
{
	SIFT_INVARIANT(ops != NULL && ops->name != NULL && ops->owns_rel != NULL &&
				   ops->schema != NULL && ops->stats != NULL &&
				   ops->open != NULL && ops->leaf_holds != NULL,
				   "sift: incomplete SiftStoreOps registration");
	for (int i = 0; i < nregistered_ops; i++)
		if (registered_ops[i] == ops)
			return;
	SIFT_INVARIANT(nregistered_ops < SIFT_STORE_OPS_MAX,
				   "sift: too many SiftStoreOps providers");
	registered_ops[nregistered_ops++] = ops;
}

void
sift_store_ops_unregister(const SiftStoreOps *ops)
{
	for (int i = 0; i < nregistered_ops; i++)
	{
		if (registered_ops[i] == ops)
		{
			for (int j = i + 1; j < nregistered_ops; j++)
				registered_ops[j - 1] = registered_ops[j];
			registered_ops[--nregistered_ops] = NULL;
			return;
		}
	}
}

const SiftStoreOps *
sift_store_ops_for_oid(Oid relid, bool is_table)
{
	if (!OidIsValid(relid))
		return NULL;
	if (!sift_store_ops_may_own(sift_relid_relam(relid), is_table))
	{
		if (!is_table)
			SIFT_NOTE_FOREIGN_PROBE();
		return NULL;
	}
	for (int i = 0; i < nregistered_ops; i++)
		if (registered_ops[i]->owns_rel(relid, is_table))
			return registered_ops[i];
	return NULL;
}

bool
sift_store_ops_may_own(Oid relam, bool is_table)
{
	const SiftCatalog *cat = sift_catalog();

	if (relam == (is_table ? cat->tbl_am : cat->col_am) && relam != InvalidOid)
		return true;
	for (int i = 0; i < nregistered_ops; i++)
		if (registered_ops[i]->any_am)
			return true;
	return false;
}

bool
sift_store_ops_any(void)
{
	return nregistered_ops > 0;
}

int
sift_store_ops_count(void)
{
	return nregistered_ops;
}

const SiftStoreOps *
sift_store_ops_at(int i)
{
	if (i < 0 || i >= nregistered_ops)
		return NULL;
	return registered_ops[i];
}
