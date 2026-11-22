/*
 * common_hash.glsl - the lock-free open-addressed hash table of the hash kernels (the program kernel and the fast kernel): a slot is its
 * state word (0 empty, 1 being claimed, the hash with the top bit set when ready), the key words, the NULL mask of the key, the rows of
 * the group and three words of every aggregate; probe_step finds the slot of a key or claims an empty one.
 */
#define READY_BIT	I64_MIN
#define SPIN_MAX	1048576u

int64_t mix64(int64_t xi)
{
	uint64_t x = uint64_t(xi);

	x ^= x >> 30;
	x *= 0xbf58476d1ce4e5b9ul;
	x ^= x >> 27;
	x *= 0x94d049bb133111ebul;
	x ^= x >> 31;
	return int64_t(x);
}

void acc_addf(uint i, double v)
{
	for (;;)
	{
		int64_t old = ht[i];

		if (atomicCompSwap(ht[i], old, bitsd(dbl(old) + v)) == old)
			break;
	}
}

bool any_pending(bool pending)
{
	return any(notEqual(subgroupBallot(pending), uvec4(0u)));
}

/* The probes of a key are the triangular numbers from its home slot (home + 1, + 3, + 6, ...: the step is the number of probes made), which visit every slot of a table of 2^n slots and do not run together
 * the keys whose homes are near, as a step of 1 does. */

/* One step of the search of the slot of the key (kw, nm) that never waits for another invocation: a slot being claimed is left for the next
 * step, and the whole subgroup takes every step together (see main), so the invocation that claimed it finishes within the step. */
void probe_step(inout bool pending, inout uint slot, inout uint probes, inout uint spins, out uint found, int64_t h, int64_t kw[8], uint nk, int64_t nm, uint naggs)
{
	uint mask = ctl[J_HT_MASK], base = slot * ctl[J_HT_STRIDE];
	int64_t ready = h | READY_BIT;
	int64_t s = ht[base];

	found = 0xFFFFFFFFu;
	if (s == 0l)
	{
		if (atomicCompSwap(ht[base], 0l, 1l) != 0l)
			return;
		int64_t used = atomicAdd(res[RES_HTUSED], 1l);

		if (uint(used) >= ctl[J_HT_MAXLOAD])
			res[RES_OVF] = 1l;
		for (uint j = 0u; j < nk; j++)
			ht[base + 1u + j] = kw[j];
		ht[base + 1u + nk] = nm;
		ht[base + 2u + nk] = 0l;
		for (uint a = 0u; a < naggs; a++)
		{
			uint k = ctl[J_AGGKIND + a], o = base + 3u + nk + a * 3u;

			ht[o] = acc_init(k, 0u);
			ht[o + 1u] = 0l;
			ht[o + 2u] = 0l;
		}
		memoryBarrierBuffer();
		atomicExchange(ht[base], ready);
		found = base;
		pending = false;
	}
	else if (s == 1l)
	{
		if (++spins > SPIN_MAX)
		{
			res[RES_OVF] = 1l;
			pending = false;
		}
	}
	else
	{
		bool eq = false;

		if (s == ready)
		{
			memoryBarrierBuffer();
			eq = ht[base + 1u + nk] == nm;
			for (uint j = 0u; j < nk && eq; j++)
				eq = ht[base + 1u + j] == kw[j];
		}
		if (eq)
		{
			found = base;
			pending = false;
		}
		else if (++probes > mask)
		{
			res[RES_OVF] = 1l;
			pending = false;
		}
		else
			slot = (slot + 1u) & mask;
	}
}

/* One aggregate of a row added into its slot with atomics: v is its value (the bits of a double for SUM_F, the key for MIN and MAX), count_it that the rows the aggregate took are counted in the slot */
void agg_add_one(uint k, int64_t v, uint o, bool count_it)
{
	if (k == AK_COUNT_STAR || k == AK_COUNT)
	{
		if (count_it)
			atomicAdd(ht[o], 1l);
		return;
	}
	if (k == AK_SUM_I)
	{
		atomicAdd(ht[o], sum_lo(v));
		if (sum_hi(v) != 0l)
			atomicAdd(ht[o + 1u], sum_hi(v));
	}
	else if (k == AK_SUM_F)
		acc_addf(o, dbl(v));
	else if (k == AK_MIN || k == AK_MIN_F)
		atomicMin(ht[o], v);
	else
		atomicMax(ht[o], v);
	if (count_it)
		atomicAdd(ht[o + 2u], 1l);
}

/* The same for the rows of the subgroup that found one slot (the lanes where use holds; every lane of the subgroup calls it): the lane lead adds their sums with one atomic a word. The limbs of
 * a SUM_I are added as the 32 bit halves they are, so that the sum of up to 64 lanes does not wrap. */
void agg_add_lanes(uint k, bool use, int64_t v, uint o, bool lead, bool count_it)
{
	int64_t n = subgroupAdd(use ? 1l : 0l);

	if (n == 0l)
		return;
	if (k == AK_COUNT_STAR || k == AK_COUNT)
	{
		if (lead && count_it)
			atomicAdd(ht[o], n);
		return;
	}
	if (k == AK_SUM_I)
	{
		int64_t lo = subgroupAdd(use ? sum_lo(v) : 0l), hi = subgroupAdd(use ? sum_hi(v) : 0l);

		if (lead)
		{
			atomicAdd(ht[o], lo);
			if (hi != 0l)
				atomicAdd(ht[o + 1u], hi);
		}
	}
	else if (k == AK_SUM_F)
	{
		double d = subgroupAdd(use ? dbl(v) : 0.0lf);

		if (lead)
			acc_addf(o, d);
	}
	else if (k == AK_MIN || k == AK_MIN_F)
	{
		int64_t m = subgroupMin(use ? v : I64_MAX);

		if (lead)
			atomicMin(ht[o], m);
	}
	else
	{
		int64_t m = subgroupMax(use ? v : I64_MIN);

		if (lead)
			atomicMax(ht[o], m);
	}
	if (lead && count_it)
		atomicAdd(ht[o + 2u], n);
}

/*
 * The rows of a subgroup that found their slots (todo), taken a slot at a time: the lanes of the slot of the lowest lane of todo are m, their number n, and the lowest of them lead, which adds all
 * their rows (agg_add_lanes) where each of them would have added one. False when no lane is left in todo, or when two slots in a row had one row (the keys of the subgroup are then mostly
 * apart, and the lanes left add their rows themselves, in parallel, which is cheaper than a round of reductions a lane).
 */
bool slot_round(inout bool todo, uint base, inout uint singles, out bool m, out bool lead, out int64_t n)
{
	uvec4 any = subgroupBallot(todo);
	uint first = subgroupBallotFindLSB(any);

	m = false;
	lead = false;
	n = 0l;
	if (!any_pending(todo))
		return false;
	m = todo && base == subgroupBroadcast(base, first);
	n = int64_t(subgroupBallotBitCount(subgroupBallot(m)));
	singles = n == 1l ? singles + 1u : 0u;
	if (singles == 2u)
	{
		m = false;
		return false;
	}
	lead = gl_SubgroupInvocationID == first;
	todo = todo && !m;
	return true;
}
