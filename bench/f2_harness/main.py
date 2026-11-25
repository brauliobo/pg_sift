import argparse
import sys
from .config import RELEASE_SCALES, SCALES
from .ladder import compute_slopes, run_ladder
from .report import write_markdown, write_results
from .run import Run, record_meta
from .workloads import workload_b2, workload_c, workload_d, workload_e, workload_f, workload_g
from . import __doc__ as DOC


def parse_args():
    ap = argparse.ArgumentParser(description=DOC.split("\n")[1])
    ap.add_argument("--host", required=True, help="socket directory or host of the cluster")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--user", required=True)
    ap.add_argument("--round", default="v6-core", help="name of the round: <out-dir>/<round>.tsv and .md")
    ap.add_argument("--git-sha", default="unknown")
    ap.add_argument("--git-branch", default="unknown")
    ap.add_argument("--build-sec", type=float, default=0.0)
    ap.add_argument("--make-jobs", type=int, default=4, help="parallel jobs of the build, for the record")
    ap.add_argument("--out-dir", default=".", help="where <round>.tsv and <round>.md are written")
    ap.add_argument("--scales", help="comma separated row counts of the ladder (default 10000,40000,160000)")
    ap.add_argument("--release-scale", nargs="?", const=",".join(map(str, RELEASE_SCALES)),
                    help="the sign-off ladder: 10000,100000,1000000 or the comma separated row counts given")
    ap.add_argument("--docs", action="store_true", help="also write docs/results/v6-core.tsv (read by scripts/gen-docs.py)")
    ap.add_argument("--verify-diff", action="store_true", help="check the checksum of every Q1-Q8 and F4-F8 answer of the sift plan against the heap scan; a difference fails the run")
    args = ap.parse_args()
    ladder = args.release_scale or args.scales
    if ladder:
        SCALES[:] = sorted({int(x) for x in ladder.split(",")})
        if len(SCALES) < 3:
            ap.error("the ladder needs at least 3 row counts (an OLS slope over 2 points is a line through them)")
    return args


def main():
    args = parse_args()
    st = Run(args)
    for step in (record_meta, run_ladder, compute_slopes, workload_d, workload_c, workload_b2, workload_f, workload_e,
                 workload_g, write_results, write_markdown):
        step(st)
    print(f"[v6-core] wrote {st.tsv_path} and {st.md_path}" + (" and docs/results/v6-core.tsv" if args.docs else ""), flush=True)
    if st.verify_bad:
        sys.exit(f"verify-diff: the sift plan and the heap scan disagree on {len(st.verify_bad)} probes: {', '.join(st.verify_bad)}")
