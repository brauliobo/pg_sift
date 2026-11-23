/*
 * agg/vk/layout.h - the words the host and the shaders exchange (agg/vk/shaders/common_decode.glsl, common_vexpr.glsl, vk_decode.comp
 * name the same numbers). Plain C, no PostgreSQL headers.
 */
#ifndef SIFT_AGG_VK_LAYOUT_H
#define SIFT_AGG_VK_LAYOUT_H

/* the words of the result buffer */
#define SIFT_VK_RES_ERR			0u
#define SIFT_VK_RES_OVF			1u
#define SIFT_VK_RES_HTUSED		2u
#define SIFT_VK_RES_NCAND		3u
#define SIFT_VK_RES_NCOMP		4u
#define SIFT_VK_RES_DEC_OFF		64u
#define SIFT_VK_RES_DEC_CAP		16384u
#define SIFT_VK_RES_ACC_OFF		(SIFT_VK_RES_DEC_OFF + SIFT_VK_RES_DEC_CAP)

/* the words of the job header in the control buffer */
#define SIFT_VK_J_NGRAN			0u
#define SIFT_VK_J_NCOLS			1u
#define SIFT_VK_J_NOPS			2u
#define SIFT_VK_J_NAGGS			3u
#define SIFT_VK_J_NKEYS			4u
#define SIFT_VK_J_WHERE			5u
#define SIFT_VK_J_GRAN0			6u
#define SIFT_VK_J_FLAGS			7u
#define SIFT_VK_J_GT			8u
#define SIFT_VK_J_DT			9u
#define SIFT_VK_J_OPS			10u
#define SIFT_VK_J_POOL			11u
#define SIFT_VK_J_DEAD			12u
#define SIFT_VK_J_KEYREG		16u
#define SIFT_VK_J_AGGKIND		24u
#define SIFT_VK_J_AGGVREG		40u
#define SIFT_VK_J_AGGFREG		56u
#define SIFT_VK_J_DENSE_KMIN	72u
#define SIFT_VK_J_DENSE_SPAN	76u
#define SIFT_VK_J_DENSE_G		78u
#define SIFT_VK_J_HT_MASK		80u
#define SIFT_VK_J_HT_STRIDE		81u
#define SIFT_VK_J_HT_MAXLOAD	82u
#define SIFT_VK_J_THETA			84u
#define SIFT_VK_J_TOPN_DESC		86u
#define SIFT_VK_J_TOPN_NULLS	87u
#define SIFT_VK_J_CAND_CAP		88u
#define SIFT_VK_J_SORTREG		89u
#define SIFT_VK_J_FAST_IMM		96u
#define SIFT_VK_J_CNT_ROWS		112u
#define SIFT_VK_J_HDR_WORDS		128u

#define SIFT_VK_FL_THETA		1u
#define SIFT_VK_FL_SORT_FLOAT	2u
#define SIFT_VK_FL_NULLS		4u

/* the aggregate kinds the kernels know */
#define SIFT_VK_AK_COUNT_STAR	0u
#define SIFT_VK_AK_COUNT		1u
#define SIFT_VK_AK_SUM_I		2u
#define SIFT_VK_AK_SUM_F		3u
#define SIFT_VK_AK_MIN			4u
#define SIFT_VK_AK_MAX			5u
#define SIFT_VK_AK_MIN_F		6u
#define SIFT_VK_AK_MAX_F		7u

#define SIFT_VK_NONE8			255u
#define SIFT_VK_DC_ALL_VALID	0xFFFFFFFFu
#define SIFT_VK_DC_ALL_NULL		0xFFFFFFFEu

/* a decode job: 16 words */
#define SIFT_VK_DJ_WORDS		16u
#define SIFT_VK_DEC_WG			64u
#define SIFT_VK_DEC_SLOTS		10u
#define SIFT_VK_DEC_WRAP_BYTES	262144u
/* MEM[MEM-VK-04]: decode scratch of one workgroup (896KiB; DEC_WGS of them in a slot work buffer) */
#define SIFT_VK_DEC_SCR_BYTES	((uint64_t) SIFT_VK_DEC_SLOTS * 8192 * 8 + SIFT_VK_DEC_WRAP_BYTES)

