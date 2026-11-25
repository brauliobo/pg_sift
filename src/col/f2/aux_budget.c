/*
 * col/f2/aux_budget.c - the budget of the sections a part carries for the reader's benefit alone (col/f2/aux_budget.h). Pure arithmetic over the sizes of the part.
 */
#include "col/f2/aux_budget.h"
#include "col/f2/part/format.h"

void
sift_aux_begin(SiftAuxBudget *b, uint64 d_total, uint64 d_comp, uint64 fixed_inline, uint32 nrows, uint32 ng, uint32 level)
{
	d_comp = Min(d_comp, d_total);
	b->cap = Min(d_total * SIFT_AUX_CAP_PERMILLE / 1000, d_comp * SIFT_AUX_COMP_CAP_PERMILLE / 1000);
	if (d_total + fixed_inline <= SIFT_PACKED_SLOT_MAX_BYTES)
		b->cap = 0;
	b->rem = b->cap;
	b->d_total = d_total;
	b->d_comp = d_comp;
	b->nrows = nrows;
	b->ng = ng;
	b->level = level;
}

bool
sift_aux_charge(SiftAuxBudget *b, uint64 bytes)
{
	bool		fits = bytes <= b->rem;

	b->rem = fits ? b->rem - bytes : 0;
	return fits;
}

bool
sift_aux_marks(SiftAuxBudget *b, uint64 nbytes)
{
	if (b->ng < 2)
		return false;
	b->rem -= Min(b->rem, nbytes);
	return true;
}

bool
sift_aux_col_stats(SiftAuxBudget *b, uint64 nbytes)
{
	if (b->ng <= 1 || b->nrows < SIFT_AUX_STATS_MIN_ROWS || nbytes > Min(b->rem, b->d_total * SIFT_AUX_STATS_PERMYRIAD / 10000))
		return false;
	return sift_aux_charge(b, nbytes);
}

uint64
sift_aux_skip_budget(const SiftAuxBudget *b)
{
	return b->nrows < SIFT_AUX_SKIP_MIN_ROWS ? 0 : Min(b->rem, b->d_comp * SIFT_AUX_SKIP_COMP_PERMILLE / 1000);
}

uint32
sift_aux_kll_k(const SiftAuxBudget *b, uint32 spec_k)
{
	if (b->level == 0 || b->nrows < SIFT_AUX_KLL_MIN_ROWS)
		return 0;
	return b->nrows < SIFT_AUX_KLL_FULL_ROWS ? Min(spec_k, SIFT_AUX_KLL_SMALL_K) : spec_k;
}

uint32
sift_aux_kll_cols(SiftAuxBudget *b, uint32 ncols, uint64 bytes_per_col)
{
	uint32		fit = bytes_per_col == 0 ? ncols : (uint32) Min((uint64) ncols, b->rem / bytes_per_col);

	sift_aux_charge(b, (uint64) fit * bytes_per_col);
	return fit;
}

bool
sift_aux_bloom_ok(const SiftAuxBudget *b, uint64 ndv)
{
	return b->nrows >= SIFT_AUX_SKIP_MIN_ROWS && ndv >= SIFT_AUX_BLOOM_MIN_NDV;
}

bool
sift_aux_trigram_ok(const SiftAuxBudget *b, bool is_text, uint64 avg_len, bool promoted_or_dict)
{
	return b->nrows >= SIFT_AUX_SKIP_MIN_ROWS && is_text && avg_len >= SIFT_AUX_TRIGRAM_MIN_AVG && !promoted_or_dict;
}
