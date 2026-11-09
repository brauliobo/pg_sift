/*
 * agg/col_conn.c - what the column source says of a connected index to the executor (SiftSrcProvider.conn_proof)
 */
#include "postgres.h"

#if __has_include("col/col.h")

#include "col/col.h"
#include "col/conn.h"

#include "agg/col_int.h"

const char *
sift_col_conn_proof(void *impl, int leaf, uint64 *lineage, uint32 *xmin)
{
	SiftConnStamp st;
	const char *why = sift_col_source_proof(((ColSource *) impl)->cs, leaf, &st);

	*lineage = st.lineage;
	*xmin = st.xmin;
	return why;
}

#endif							/* __has_include("col/col.h") */