/* the encodings of a chunk (SiftChunkEnc of col/granule/granule.h) the decoder takes */
#define SIFT_VK_ENC_INT_FOR		2u
#define SIFT_VK_ENC_INT_DELTA	3u
#define SIFT_VK_ENC_INT_CONST	4u
#define SIFT_VK_ENC_DICT_CODE	5u
#define SIFT_VK_ENC_FLOAT_RAW	7u
#define SIFT_VK_ENC_ALL_NULL	9u
#define SIFT_VK_ENC_BOOL_BITS	10u
#define SIFT_VK_ENC_NUM64		11u
#define SIFT_VK_ENC_RLE			15u
#define SIFT_VK_ENC_ALP			16u
#define SIFT_VK_ENC_ALP_RD		25u

/* chunk flags */
#define SIFT_VK_CF_LZ4			1u		/* the record is {algorithm, raw length} and an LZ4 block of the validity and the body */
#define SIFT_VK_CF_ALL_VALID	2u
#define SIFT_VK_CF_ALL_NULL		4u
#define SIFT_VK_CF_NULL_SPARSE	8u		/* the validity is a count and a list of the rows that are null */
#define SIFT_VK_CF_NONNULL_SPARSE 16u	/* ... of the rows that hold a value */

/* the operations of a program: those of SiftVecOpCode (agg/vexpr.h), and the membership of a set */
enum SiftVkOp
{
	SIFT_VK_OP_LOAD_COL, SIFT_VK_OP_LOAD_CONST, SIFT_VK_OP_LOAD_NULL, SIFT_VK_OP_CAST, SIFT_VK_OP_ADD, SIFT_VK_OP_SUB,
	SIFT_VK_OP_MUL, SIFT_VK_OP_DIV, SIFT_VK_OP_NEG, SIFT_VK_OP_ABS, SIFT_VK_OP_CMP, SIFT_VK_OP_IS_NULL, SIFT_VK_OP_MASK_AND,
	SIFT_VK_OP_MASK_OR, SIFT_VK_OP_MASK_NOT, SIFT_VK_OP_MASK_COL, SIFT_VK_OP_MASK_CONST, SIFT_VK_OP_INIT_NULL, SIFT_VK_OP_COPY,
	SIFT_VK_OP_BLEND, SIFT_VK_OP_ACT_COPY, SIFT_VK_OP_ACT_AND, SIFT_VK_OP_ACT_ANDN, SIFT_VK_OP_ACT_NOTFALSE,
	SIFT_VK_OP_ACT_NOTVALID, SIFT_VK_OP_SKIP_IF_EMPTY, SIFT_VK_OP_RESCALE, SIFT_VK_OP_DATE_TRUNC, SIFT_VK_OP_EXTRACT,
	SIFT_VK_OP_MASK_BLEND, SIFT_VK_OP_MASK_UNKNOWN, SIFT_VK_OP_MASK_ISNULL, SIFT_VK_OP_ACT_UNKNOWN,
	SIFT_VK_OP_IN_SET = 40
};

/* the types of the operands */
enum SiftVkType { SIFT_VK_T_I32, SIFT_VK_T_I64, SIFT_VK_T_NUM, SIFT_VK_T_F64 };

enum SiftVkCmp { SIFT_VK_CMP_EQ, SIFT_VK_CMP_NE, SIFT_VK_CMP_LT, SIFT_VK_CMP_LE, SIFT_VK_CMP_GT, SIFT_VK_CMP_GE };

/* the errors a program raises (the first word of the error of a result) */
enum SiftVkProgErr
{
	SIFT_VK_PE_INT4 = 1, SIFT_VK_PE_INT8, SIFT_VK_PE_DIV0, SIFT_VK_PE_FLOAT_OVF, SIFT_VK_PE_FLOAT_UNF, SIFT_VK_PE_TIME,
	SIFT_VK_PE_UNSUP, SIFT_VK_PE_RANGE
};

#define SIFT_VK_OP_WORDS		8u

#endif
