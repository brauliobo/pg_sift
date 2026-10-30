#!/usr/bin/env python3
"""The benchmark cases of the optimizations (bench/cases.tsv, bench/cases/*.sql; bench/bench cases, steps/cases.sh).

    python3 -I cases.py run   [--cases ID,..] [--engines LIST] [--rows N] [--rounds R] [--runs N] [--duck-port P] [--io cold|warm|off] [--out FILE] [--docs]
    python3 -I cases.py list
    python3 -I cases.py plan  [--cases ..] [--rows N] [--scales L]      (what a run needs, no server)

A case is a row of the registry (the feature, the CHANGELOG sentence it measures, the dataset and index, the plan the scan must show, the
baselines, the README block it feeds) and a SQL file in the layout of bench/sql/store/queries (`--@ pg` the statement every lane runs, `--@ sift`
or `--@ duck` replace it for that lane, `--@ setup` DDL after the index, `-- set: a = 1; b = 2` settings of every lane, `-- base: sift.col_format_minor = 14` a lane `base` over a second copy of the tables built under those settings (what an older default would have written), `-- fresh: yes` a new backend for every timed statement (the first statement of a backend; a variant named warm.. keeps its backend), `--@ prepare` / `--@ load` for a case that times a load, `-- ordered: yes` the rows come in the order of the statement, `-- ordered: ties 3` ordered by the third column with the ties in any order: the page may differ in the rows of a tie, and `--@ ties` is the statement of all the rows with the key value of a row, `{3}`). `run` builds
each case's table in a schema of its own on the server named by PGHOST / PGPORT (steps/cases.sh starts it), asserts the plan of the sift lane from
EXPLAIN (a case that falls back to the normal plan fails), then times the lanes interleaved in rounds and compares every answer with the native one:

    sift     the planner's own choice with pg_sift
    native   the same table and statement with sift.enable_agg = off (PostgreSQL's plan on the same rows)
    <name>   a variant of the registry: the sift lane with settings (`scan:sift.agg_preagg=off`)
    pg_duckdb  a pg_duckdb server (--duck-port) holding a plain copy of the rows, duckdb.force_execution = on; NA when DuckDB does not run it

A wrong answer or a lost plan is a failure (exit 1); a case where sift is slower than native is reported as a loss, not hidden.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from cases.main import main

if __name__ == '__main__':
    main(__doc__)
