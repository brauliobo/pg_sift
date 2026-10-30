/*
 * agg/combo/out.c - combos: the output rows
 * The output table of a combo, the decoding of a numeric, the evaluation of a slot and the
 * emit of the rows of a combination.
 */
#include "postgres.h"

#include "executor/executor.h"

#include "agg/combo/int.h"

/* the output rows of a tuple whose key has nm matches */
static uint32
join_nout(SiftJoinKind kind, int nm)
{
	switch (kind)
	{
		case SJ_INNER:
		case SJ_RIGHT:
			return nm;
		case SJ_LEFT:
		case SJ_FULL:
			return nm > 0 ? nm : 1;
		case SJ_SEMI:
			return nm > 0;
		case SJ_ANTI:
			return nm == 0;
	}
	return 0;
}

static void
combo_grow_out(AggExec *ex, ComboSt *cs, int nslots, uint32 add)
{
	if (cs->nout + add > cs->outcap)
	{
		cs->outcap = Max(Max(cs->outcap * 2, cs->nout + add), 64);
		cs->val = cs->val ? repalloc(cs->val, (size_t) cs->outcap * Max(nslots, 1) * sizeof(uint32)) :
			MemoryContextAlloc(cs->cxt, (size_t) cs->outcap * Max(nslots, 1) * sizeof(uint32));
		cs->maprow = cs->maprow ? repalloc(cs->maprow, cs->outcap * sizeof(uint32)) :
			MemoryContextAlloc(cs->cxt, cs->outcap * sizeof(uint32));
	}
}

/* the table of the distinct values a slot gives ids to: of the key, or of the aggregate it is the argument of */
static OutTable *
slot_ot(AggExec *ex, const SiftAggSlot *sl)
{
	return sl->agg >= 0 ? &ex->aot[sl->agg] : &ex->ks[sl->key].ot;
}

/*
 * A finite numeric as the integer times 10^-dscale. This reads the stored form of the type (a short or long header, base 10000
 * digits, the weight of the first one), which is what pg_upgrade keeps stable; NaN, the infinities and what does not fit 38 digits
 * are not ok and stay numerics (the sums add them with numeric_add).
 */
void
numeric_decode(Datum d, DNum *out)
{
	const char *data = VARDATA_ANY(DatumGetPointer(d));
	Size		len = VARSIZE_ANY_EXHDR(DatumGetPointer(d));
	const sift_i128 *p10 = pow10_tab();
	uint16		hdr;
	int			hsz,
				weight,
				dscale;
	bool		neg;
	sift_i128	m = 0;

	out->ok = false;
	memcpy(&hdr, data, sizeof(hdr));
	if ((hdr & 0xC000) == 0xC000)
		return;
	if (hdr & 0x8000)
	{
		hsz = 2;
		dscale = (hdr & 0x1F80) >> 7;
		weight = (hdr & 0x0040) ? (int) (~0x003F | (hdr & 0x003F)) : (int) (hdr & 0x003F);
		neg = (hdr & 0x2000) != 0;
	}
	else
	{
		int16		w;

		hsz = 4;
		memcpy(&w, data + 2, sizeof(w));
		dscale = hdr & 0x3FFF;
		weight = w;
		neg = (hdr & 0xC000) == 0x4000;
	}
	for (int i = 0; i < (int) ((len - hsz) / 2); i++)
	{
		int16		dg;
		int			e = 4 * (weight - i) + dscale;
		sift_i128	t;

		memcpy(&dg, data + hsz + 2 * i, sizeof(dg));
		if (dg == 0)
			continue;
		if (e < 0)
		{
			if (e < -3 || dg % (int) p10[-e] != 0)
				return;
			t = dg / (int) p10[-e];
		}
		else if (e > 38 || __builtin_mul_overflow((sift_i128) dg, p10[e], &t))
			return;
		if (__builtin_add_overflow(m, t, &m))
			return;
	}
	out->m = neg ? -m : m;
	out->scale = (int16) dscale;
	out->ok = dscale <= 38;
}

/* a new value of a direct slot, kept in the cache; the number (from 1) it goes by */
static uint32
dat_put(AggExec *ex, ComboSt *cs, int sl, Datum d)
{
	Datum		v = d;

	if (cs->ndat == cs->datcap)
	{
		MemoryContext old = MemoryContextSwitchTo(cs->cxt);

		cs->datcap = Max(cs->datcap * 2, 64);
		cs->dat = cs->dat ? repalloc(cs->dat, cs->datcap * sizeof(Datum)) : palloc(cs->datcap * sizeof(Datum));
		if (cs->decode)
			cs->dnum = cs->dnum ? repalloc(cs->dnum, cs->datcap * sizeof(DNum)) : palloc(cs->datcap * sizeof(DNum));
		MemoryContextSwitchTo(old);
	}
	if (!cs->tbyval[sl])
	{
		Size		sz = cs->tlen[sl] == -1 ? VARSIZE_ANY(DatumGetPointer(d)) : cs->tlen[sl];

		if (cs->arena_left < sz)
		{
			cs->arena_left = Max(sz, 65536);
			cs->arena = MemoryContextAlloc(cs->cxt, cs->arena_left);
		}
		memcpy(cs->arena, DatumGetPointer(d), sz);
		v = PointerGetDatum(cs->arena);
		sz = MAXALIGN(sz);
		cs->arena += Min(sz, cs->arena_left);
		cs->arena_left -= Min(sz, cs->arena_left);
	}
	if (cs->decode && cs->decode[sl])
		numeric_decode(v, &cs->dnum[cs->ndat]);
	cs->dat[cs->ndat++] = v;
	return cs->ndat;
}

