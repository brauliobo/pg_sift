/*
 * col/test/unit_damage.c - damage the tests do to the units and chunks of a segment
 * one byte, one chunk, one object
 * to see the verification refuse it
 */
#include "postgres.h"
#ifdef SIFT_TESTING
#include "funcapi.h"
#include "col/am.h"
#include "col/pagg.h"
#include "core/sql.h"
#endif							/* SIFT_TESTING */
#include "col/test/test_int.h"

#ifdef SIFT_TESTING
PG_FUNCTION_INFO_V1(sift_test_col_damage);

PG_FUNCTION_INFO_V1(sift_test_col_pre_stats);

typedef enum DamageSection
{
	DMG_UNIT,
	DMG_CHUNK,
	DMG_SCHEMA
} DamageSection;

typedef struct Damage
{
	DamageSection section;
	const SiftColSchema *sc;
	MemoryContext cx;
	uint32		needed;			/* the bytes a read of the intact object touches: a cut below it must be refused */
	uint32		flags;			/* the chunk flags and (1 << 8 + enc) seen in the objects that parsed */
	bool		sound;			/* what a reader trusts after a parse still holds, and a refusal was a corruption error */
} Damage;

/* the kinds, nulls and overflow bitmaps of a parsed chunk, checked again from the bits: no kind outside its slots, none in two, none beyond n */
static bool
chunk_sound(const SiftCChunk *c)
{
	uint32		words = (c->n + 63) / 64;
	uint64		tail = c->n % 64 ? ~UINT64CONST(0) << (c->n % 64) : 0;

	for (uint32 w = 0; w < words; w++)
	{
		uint64		nul = c->nulls ? c->nulls[w] : 0,
					ovf = c->ovf ? c->ovf[w] : 0,
					seen = 0;

		if ((ovf & ~nul) || (w == words - 1 && ((nul | ovf) & tail)))
			return false;
		for (int tag = 1; tag < lengthof(c->kind); tag++)
		{
			uint64		m = c->kind[tag] ? c->kind[tag][w] : 0;

			if ((m & (seen | ovf)) || (tag == SIFT_COLTAG_JNULL ? (m & ~nul) : (m & nul)) || (w == words - 1 && (m & tail)))
				return false;
			seen |= m;
		}
	}
	return true;
}

/* A chunk the way a scan reads it: decoded, and its strings taken */
static void
chunk_read(Damage *d, const SiftCChunk *c)
{
	d->flags |= c->flags | 1U << (8 + c->enc);
	d->sound &= chunk_sound(c);
	if (c->n > (1U << 20))
		return;
	if (c->enc == SIFT_COLENC_STRS)
	{
		uint32	   *off;
		char	   *bytes;

		sift_cchunk_strs(c, CurrentMemoryContext, &off, &bytes);
	}
	else
		sift_cchunk_decode(c, palloc(sizeof(int64) * (c->n + SIFT_COL_PAD / 8)));
}

