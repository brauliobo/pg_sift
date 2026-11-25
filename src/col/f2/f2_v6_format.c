/*
 * col/f2/f2_v6_format.c - the registration of the SiftStoreOps of f2_store_ops.c with the others (core/store_ops.h). The relation is told from its oid: the attributes of a table are its columns, those of an index its columns.
 */
#include "postgres.h"

#include "col/f2/f2_store_ops_int.h"
#include "col/f2/f2_v6_format.h"

void
sift_f2_v6_format_init(void)
{
	sift_store_ops_register(&sift_f2_store_ops);
}
