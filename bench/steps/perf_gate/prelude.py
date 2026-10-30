# The imports, the paths of the harness and the limits of a run (pairs, margins, wall limits, the insert pilot).
# The parts of this directory run in the namespace of bench/steps/perf_gate.py, in this order.

import argparse
import ctypes
import contextlib
import datetime
import hashlib
import json
import math
import os
from pathlib import Path
import platform
import re
import shutil
import signal
import statistics
import subprocess
import sys
import tarfile
import tempfile
import time

ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(ROOT / 'bench/perf'))
sys.path.insert(0, str(ROOT / 'bench/steps'))
import cell
from catalog import CASES, select

GATING = {c.name for c in CASES if c.gate}

INITIAL_PAIRS = 6
READ_CONFIRMATION_PAIRS = 16
MUTATION_CONFIRMATION_PAIRS = 6
# An inconclusive confirmation (neither inside the margin nor clearly outside it) is a noise-limited case: one more fresh block of the
# widest supported size decides it when the budget still covers it and the cases after it. Measured: a mutation case with its blocks
# runs 25-50 s on the hosted runner, a settled 6-pair INSERT block has a pair log-ratio SD of 0.09 (95% half-width 0.09 ~ the margin).
EXTENSION_PAIRS = 16
CASE_RESERVE_SECONDS = 30
TARGET = 0.150
MATERIAL = 1.10
T95 = {6:  2.570581835636305,
       16: 2.131449545559323}
DEADLINE = math.inf
PROCESS_ORIGIN = time.monotonic()
BATCH_WALL_LIMIT = 1.0
# the nightly fixture (32768 rows) takes 1.85 s to restore on a hosted runner, more than the one second of the pull request tier
NIGHTLY_BATCH_WALL_LIMIT = 4.0
INSERT_PILOT = 2048
INSERT_GROWTH_CAP = 2048
INSERT_HISTORY_CAP = 5
CPU_PILOT_ROWS = 32
CPU_PILOT_ITERATIONS = 16
CPU_ITERATION_CAP = 1024
SETTINGS = '''SET statement_timeout='30s'; SET max_parallel_workers_per_gather=0;
SET max_parallel_maintenance_workers=0; SET jit=off; SET sift.async_maintenance=off;
SET sift.table_write_buffer_limit=2048; SET cpu_tuple_cost=0.5; SET enable_seqscan=off; SET sift.enable_agg=on; SET sift.agg_preagg=on'''
