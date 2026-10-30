# The command line of cases.py.
# The docstring of bench/steps/cases.py is its help text.

import argparse
import os
import sys
import time

from . import runcfg
from .runcfg import BENCH, MIN_RUNS, REPO
from .run import cmd_list, cmd_plan, cmd_run


def main(doc):
    ap = argparse.ArgumentParser(description=doc.split('\n')[0])
    ap.add_argument('cmd', choices=['run', 'list', 'plan'])
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
    ap.add_argument('--any-plan', action='store_true', help='the planner\'s plan at this size is what the rows show, not a failure (a smoke run: at a few thousand rows it keeps PostgreSQL\'s plan)')
    ap.add_argument('--io', choices=['cold', 'warm', 'off'], default='cold', help='the IO rows of every lane (steps/iolib.py): cold empties the case\'s tables from the shared buffers and the OS cache first (io_cold_*, then io_warm_*), warm measures the warm run only, '
                    'off writes no io_* rows. A run of a case replaces all of its rows in bench/results, so --io off (or warm) drops the io_cold_* rows the file had')
    ap.add_argument('--minstr', action='store_true', help='the user instructions of every case\'s statement (minstr rows), not only of the cases whose registry metric names them: the ledger and the instruction ceilings')
    ap.add_argument('--no-share', action='store_true', help='one table for every case, as before (the build of a case does not depend on the others that ran)')
    ap.add_argument('--times', help='write the seconds of every case to this file (bench/results/cases-time.tsv with --docs), without the results')
    ap.add_argument('--docs', action='store_true')
    ap.add_argument('--no-wait', action='store_true')
    ap.add_argument('--wait', type=int, default=300)
    args = ap.parse_args()
    if args.work_mem:
        os.environ['CASES_WORK_MEM'] = args.work_mem
    sys.exit({'run': cmd_run, 'list': cmd_list, 'plan': cmd_plan}[args.cmd](args))
