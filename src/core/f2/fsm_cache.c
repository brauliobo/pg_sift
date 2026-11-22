/*
 * core/f2/fsm_cache.c - the free blocks a backend holds for a relation file go with the file (core/f2/fsm.h).
 */
#include "core/host.h"

#include "col/f2/part_format.h"
#include "core/f2/fsm.h"

/* MEM[MEM-CORE-08]: backend BSS FSM extent cache (fixed small ring; COW clean idle) */
void
sift_fsm_cache_discard_rel(Relation rel)
{
	sift_part_discard_preferred(&rel->rd_locator);
}