/*
 * The value of slot sl in the output row `row` of a combo without a join (its entry is the row), evaluated by PostgreSQL when the first
 * tuple that needs it comes: an expression may raise an error (a division by zero) that the normal plan raises only for the rows that
 * reach the expression, and a slot is read only for the tuples that do.
 */
uint32
combo_eval_slot(AggExec *ex, int ci, uint32 row, int sl)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	ComboSt    *cs = &ex->cs[ci];
	MemoryContext old = MemoryContextSwitchTo(ex->comcxt);
	bool		isnull;
	Datum		d;
	uint32		r;

	combo_bind_leaves(ex, ci, &cs->vid[(size_t) row * cs->ninputs]);
	d = ExecEvalExprSwitchContext(cs->slot[sl], ex->lutcx, &isnull);
	if (isnull)
		r = 0;
	else if (cb->slot[sl].direct)
		r = dat_put(ex, cs, sl, d);
	else if (!cb->slot[sl].is_key)
		r = DatumGetBool(d);
	else
	{
		MemoryContextSwitchTo(ex->cxt);
		r = ot_id(slot_ot(ex, &cb->slot[sl]), d);
	}
	MemoryContextSwitchTo(old);
	/* the datums of the leaves and the temporaries of the expression are dropped every few evaluations, not after each */
	if ((++cs->nevals & 63) == 0)
	{
		ResetExprContext(ex->lutcx);
		MemoryContextReset(ex->comcxt);
	}
	cs->val[(size_t) row * cb->nslots + sl] = r;
	return r;
}

/* the output rows of an entry with nm matching rows (none: a row of NULL columns), every slot evaluated; in the combo's work context */
void
combo_emit(AggExec *ex, int ci, ComboEnt *ent, const uint32 *rows, int nm, uint32 nout)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = &ex->spec->join[cb->join];
	ComboSt    *cs = &ex->cs[ci];
	MapTab	   *m = cs->map;

	combo_grow_out(ex, cs, cb->nslots, nout);
	ent->nout = nout;
	ent->base = cs->nout;
	for (uint32 o = 0; o < nout; o++)
	{
		cs->maprow[cs->nout] = nm > 0 ? rows[o] : UINT32_MAX;
		for (int c = 0; c < j->ncol; c++)
			if (nm > 0)
				map_bind(ex, cb->nleaves + c, m, rows[o], c);
			else
				bind_param(ex, cb->nleaves + c, (Datum) 0, true);
		for (int sl = 0; sl < cb->nslots; sl++)
		{
			bool		isnull;
			Datum		d = ExecEvalExprSwitchContext(cs->slot[sl], ex->lutcx, &isnull);
			uint32		r;

			if (!cb->slot[sl].is_key)
				r = !isnull && DatumGetBool(d);
			else if (isnull)
				r = 0;
			else
			{
				MemoryContextSwitchTo(ex->cxt);
				r = ot_id(slot_ot(ex, &cb->slot[sl]), d);
				MemoryContextSwitchTo(ex->comcxt);
			}
			cs->val[(size_t) cs->nout * cb->nslots + sl] = r;
		}
		cs->nout++;
		ResetExprContext(ex->lutcx);
	}
}

/* the output rows of a new entry: with a join its matches, every slot evaluated; without, one row whose slots wait for the tuples that need them */
void
combo_make(AggExec *ex, int ci, const uint64 *v, ComboEnt *ent)
{
	SiftAggCombo *cb = &ex->spec->combo[ci];
	const SiftAggJoin *j = cb->join >= 0 ? &ex->spec->join[cb->join] : NULL;
	ComboSt    *cs = &ex->cs[ci];
	MemoryContext old;
	uint32	   *rows;
	int			nm;

	if (j == NULL)
	{
		combo_grow_out(ex, cs, cb->nslots, 1);
		ent->nout = 1;
		ent->base = cs->nout;
		for (int sl = 0; sl < cb->nslots; sl++)
			cs->val[(size_t) cs->nout * cb->nslots + sl] = COMBO_UNEVAL;
		cs->nout++;
		return;
	}
	old = MemoryContextSwitchTo(ex->comcxt);
	combo_bind_leaves(ex, ci, v);
	nm = combo_matches(ex, ci, v, &rows);
	if (j->fslot >= 0 && nm > 0)
		nm = combo_filter_rows(ex, ci, rows, nm);
	for (int r = 0; cs->map->keep && r < nm; r++)
		cs->map->hit[rows[r]] = true;
	combo_emit(ex, ci, ent, rows, nm, join_nout(j->kind, nm));
	MemoryContextSwitchTo(old);
	MemoryContextReset(ex->comcxt);
}

/* the entry of the tuple of NULL leaves whose output rows are the rows of the preserved side that no tuple matched (their columns, NULL leaves) */
ComboEnt
combo_unmatched(AggExec *ex, int ci, const uint32 *prior)
{
	const MapTab *m = ex->cs[ci].map;
	const SiftAggCombo *cb = &ex->spec->combo[ci];
	uint64		v[SIFT_AGG_MAX_CLEAVES] = {0};
	ComboEnt	ent;
	MemoryContext old = MemoryContextSwitchTo(ex->comcxt);
	uint32	   *rows = palloc(Max(m->nrow, 1) * sizeof(uint32));
	int			nm = 0;

	for (uint32 r = 0; r < m->nrow; r++)
		if (!m->hit[r])
			rows[nm++] = r;
	for (int p = 0; p < cb->nleaves; p++)
		if (cb->leaf[p] < 0)
			v[p] = combo_prior_vid(ex, ci, p, prior[cb->refcombo[p]]);
	combo_bind_leaves(ex, ci, v);
	combo_emit(ex, ci, &ent, rows, nm, nm);
	MemoryContextSwitchTo(old);
	MemoryContextReset(ex->comcxt);
	return ent;
}
