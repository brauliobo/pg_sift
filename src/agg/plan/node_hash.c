/*
 * agg/plan/node_hash.c - the hash of node trees that agrees with equal() (node_hash.h).
 */
#include "postgres.h"

#include "common/hashfn.h"
#include "nodes/primnodes.h"
#include "nodes/value.h"
#include "utils/datum.h"

#include "agg/plan/node_hash.h"

/* a hash of a node from the fields equal() compares (not the ones it skips: locations, the function of an operator that may be set or not), so that equal nodes share it; what it leaves out only costs a longer chain */
uint64
sift_node_hash(const Node *n)
{
	uint64		h;

	if (n == NULL)
		return 0;
	h = nodeTag(n);
	switch (nodeTag(n))
	{
		case T_Const:
			{
				const Const *c = (const Const *) n;

				h = hash_combine64(h, c->consttype);
				if (!c->constisnull)
					h = hash_combine64(h, c->constbyval ? (uint64) c->constvalue :
									   hash_bytes((const unsigned char *) DatumGetPointer(c->constvalue), datumGetSize(c->constvalue, false, c->constlen)));
				return h;
			}
		case T_Param:
			return hash_combine64(hash_combine64(h, ((const Param *) n)->paramkind), ((const Param *) n)->paramid);
		case T_Var:
			return hash_combine64(hash_combine64(h, ((const Var *) n)->varno), ((const Var *) n)->varattno);
		case T_String:
			return hash_combine64(h, hash_bytes((const unsigned char *) ((const String *) n)->sval, strlen(((const String *) n)->sval)));
		case T_OpExpr:
			return hash_combine64(hash_combine64(h, ((const OpExpr *) n)->opno), sift_node_hash((const Node *) ((const OpExpr *) n)->args));
		case T_FuncExpr:
			return hash_combine64(hash_combine64(h, ((const FuncExpr *) n)->funcid), sift_node_hash((const Node *) ((const FuncExpr *) n)->args));
		case T_BoolExpr:
			return hash_combine64(hash_combine64(h, ((const BoolExpr *) n)->boolop), sift_node_hash((const Node *) ((const BoolExpr *) n)->args));
		case T_List:
			return hash_combine64(h, sift_list_hash((const List *) n));
		default:
			return h;
	}
}

uint64
sift_list_hash(const List *l)
{
	uint64		h = list_length(l);

	foreach_ptr(Node, n, l)
		h = hash_combine64(h, sift_node_hash(n));
	return h;
}
