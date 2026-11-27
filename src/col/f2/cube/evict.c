/*
 * col/f2/cube/evict.c - keeps the accumulators of a summary build within its memory budget: the key with the largest open level-1 node is flushed,
 * then the keys touched longest ago write the month cubes of level 2 to a temporary file and read them back when the object is laid out.
 */
/* check-memory: allow-allocs the arrays follow the keys of the pair map and the month cubes of one build, which the budget bounds */
#include "core/host.h"

#include "col/f2/cube/build_int.h"

static void
lru_remove(SiftScopeBuild *b, KeyAcc *k)
{
	if (!k->in_lru)
		return;
	if (k->lru_prev != NULL)
		k->lru_prev->lru_next = k->lru_next;
	else
		b->lru_head = k->lru_next;
	if (k->lru_next != NULL)
		k->lru_next->lru_prev = k->lru_prev;
	else
		b->lru_tail = k->lru_prev;
	k->lru_prev = k->lru_next = NULL;
	k->in_lru = false;
}

void
sift_sbld_lru_front(SiftScopeBuild *b, KeyAcc *k)
{
	lru_remove(b, k);
	k->lru_next = b->lru_head;
	if (b->lru_head != NULL)
		b->lru_head->lru_prev = k;
	b->lru_head = k;
	if (b->lru_tail == NULL)
		b->lru_tail = k;
	k->in_lru = true;
}

/* The rings and the month map of a key: what the allocator holds for them, grown with the key's rows */
static Size
ring_bytes(const SiftScopeBuild *b, const KeyAcc *k)
{
	Size		n = (Size) b->nrings * sizeof(SiftTopKHeap);

	for (uint32 q = 0; q < b->nrings; q++)
		n += (Size) k->ring[q].cap * sizeof(SiftScopeNewest);
	n += (Size) k->mmap.cap * sizeof(SiftMonthMapEntry) + (Size) k->mmap.ht_cap * sizeof(uint16);
	for (uint32 i = 0; i < k->mmap.n; i++)
		n += (Size) k->mmap.e[i].cap * sizeof(uint32);
	return n;
}

/* Brings the bytes the build counts for a key, the heap of open nodes and the list of keys with month cubes in memory up to what the key holds now */
void
sift_sbld_account(SiftScopeBuild *b, KeyAcc *k)
{
	Size		base = sizeof(*k) + ring_bytes(b, k) + sift_scube_bytes(&k->l2_all),
				open = sift_scube_bytes(&k->l1_all) + sift_smonth_bytes(&k->l1_months) + (Size) k->capmembers * sizeof(int64),
				l2m = sift_smonth_bytes(&k->l2_months);

	b->live = b->live + base + open + l2m - k->est_base - k->est_open - k->est_l2m;
	b->l2m_live = b->l2m_live + l2m - k->est_l2m;
	k->est_base = base;
	k->est_open = open;
	k->est_l2m = l2m;
	b->open_size[k->ord] = open;
	if (k->nrefs > 0)
		sift_mheap_set(&b->open, k->ord);
	else
		sift_mheap_remove(&b->open, k->ord);
	if (k->l2_months.n > 0 && !k->in_lru)
		sift_sbld_lru_front(b, k);
	else if (k->l2_months.n == 0)
		lru_remove(b, k);
}

/* ---------------------------------------------------------------------------
 * 1b. Eviction and spill
 * ------------------------------------------------------------------------- */

/* A spill in the spill file: this head, (month, length) for each month, the cubes */
typedef struct SpillHead
{
	SpillAt		prev;			/* the spill of the key before this one */
	uint32		nmonths;
	uint32		cube_bytes;
} SpillHead;

