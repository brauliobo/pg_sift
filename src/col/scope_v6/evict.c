/*
 * col/scope_v6/evict.c - keeps the accumulators of a summary build within its memory budget: the key with the largest open level-1 node is flushed,
 * then the keys touched longest ago write the month cubes of level 2 out as bytes and read them back when the object is laid out.
 */
/* check-memory: allow-allocs the arrays follow the keys of the pair map and the month cubes of one build, which the budget bounds */
#include "core/host.h"

#include "col/scope_v6/build_int.h"

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

/* Brings the bytes the build counts for a key, the heap of open nodes and the list of keys with month cubes in memory up to what the key holds now */
void
sift_sbld_account(SiftScopeBuild *b, KeyAcc *k)
{
	Size		base = sizeof(*k) + (Size) b->nrings * sizeof(SiftTopKHeap) + sift_scube_bytes(&k->l2_all),
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

/* The month cubes of level 2 of a key that went out of memory come back into its table */
void
sift_sbld_unspill(SiftScopeBuild *b, KeyAcc *k)
{
	uint32		n;
	const char *dir,
			   *cube_at;

	if (k->spill == NULL)
		return;
	memcpy(&n, k->spill, sizeof(n));
	dir = k->spill + sizeof(n);
	cube_at = dir + (Size) n * 2 * sizeof(uint32);
	for (uint32 i = 0; i < n; i++)
	{
		int32		month;
		uint32		len;
		SiftScopeCubeAcc cube;

		memcpy(&month, dir + (Size) i * 8, sizeof(month));
		memcpy(&len, dir + (Size) i * 8 + 4, sizeof(len));
		SIFT_INVARIANT(sift_scube_read(cube_at, len, &cube), "scope summary: a damaged spilled month cube");
		sift_scube_merge(sift_smonth_get(&k->l2_months, month, b->spec.shape), &cube, NULL);
		sift_scube_free(&cube);
		cube_at += len;
	}
	pfree(k->spill);
	k->spill = NULL;
}

/* The month cubes of level 2 of a key as bytes, and out of the accumulators: the bytes are the output's size, not the accumulator's */
static void
spill_l2(SiftScopeBuild *b, KeyAcc *k)
{
	StringInfoData cubes;
	uint32		n,
			   *dir;
	char	   *blob;
	Size		len;

	sift_sbld_unspill(b, k);
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
	blob = palloc(len);
	memcpy(blob, &n, sizeof(n));
	memcpy(blob + sizeof(n), dir, (Size) n * 2 * sizeof(uint32));
	memcpy(blob + sizeof(n) + (Size) n * 2 * sizeof(uint32), cubes.data, cubes.len);
	k->spill = blob;
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
	while (b->live > b->budget / 4 * 3)
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
