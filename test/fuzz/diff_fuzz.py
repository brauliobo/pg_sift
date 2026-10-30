#!/usr/bin/env python3
"""Differential fuzz of the aggregate pushdown (docs/AGG.md, "Differential suite").

A seed gives a database (test/fuzz/diff_fuzz_data.sql: small tables, NULLs, duplicates, a sift_col index on each fact table) and a script
of episodes: a few DML statements (updates, deletes, inserts: dirty rows and write-buffer rows) and then random queries of the shapes of
the pushdown (first-row lookups, two-level aggregates, partition pruning, pre-aggregates, dirty-row shapes, UNION ALL arms, top-N). Each
query runs under the plan PostgreSQL has without sift (sift.enable_agg off, the reference) and under variants of the pushdown (the scan,
the pre-aggregates, the arms alone, parallel workers, ...); the results are compared exactly, floats within 1e-12 + 1e-9 relative
(test/col/run.sql), rows by position only when the query orders them totally. A query that raises must raise the same SQLSTATE everywhere.

  test/fuzz/diff_fuzz.py --seeds 1-3 --queries 40 --rows 3000        the quick tier (scripts/ci.sh, job difftest-fuzz)
  test/fuzz/diff_fuzz.py --seeds 100-199 --queries 300 --out DIR     a long search: a reproducer per divergence in DIR (shrunk)
  test/fuzz/diff_fuzz.py --replay FILE                               runs a reproducer written by --out again
  test/fuzz/diff_fuzz.py --table --seeds 1-3 --queries 40            the same on sift_tables (scripts/ci.sh, job difftest-fuzz-tbl)
  test/fuzz/diff_fuzz.py --selftest                                  no server: every family generates and renders in both modes (the lint job)

--table rewrites the fact tables USING sift_table (no sift_col index; the jsonb paths are declared with sift.table_set_paths) and runs the DML on them
(updates, deletes, inserts, rolled back writes, VACUUM [FULL], sift.table_maintain: tombstones, write buffers, unfrozen groups); the reference is the
normal plan on the same table. The families that need an option of the column index are skipped (TABLE_SKIP).

Needs the scripts/pgtest.sh cluster (SIFT_TMP, SIFT_PORT) installed and started. Exit 1 on a divergence or a query the generator got wrong.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from diff_fuzz.main import main

if __name__ == '__main__':
    sys.exit(main(__doc__))
