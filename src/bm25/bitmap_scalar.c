/* Baseline x86-64 (or any target) kernels: the generic template, no ISA extensions. */
#define KFN(x) x##_scalar
#define SIFT_PC64 sift_pc64
#define and_wide_scalar sift_and_wide_gen
#define or_wide_scalar sift_or_wide_gen
#define andnot_wide_scalar sift_andnot_wide_gen
#define cnt_wide_scalar sift_cnt_wide_gen
#define SIFT_KERN_TMPL
#include "bm25/bitmap_kern.h"

const sift_kernels sift_kern_scalar = SIFT_KERN_TABLE;
