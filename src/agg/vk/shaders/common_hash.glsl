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