/* The month cubes of level 2 of a key that went to the spill file come back into its table, a spill at a time (they are merged, so the order does not matter) */
void
sift_sbld_unspill(SiftScopeBuild *b, KeyAcc *k)
{
	while (k->spill.file >= 0)
	{
		SpillHead	h;
		uint32	   *dir;
		char	   *cubes,
				   *cube_at;

		SIFT_INVARIANT(BufFileSeek(b->spills, k->spill.file, k->spill.off, SEEK_SET) == 0, "scope summary: could not seek the spill file");
		BufFileReadExact(b->spills, &h, sizeof(h));
		dir = palloc(sizeof(uint32) * 2 * Max(h.nmonths, 1));
		cubes = cube_at = palloc(Max(h.cube_bytes, 1));
		BufFileReadExact(b->spills, dir, sizeof(uint32) * 2 * h.nmonths);
		BufFileReadExact(b->spills, cubes, h.cube_bytes);
		for (uint32 i = 0; i < h.nmonths; i++)
		{
			SiftScopeCubeAcc cube;

			SIFT_INVARIANT(sift_scube_read(cube_at, dir[2 * i + 1], &cube), "scope summary: a damaged spilled month cube");
			sift_smonth_merge_cube(&k->l2_months, b->spec.shape, (int32) dir[2 * i], &cube, NULL);
			sift_scube_free(&cube);
			cube_at += dir[2 * i + 1];
		}
		k->spill = h.prev;
		pfree(dir);
		pfree(cubes);
	}
}

/* The month cubes of level 2 in memory written to the spill file, one more spill of the key, and out of the accumulators: the bytes are the output's size, not the accumulator's */
static void
spill_l2(SiftScopeBuild *b, KeyAcc *k)
{
	StringInfoData cubes;
	SpillHead	h = {k->spill, k->l2_months.n, 0};
	uint32	   *dir;

	sift_smonth_sort(&k->l2_months);
	initStringInfo(&cubes);
	dir = palloc(sizeof(uint32) * 2 * Max(h.nmonths, 1));
	for (uint32 i = 0; i < h.nmonths; i++)
	{
		uint32		from = cubes.len;

		sift_scube_write(&cubes, &k->l2_months.e[i].cube);
		dir[2 * i] = (uint32) k->l2_months.e[i].month_id;
		dir[2 * i + 1] = cubes.len - from;
	}
	h.cube_bytes = cubes.len;
	if (b->spills == NULL)
		b->spills = BufFileCreateTemp(false);
	BufFileTell(b->spills, &k->spill.file, &k->spill.off);
	BufFileWrite(b->spills, &h, sizeof(h));
	BufFileWrite(b->spills, dir, sizeof(uint32) * 2 * h.nmonths);
	BufFileWrite(b->spills, cubes.data, cubes.len);
	b->stats.l2_spills++;
	b->stats.l2_spill_bytes += sizeof(h) + sizeof(uint32) * 2 * h.nmonths + cubes.len;
	pfree(dir);
	pfree(cubes.data);
	sift_smonth_free(&k->l2_months);
	sift_sbld_account(b, k);
}

/*
 * Brings the accumulators back under the budget: while what they hold besides the month cubes of level 2 is over three quarters of it, the key with
 * the largest open node is flushed (a flush moves the months of the node into those cubes, which a spill gives back, not a flush), then the keys
 * touched longest ago write their month cubes of level 2 to the spill file until the accumulators hold 85 hundredths. What is left is what each
 * active key holds at least: its cubes, its ring and its level-2 all-time cube. The pools, the nodes, the slices and the dictionary are the bytes
 * of the object, which no eviction gives back, so they are not counted.
 */
void
sift_sbld_evict_if_over(SiftScopeBuild *b)
{
	while (b->live - b->l2m_live > b->budget / 4 * 3)
	{
		uint32		ord = sift_mheap_top(&b->open);

		if (ord == UINT32_MAX)
			break;
		sift_sbld_flush_l1(b, b->acc[ord], true);
		sift_sbld_account(b, b->acc[ord]);
		b->stats.evictions++;
	}
	while (b->live > b->budget / 100 * 85 && b->lru_tail != NULL)
		spill_l2(b, b->lru_tail);
	b->stats.peak_live = Max(b->stats.peak_live, b->live);
}
