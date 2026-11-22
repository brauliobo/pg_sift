/*
 * common_ungrouped.glsl - the accumulators of the aggregates of no GROUP BY, shared by the program kernel and the fast kernel: every
 * invocation adds the rows it meets into registers (la, lf, lrows); the workgroup reduces them with the subgroup operations and one atomic
 * update per word goes to the result. The includer defines AGG_KIND(a) first.
 */
#define MAXAGG 16u

shared int64_t sh_rows[16];
shared int64_t sh_a[16 * MAXAGG * 3];
shared double sh_f[16 * MAXAGG];

int64_t lrows;
int64_t la[MAXAGG * 3];
double lf[MAXAGG];

void ungrouped_init(uint naggs)
{
	lrows = 0l;
	[[unroll]] for (uint a = 0u; a < naggs; a++)
	{
		la[a * 3u] = acc_init(AGG_KIND(a), 0u);
		la[a * 3u + 1u] = 0l;
		la[a * 3u + 2u] = 0l;
		lf[a] = 0.0lf;
	}
}

void ungrouped_subgroup(uint naggs)
{
	uint sg = gl_SubgroupID;

	lrows = subgroupAdd(lrows);
	if (subgroupElect())
		sh_rows[sg] = lrows;
	[[unroll]] for (uint a = 0u; a < naggs; a++)
	{
		uint k = AGG_KIND(a);
		int64_t w0 = la[a * 3u], w1 = la[a * 3u + 1u], w2 = subgroupAdd(la[a * 3u + 2u]);
		double f = 0.0lf;

		if (k == AK_MIN || k == AK_MIN_F)
			w0 = subgroupMin(w0);
		else if (k == AK_MAX || k == AK_MAX_F)
			w0 = subgroupMax(w0);
		else if (k == AK_SUM_F)
			f = subgroupAdd(lf[a]);
		else
		{
			w0 = subgroupAdd(w0);
			w1 = subgroupAdd(w1);
		}
		if (subgroupElect())
		{
			sh_a[(sg * MAXAGG + a) * 3u] = w0;
			sh_a[(sg * MAXAGG + a) * 3u + 1u] = w1;
			sh_a[(sg * MAXAGG + a) * 3u + 2u] = w2;
			sh_f[sg * MAXAGG + a] = f;
		}
	}
}

void ungrouped_publish(uint tid, uint naggs)
{
	if (tid > naggs)
		return;
	uint nsg = gl_NumSubgroups;

	if (tid == 0u)
	{
		int64_t t = 0l;
		for (uint s = 0u; s < nsg; s++)
			t += sh_rows[s];
		atomicAdd(res[RES_ACC_OFF], t);
		return;
	}
	uint a = tid - 1u, k = AGG_KIND(a);
	int64_t w0 = acc_init(k, 0u), w1 = 0l, w2 = 0l;
	double f = 0.0lf;

	for (uint s = 0u; s < nsg; s++)
	{
		int64_t x = sh_a[(s * MAXAGG + a) * 3u];

		if (k == AK_MIN || k == AK_MIN_F)
			w0 = min(w0, x);
		else if (k == AK_MAX || k == AK_MAX_F)
			w0 = max(w0, x);
		else
			w0 += x;
		w1 += sh_a[(s * MAXAGG + a) * 3u + 1u];
		w2 += sh_a[(s * MAXAGG + a) * 3u + 2u];
		f += sh_f[s * MAXAGG + a];
	}
	uint o = RES_ACC_OFF + 1u + a * 3u;

	if (k == AK_MIN || k == AK_MIN_F)
		atomicMin(res[o], w0);
	else if (k == AK_MAX || k == AK_MAX_F)
		atomicMax(res[o], w0);
	else if (k == AK_SUM_F)
	{
		for (;;)
		{
			int64_t old = res[o];
			if (atomicCompSwap(res[o], old, bitsd(dbl(old) + f)) == old)
				break;
		}
	}
	else
		atomicAdd(res[o], w0);
	if (w1 != 0l)
		atomicAdd(res[o + 1u], w1);
	if (w2 != 0l)
		atomicAdd(res[o + 2u], w2);
}

/* the workgroup's registers reduced and added to the result */
void ungrouped_finish(uint tid, uint naggs)
{
	ungrouped_subgroup(naggs);
	barrier();
	ungrouped_publish(tid, naggs);
}
