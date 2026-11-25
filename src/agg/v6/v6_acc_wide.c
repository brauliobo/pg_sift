/*
 * agg/v6/v6_acc_wide.c - what a sum of a derived numeric (agg/v6/v6_acc_wide.h) says when it cannot go on.
 */
#include "postgres.h"

#include "agg/v6/v6_acc_wide.h"

void
sift_v6_wide_overflow(void)
{
	ereport(ERROR, (errcode(ERRCODE_NUMERIC_VALUE_OUT_OF_RANGE), errmsg("a sum of a numeric expression is out of the range the aggregate scan computes"),
					errhint("Run the statement with sift.enable_agg = off.")));
}
