#!/usr/bin/env python3
"""Differential fuzz of transactions on a sift_col index (docs/AGG.md, "Transactions").

  test/fuzz/txn_fuzz.py [--seeds 1-20] [--actions 60] [--tracks auto,key,off,ctid] [--out DIR] [--stop-after N] [--budget SEC] [--no-shrink]
  test/fuzz/txn_fuzz.py --shapes [--only cursor|prep|persist] [--parts 0|3] [--concurrently] [--variants scan,default,index,workers]   cursors, prepared statements, partitions, upserts with SPI (docs/AGG.md)
  test/fuzz/txn_fuzz.py --repro DIR/txn_fuzz_N.json | --selftest

Needs the scripts/pgtest.sh cluster (SIFT_TMP, SIFT_PORT); max_prepared_transactions > 0 adds prepared transactions to the scripts.

A seed gives a table t(k, tenant, amount, pad) with an int or text key and a sift_col index (track_changes auto, on a key, 'ctid' or off; rows in the write buffer
or not, promoted or not, vacuumed or not; a trigger on t or none) and a script of actions: statements of a session A (INSERT, UPDATE of the same rows several times,
of a key, of a column the index does not hold, DELETE, INSERT .. ON CONFLICT, MERGE, COPY, data-modifying CTEs, a volatile function that writes through SPI, a DO block
with an exception block, SAVEPOINT / ROLLBACK TO / RELEASE, a cursor and WHERE CURRENT OF, PREPARE TRANSACTION and its resolution, READ COMMITTED and REPEATABLE READ),
statements of a session B that writes its own rows and holds a REPEATABLE READ snapshot open, VACUUM, sift.col_promote and reconnects. After a statement, at the end of a
transaction and at the end, a check runs the same grouped aggregates, point lookups, top-N and range count as the plain heap plan (the reference: no scan, no index) and as
the scan forced, the default plan, an index scan and the scan with workers, in the session that wrote (inside its transaction) and in a fresh backend. The first query whose
rows differ ends the run, and the script is cut down (delta debugging: actions, rows, the trigger, the write buffer) to a reproducer: DIR/txn_fuzz_N.sql and .json.
"""
import os
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from txn_fuzz import main  # noqa: E402

if __name__ == '__main__':
    sys.exit(main(__doc__))
