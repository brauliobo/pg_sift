/* Developer experiments: sift.eval_methods names the ones that are on; a method is off unless named. */
#ifndef SIFT_CORE_EVAL_H
#define SIFT_CORE_EVAL_H

typedef enum SiftEvalMethod
{
	SIFT_EVAL_PENALTY_MAINTENANCE,
	SIFT_EVAL_UNIT_REWRITE,
	SIFT_EVAL_LATEST_HINT
} SiftEvalMethod;

extern void
sift_eval_init(void);
extern bool
sift_eval_on(SiftEvalMethod method);
#endif
