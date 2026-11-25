/*
 * col/f2/cube/evict.c - keeps the accumulators of a summary build within its memory budget: the key with the largest open level-1 node is flushed,
 * then the keys touched longest ago write the month cubes of level 2 out as bytes and read them back when the object is laid out.
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

/* MEM[MEM-SCOPE-10]: pools, nodes, slices, dict, spills added to the build budget (was 0 B) */
static Size
build_overhead(const SiftScopeBuild *b)
{
	return (Size) b->l0pool.maxlen + b->l1pool.maxlen + (Size) b->capnodes * sizeof(NodeRec) + (Size) b->capslices * sizeof(*b->slices) +
		(Size) b->dict.cap * (sizeof(char *) + sizeof(uint32)) + (Size) b->dict.nslot * sizeof(uint32) + b->stats.l2_spill_bytes;
}

/* Brings the bytes the build counts for a key, the heap of open nodes and the list of keys with month cubes in memory up to what the key holds now */
void
sift_sbld_account(SiftScopeBuild *b, KeyAcc *k)
{
	Size		base = sizeof(*k) + ring_bytes(b, k) + sift_scube_bytes(&k->l2_all),
				open = sift_scube_bytes(&k->l1_all) + sift_smonth_bytes(&k->l1_months) + (Size) k->capmembers * sizeof(int64),
				l2m = sift_smonth_bytes(&k->l2_months);

	b->live = b->live + base + open + l2m - k->est_base - k->est_open - k->est_l2m;
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

/* The month cubes of level 2 of a key that went out of memory come back into its table, a spill at a time (they are merged, so the order does not matter) */
void
sift_sbld_unspill(SiftScopeBuild *b, KeyAcc *k)
{
	while (k->spill != NULL)
	{
		KeySpill   *sp = k->spill;
		uint32		n;
		const char *dir = sp->blob + sizeof(n),
				   *cube_at;

		memcpy(&n, sp->blob, sizeof(n));
		cube_at = dir + (Size) n * 2 * sizeof(uint32);
		for (uint32 i = 0; i < n; i++)
		{
			int32		month;
			uint32		len;
			SiftScopeCubeAcc cube;

			memcpy(&month, dir + (Size) i * 8, sizeof(month));
			memcpy(&len, dir + (Size) i * 8 + 4, sizeof(len));
			SIFT_INVARIANT(sift_scube_read(cube_at, len, &cube), "scope summary: a damaged spilled month cube");
			sift_smonth_merge_cube(&k->l2_months, b->spec.shape, month, &cube, NULL);
			sift_scube_free(&cube);
			cube_at += len;
		}
		k->spill = sp->next;
		pfree(sp->blob);
		pfree(sp);
	}
}

/* The month cubes of level 2 in memory as bytes, one more spill of the key, and out of the accumulators: the bytes are the output's size, not the accumulator's */
static void
spill_l2(SiftScopeBuild *b, KeyAcc *k)
{
	StringInfoData cubes;
	uint32		n,
			   *dir;
	KeySpill   *sp = palloc(sizeof(*sp));
	Size		len;

	sift_smonth_sort(&k->l2_months);
	n = k->l2_months.n;
	initStringInfo(&cubes);
	dir = palloc(sizeof(uint32) * 2 * Max(n, 1));
	for (uint32 i = 0; i < n; i++)
	{
		uint32		at = cubes.len;

		sift_scube_write(&cubes, &k->l2_months.e[i].cube);
		dir[2 * i] = (uint32) k->l2_months.e[i].month_id;
		dir[2 * i + 1] = cubes.len - at;
	}
	len = sizeof(n) + (Size) n * 2 * sizeof(uint32) + cubes.len;
	sp->blob = palloc(len);
	memcpy(sp->blob, &n, sizeof(n));
	memcpy(sp->blob + sizeof(n), dir, (Size) n * 2 * sizeof(uint32));
	memcpy(sp->blob + sizeof(n) + (Size) n * 2 * sizeof(uint32), cubes.data, cubes.len);
	sp->next = k->spill;
	k->spill = sp;
	b->stats.l2_spills++;
	b->stats.l2_spill_bytes += len;
	pfree(dir);
	pfree(cubes.data);
	sift_smonth_free(&k->l2_months);
	sift_sbld_account(b, k);
}

/*
 * Brings the accumulators back under the budget: the key with the largest open node is flushed until they hold three quarters of it, then the
 * keys touched longest ago write their month cubes of level 2 out until they hold 85 hundredths. What is left is what each active key holds
 * at least: its cubes, its ring and its level-2 all-time cube.
 */
void
sift_sbld_evict_if_over(SiftScopeBuild *b)
{
	while (b->live + build_overhead(b) > b->budget / 4 * 3)
	{
		uint32		ord = sift_mheap_top(&b->open);

		if (ord == UINT32_MAX)
			break;
		sift_sbld_flush_l1(b, b->acc[ord], true);
		sift_sbld_account(b, b->acc[ord]);
		b->stats.evictions++;
	}
	while (b->live + build_overhead(b) > b->budget / 100 * 85 && b->lru_tail != NULL)
		spill_l2(b, b->lru_tail);
	b->stats.peak_live = Max(b->stats.peak_live, b->live);
}
