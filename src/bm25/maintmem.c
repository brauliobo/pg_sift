/*
 * bm25/maintmem.c - the memory a maintenance step of a BM25 index may take (bm25/maintmem.h)
 */
#include "postgres.h"

#include "miscadmin.h"
#include "postmaster/autovacuum.h"

#include "bm25/maintmem.h"

Size
sift_bm25_maint_bytes(int nparticipants)
{
	Size		raw;

	if (!AmAutoVacuumWorkerProcess() || autovacuum_work_mem == -1)
		return sift_mem_budget_bytes(SIFT_BUDGET_MAINT_MEM, nparticipants, 1);
	raw = (Size) autovacuum_work_mem * 1024;
	return Max(raw / Max(nparticipants, 1), Min(raw, SIFT_MEM_MIN_MAINT_BYTES));
}