/* One stored object read as a scan reads it; false when it is refused, by a failed parse or by an error (a corruption one, or d->sound is false) */
static bool
damage_read(Damage *d, const uint8 *p, uint32 len, const SiftColSchema *orig)
{
	volatile bool ok = false;
	MemoryContext old;

	MemoryContextReset(d->cx);
	old = MemoryContextSwitchTo(d->cx);
	d->sound = true;
	PG_TRY();
	{
		SiftCChunk	c;

		switch (d->section)
		{
			case DMG_CHUNK:
				if ((ok = sift_cchunk_parse(p, len, &c)))
				{
					d->needed = (uint32) (c.payload + c.paylen - p);
					chunk_read(d, &c);
				}
				break;
			case DMG_UNIT:
				{
					SiftCUnitView v;

					if ((ok = sift_cunit_parse(p, len, d->sc, &v)))
					{
						SiftKey    *keys = palloc(sizeof(SiftKey) * (v.nrows + 1));

						d->needed = 0;
						for (int col = 0; col < d->sc->ncols; col++)
							if (sift_cunit_chunk(&v, col, &c))
							{
								d->needed = Max(d->needed, v.chunk_off[col] + v.chunk_len[col]);
								chunk_read(d, &c);
							}
						sift_cunit_keys(&v, 7, keys);
						for (uint32 r = 1; r < v.nrows; r++)
							d->sound &= keys[r - 1] < keys[r];
					}
				}
				break;
			case DMG_SCHEMA:
				{
					SiftColSchema *s = sift_cschema_parse(d->cx, p, len);

					if ((ok = s != NULL))
					{
						d->needed = len;
						/* the hash covers every field but the flags: an accepted schema is the stored one */
						for (int i = 0; i < s->ncols; i++)
						{
							const SiftColDef *a = &s->cols[i],
									   *b = &orig->cols[i];

							d->sound &= i < orig->ncols && a->id == b->id && a->parent == b->parent && a->kind == b->kind && a->store == b->store &&
								a->attno == b->attno && a->type == b->type && a->scale == b->scale && a->lanes == b->lanes && a->lane == b->lane &&
								strcmp(a->path, b->path) == 0;
						}
						d->sound &= s->ncols == orig->ncols;
					}
				}
				break;
		}
	}
	PG_CATCH();
	{
		ErrorData  *e;

		MemoryContextSwitchTo(d->cx);
		e = CopyErrorData();
		FlushErrorState();
		d->sound = e->sqlerrcode == ERRCODE_DATA_CORRUPTED || e->sqlerrcode == ERRCODE_INDEX_CORRUPTED || e->sqlerrcode == ERRCODE_FEATURE_NOT_SUPPORTED;
		ok = false;
	}
	PG_END_TRY();
	MemoryContextSwitchTo(old);
	return ok;
}

#define DAMAGE_POSITIONS	600	/* the cuts and the damaged bytes of an object: all of the first and last 64, then every nth */

/* The object intact, cut at the positions, and with a byte set to other values or a bit flipped at them; the tally is the columns of sift._test_col_damage */
static void
damage_object(Damage *d, const uint8 *blob, uint32 len, const SiftColSchema *orig, int64 *t)
{
	static const uint8 vals[] = {0x00, 0x01, 0x02, 0x7F, 0x80, 0xFF};
	uint32		stride = 1 + len / DAMAGE_POSITIONS,
				needed;
	uint8	   *copy = palloc0(len + SIFT_COL_PAD);

	enum {OBJECTS, INTACT, CUT, CUT_ACCEPTED, DAMAGED, ACCEPTED, UNSOUND};

	t[OBJECTS]++;
	memcpy(copy, blob, len);
	if (damage_read(d, copy, len, orig) && d->sound)
		t[INTACT]++;
	else
		t[UNSOUND]++;
	needed = d->needed;
	for (uint32 l = 0; l < len; l += (l < 64 || l + 64 >= len) ? 1 : stride)
	{
		memset(copy, 0, len + SIFT_COL_PAD);
		memcpy(copy, blob, l);
		t[CUT]++;
		if (damage_read(d, copy, l, orig))
		{
			t[CUT_ACCEPTED]++;
			t[UNSOUND] += l < needed || !d->sound;
		}
		else
			t[UNSOUND] += !d->sound;
	}
	for (uint32 i = 0; i < len; i += (i < 64 || i + 64 >= len) ? 1 : stride)
		for (int k = 0; k < lengthof(vals) + 2; k++)
		{
			uint8		v = k < lengthof(vals) ? vals[k] : blob[i] ^ (k == lengthof(vals) ? 0x01 : 0x80);

			if (v == blob[i])
				continue;
			memcpy(copy, blob, len);
			copy[i] = v;
			t[DAMAGED]++;
			if (damage_read(d, copy, len, orig))
			{
				t[ACCEPTED]++;
				t[UNSOUND] += !d->sound;
			}
			else
				t[UNSOUND] += !d->sound;
		}
	pfree(copy);
}

/*
 * sift._test_col_damage(index, section, units): the stored objects of a section ('unit' the unit record with its chunks, 'chunk' each chunk alone,
 * 'schema' the leaf list) of the first units of the index, read as a scan reads them, cut at the positions of damage_object and with bytes changed,
 * one at a time. A reader must never read outside the object (meaningful under SANITIZE=1), must refuse a cut one (below the bytes it needs) and
 * every refusal must be a corruption error (or the unsupported-feature one of an encoding this build does not know); what it accepts keeps what the format promises (the kinds of a chunk, the order of the keys of a unit,
 * the stored leaf list). flags: the chunk flags (1 nulls, 2 overflow, 4 tags, 8 kinds) and, from bit 8, the encodings seen in the intact objects.
 */
