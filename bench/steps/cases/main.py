# The command line of cases.py.
# The docstring of bench/steps/cases.py is its help text.

import argparse
import os
import sys
import time

from . import runcfg
from .runcfg import BENCH, MIN_RUNS, REPO
from .coverage import cmd_coverage
from .run import cmd_cpus, cmd_list, cmd_plan, cmd_run


def main(doc):
    ap = argparse.ArgumentParser(description=doc.split('\n')[0])
    ap.add_argument('cmd', choices=['run', 'list', 'plan', 'coverage', 'cpus'])
    ap.add_argument('--cases', type=lambda s: s.split(','), default=[])
    ap.add_argument('--engines', default='sift,native')
    ap.add_argument('--rows', type=int, default=200000)
    ap.add_argument('--rounds', type=int, default=3)
    ap.add_argument('--runs', type=int, default=5)
    ap.add_argument('--duck-port', type=int, default=0)
    ap.add_argument('--out', default=os.path.join(BENCH, 'out', 'cases-%s.tsv' % time.strftime('%Y%m%dT%H%M%S')))
    ap.add_argument('--scales', help='run every case at ROWS times each of these (1,10,100): the curve of bench/results/cases-scale.tsv, the planner\'s plan at each size')
    ap.add_argument('--timeout', type=int, default=runcfg.TIMEOUT, help='seconds a statement may run (the large points need more)')
    ap.add_argument('--work-mem', help='work_mem of every lane (the cluster runs 64MB; a large setting is 4GB): written to the file as the work_mem meta')
    ap.add_argument('--lane-budget', type=float, default=runcfg.LANE_BUDGET, help='seconds of timed runs after which a lane stops repeating, with at least %d runs (0: always the full rounds x runs); a case whose statement takes seconds is not run fifteen times' % MIN_RUNS)
    ap.add_argument('--headline', action='store_true', help='only the cases marked `headline` in bench/cases.tsv (the quick release profile: bench/bench release-quick)')
    ap.add_argument('--arm-budget', type=int, default=300, help='with --records: seconds one arm may take over all its repetitions; an arm over it, or one that reaches --timeout, is recorded as unavailable with its reason and the run goes on (0: no budget)')
    ap.add_argument('--any-plan', action='store_true', help='the planner\'s plan at this size is what the rows show, not a failure (a smoke run: at a few thousand rows it keeps PostgreSQL\'s plan)')
    ap.add_argument('--io', choices=['cold', 'warm', 'off'], default='cold', help='the IO rows of every lane (steps/iolib.py): cold empties the case\'s tables from the shared buffers and the OS cache first (io_cold_*, then io_warm_*), warm measures the warm run only, '
                    'off writes no io_* rows. A run of a case replaces all of its rows in bench/results, so --io off (or warm) drops the io_cold_* rows the file had')
    ap.add_argument('--minstr', action='store_true', help='the user instructions of every case\'s statement (minstr rows), not only of the cases whose registry metric names them: the ledger and the instruction ceilings')
    ap.add_argument('--budget', help='fail when a case of this file (test/plan/execution.tsv) ran above its ceiling of millions of instructions (needs --minstr)')
    ap.add_argument('--budget-update', action='store_true', help='rewrite the ceilings of the cases that ran in the --budget file: the count with 15%% over it')
    ap.add_argument('--no-share', action='store_true', help='one table for every case, as before (the build of a case does not depend on the others that ran)')
    ap.add_argument('--times', help='write the seconds of every case to this file (bench/results/cases-time.tsv with --docs), without the results')
    ap.add_argument('--strict', action='store_true', help='coverage: fail (exit 1) on every gap; without it the gaps are warnings')
    ap.add_argument('--host-plan', action='store_true', help='coverage: print the commands that measure what is missing (the host window)')
    ap.add_argument('--since', help='coverage: the commits since this tag are the landed claims (default: the last tag behind HEAD)')
    ap.add_argument('--root', help='coverage: the checkout to read (default: this one)')
    ap.add_argument('--records', metavar='FILE', help='also write the measurement records (steps/records/README.md) of the query cases: each case on the arms it can answer with, warm and cold, checked on the way out')
    ap.add_argument('--docs', action='store_true')
    ap.add_argument('--cpus', type=int, default=1, help='cpus: the quota asked for (the answer is the quota the cases need, at least this)')
    ap.add_argument('--selftest', action='store_true', help='cpus: check the rule on made-up variants')
    ap.add_argument('--profile', metavar='DIR', help='load cases: perf record of the backend during the timed load statements, one CASE.LANE.data.txt (symbols by self cycles) for each lane in DIR')
    ap.add_argument('--no-wait', action='store_true')
    ap.add_argument('--wait', type=int, default=300)
    args = ap.parse_args()
    if args.profile:
        os.makedirs(args.profile, exist_ok=True)
    if args.work_mem:
        os.environ['CASES_WORK_MEM'] = args.work_mem
    sys.exit({'run': cmd_run, 'list': cmd_list, 'plan': cmd_plan, 'coverage': cmd_coverage, 'cpus': cmd_cpus}[args.cmd](args))
