/*
 * common_compact.glsl - what the kernels that read the finished hash table share (vk_ht_compact.comp, vk_ht_topk.comp): the slot of a group written as a record without its state
 * word, and the words of the control and result buffers that only these kernels use (agg/vk/run.c names the same numbers).
 */
#define J_TK_NCOMP		90u		/* the components of the order of the top K groups, at most two: ncomp, then a word each */
#define J_TK_COMP		91u		/* bit 0: the component is an aggregate (else a key), bits 1..8 its index, bit 9 DESC, bit 10 NULLs first */
#define J_TK_K			93u		/* the K of the top K */
#define J_TK_COVER		94u		/* the components are all the sort columns: of the groups that tie with the K-th only the ones that make K are candidates */
#define RES_TAU			8u		/* valid, the NULL flags of the components, then the value of each: the K-th group of the order */
#define RES_TK_REMAIN	12u		/* the state of the radix selection of the threshold: the rank still to find among the groups equal to what is chosen so far */
#define RES_TK_PREFIX	13u		/* the bytes of the value chosen so far */
#define RES_TK_DONE		14u		/* the table has fewer than K groups: there is no threshold */
#define RES_TK_CLASS	15u		/* the class of the value chosen for the component: 0 NULL first, 1 a value, 2 NULL last */
#define RES_TK_TIES		16u		/* the groups that tie with the K-th and were taken */
#define RES_TK_SKIP		17u		/* the leading bytes of the value that all the groups that can be the K-th share: their rounds are not run */
#define TK_MAX_COMP		2u

/* the group of slot s as the record at word o of the result: the key words, the NULL mask, the rows and the aggregates; the counts the fast kernels leave out (J_CNT_ROWS) are the rows of the group */
void write_group(uint s, uint o)
{
	uint stride = ctl[J_HT_STRIDE], b = s * stride;

	for (uint w = 1u; w < stride; w++)
		out64[o + w - 1u] = ht[b + w];
	uint cr = ctl[J_CNT_ROWS], nk = ctl[J_NKEYS];

	for (uint a = 0u; cr != 0u && a < ctl[J_NAGGS]; a++)
		if (((cr >> a) & 1u) != 0u)
			out64[o + 2u + nk + a * 3u + (ctl[J_AGGKIND + a] <= AK_COUNT ? 0u : 2u)] = ht[b + 2u + nk];
}

bool slot_ready(uint s)
{
	return ht[s * ctl[J_HT_STRIDE]] < 0l;
}

/* the key j of slot s is NULL */
bool key_null(uint s, uint j)
{
	uint b = s * ctl[J_HT_STRIDE];

	return ((ht[b + 1u + ctl[J_NKEYS]] >> j) & 1l) != 0l;
}