Datum
sift_test_col_damage(PG_FUNCTION_ARGS)
{
	char	   *section = text_to_cstring(PG_GETARG_TEXT_PP(1));
	int			maxunits = PG_GETARG_INT32(2);
	TestScan	ts;
	SiftColUnit unit;
	Damage		d = {0};
	int64		t[7] = {0};
	Datum		v[8];
	bool		nulls[8] = {false};

	if (strcmp(section, "unit") == 0)
		d.section = DMG_UNIT;
	else if (strcmp(section, "chunk") == 0)
		d.section = DMG_CHUNK;
	else if (strcmp(section, "schema") == 0)
		d.section = DMG_SCHEMA;
	else
		elog(ERROR, "sift test: unknown section \"%s\"", section);
	d.cx = AllocSetContextCreate(CurrentMemoryContext, "damage", ALLOCSET_DEFAULT_SIZES);
	ctest_scan_open(&ts, PG_GETARG_OID(0), "", TEST_NOWBUF);
	d.sc = sift_col_rel(ts.index)->schema;
	if (d.section == DMG_SCHEMA)
	{
		StringInfoData buf;

		initStringInfo(&buf);
		sift_cschema_serialize(d.sc, &buf);
		damage_object(&d, (const uint8 *) buf.data, buf.len, d.sc, t);
	}
	for (int64 units = 0; d.section != DMG_SCHEMA && units < maxunits && ctest_test_next(&ts, &unit);)
	{
		SiftColSegment *seg;
		uint32		u,
					len;
		const uint8 *rec;
		SiftCUnitView uv;

		if (!sift_col_unit_stored(&unit, &seg, &u))
			continue;
		units++;
		rec = sift_cseg_unit(seg, u, &len);
		if (d.section == DMG_UNIT)
			damage_object(&d, rec, len, NULL, t);
		else if (sift_cunit_parse(rec, len, d.sc, &uv))
			for (int col = 0; col < d.sc->ncols; col++)
				if (uv.chunk_len[col] > 0)
				{
					/* a chunk is 8-aligned in the record, as the reader's own parse needs it */
					uint8	   *c = palloc(uv.chunk_len[col] + SIFT_COL_PAD);

					memset(c + uv.chunk_len[col], 0, SIFT_COL_PAD);
					memcpy(c, rec + uv.chunk_off[col], uv.chunk_len[col]);
					damage_object(&d, c, uv.chunk_len[col], NULL, t);
					pfree(c);
				}
	}
	ctest_scan_close(&ts);
	for (int i = 0; i < 7; i++)
		v[i] = Int64GetDatum(t[i]);
	v[7] = Int32GetDatum((int32) d.flags);
	PG_RETURN_DATUM(sift_result_row(fcinfo, v, nulls));
}

/* sift._test_col_pre_stats(index): the units that hold the histogram (with min and max) of a leaf, or the rollup of a list, as the planner counts them */
Datum
sift_test_col_pre_stats(PG_FUNCTION_ARGS)
{
	ReturnSetInfo *rsi = (ReturnSetInfo *) fcinfo->resultinfo;
	Relation	index = index_open(PG_GETARG_OID(0), AccessShareLock);
	const SiftColPreStats *pre = sift_col_pre_stats(index);

	InitMaterializedSRF(fcinfo, 0);
	for (int c = 0; c < sift_col_index_defs(index)->ncols; c++)
	{
		Datum		v[7] = {Int32GetDatum(c), Float8GetDatum(pre->units), Float8GetDatum(pre->dim[c]), Float8GetDatum(pre->mm[c]), Float8GetDatum(pre->roll[c])};
		bool		nulls[7] = {false};

		if (pre->dim[c] > 0 || pre->roll[c] > 0)
			tuplestore_putvalues(rsi->setResult, rsi->setDesc, v, nulls);
	}
	index_close(index, AccessShareLock);
	return (Datum) 0;
}
#endif							/* SIFT_TESTING */
