# The paths, datasets and limits of a cases run, and its logger.
# TIMEOUT and LANE_BUDGET are set by cmd_run from the command line and read as settings.TIMEOUT.

import os
import re
import sys

import benchlib


HERE = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
sys.path.insert(0, HERE)

BENCH = os.path.dirname(HERE)
REPO = os.path.dirname(BENCH)
GEN = os.path.join(BENCH, 'sql', 'store', 'gen.sql')
EVENTS_GEN = os.path.join(REPO, 'test', 'plan', 'gen.sql')   # the events dataset of the plan-choice gate: one schema, c_events, for every case of the dataset
DATASETS = {'events': ('c_events', EVENTS_GEN), 'keys': ('c_keys', os.path.join(BENCH, 'sql', 'keys', 'gen.sql')),
            'tpch': ('c_tpch', os.path.join(BENCH, 'sql', 'tpch', 'gen.sql')),
            'hits': ('c_hits', os.path.join(BENCH, 'sql', 'hits', 'gen.sql')),
            'tseries': ('c_tseries', os.path.join(BENCH, 'sql', 'tseries', 'gen.sql'))}   # the datasets a case shares: schema and generator
BUILT = set()
COLUMNS = ['id', 'customer_id', 'country', 'category', 'status', 'amount', 'quantity', 'created_at', 'title']
TYPES = ['bigint', 'int', 'text', 'text', 'text', 'numeric(10,2)', 'smallint', 'timestamp', 'text']
DERIVED = ['segment_id', 'order_date']   # int with 6,313 values and the date of the order: the keys of the key-presence and calendar cases
INDEX_COLUMNS = ', '.join(COLUMNS[1:8] + DERIVED)
DB = 'cases'
FIELDS = ['id', 'changelog', 'dataset', 'index', 'plan', 'baselines', 'variants', 'metric', 'block', 'headline']
TIMEOUT = 60
LANE_BUDGET = 20.0   # seconds of timed runs after which a lane stops repeating (it has MIN_RUNS or more); --lane-budget
MIN_RUNS = 3
SCAN = re.compile(r'Custom Scan \(Sift')
FORCED = ('agg_forced', 'sift.agg_force = on')   # the lane of a fallback case: what the scan can do where the planner declines it
TIMES = {}   # case id -> [build seconds, run seconds] of the last point
IO_HOW = set()   # how the cold runs were made cold (iolib.cool), for the meta lines of the results file


log, die = benchlib.logger('cases')
