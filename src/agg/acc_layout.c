/*
 * agg/acc_layout.c - see acc_layout.h.
 */
#include "postgres.h"

#include "catalog/pg_type_d.h"

#include "agg/acc_layout.h"

unsigned
sift_agg_acc_layout(const SiftAggAgg *a, bool lane_serial)
{
	switch (a->kind)
	{
		case SAA_SUM_INT:
		case SAA_AVG_INT:
		case SAA_PCT_CONT:
		case SAA_PCT_DISC:
			return ACCL_I;
		case SAA_SUM_NUM:
		case SAA_AVG_NUM:
			return ACCL_W | ACCL_I | ACCL_NF;
		case SAA_MIN:
		case SAA_MAX:
			return ACCL_I | ACCL_D | (a->num.n ? ACCL_W : 0);
		case SAA_SUM_F64:
		case SAA_AVG_F64:
		case SAA_O_SUM_F64:
		case SAA_O_AVG_F64:
			return ACCL_D | ACCL_NF | (lane_serial ? ACCL_DT : 0);
		case SAA_O_SUM_I64:
			return ACCL_W;
		case SAA_M_PG:
			return ACCL_NF | ACCL_V;
		case SAA_X_SUM:
		case SAA_X_AVG:
			return ACCL_D | ACCL_NF | ACCL_V | (lane_serial && a->restype == FLOAT8OID ? ACCL_DT : 0) |
				(a->restype == NUMERICOID ? ACCL_I | ACCL_W : 0);
		case SAA_M_SUM:
		case SAA_M_AVG:
			return ACCL_D | ACCL_NF | ACCL_V;
		case SAA_M_MIN:
		case SAA_M_MAX:
		case SAA_X_MIN:
		case SAA_X_MAX:
			return ACCL_V;
		case SAA_O_COUNT_ROWS:
		case SAA_COUNT_ROWS:
			return ACCL_LAST;
		case SAA_GEN:
			return ACCL_V | ACCL_R | ACCL_NF | (a->gen->ordered || a->gen->sorted || a->gen->distinct ? ACCL_I : 0);
		default:
			return 0;
	}
}

double
sift_agg_acc_layout_bytes(unsigned layout)
{
	static const struct
	{
		unsigned	flag;
		int			bytes;
	}			array[] = {
		{ACCL_I, sizeof(int64)}, {ACCL_W, sizeof(sift_i128)}, {ACCL_D, sizeof(double)}, {ACCL_DT, sizeof(double)},
		{ACCL_NF, sizeof(uint8)}, {ACCL_V, sizeof(Datum)}, {ACCL_R, sizeof(Datum)}, {ACCL_LAST, sizeof(uint64)},
	};
	double		b = sizeof(int64);	/* n */

	for (int k = 0; k < lengthof(array); k++)
		if (layout & array[k].flag)
			b += array[k].bytes;
	return b;
}
