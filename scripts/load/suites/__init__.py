"""The suites of the load gate beside the core catalog (cases.py): workloads (transactions, analytics under a tight work_mem, online DDL, features, pooled sessions, the parallel pool) on a
cluster sized for many sessions and prepared transactions (test/t/core/load_workloads.sh), and repl (a hot standby and its promotion, logical decoding to a subscriber) on a primary
with wal_level logical (test/t/core/load_repl.sh). A case names the Spec 107 finding it waits for in `open` (S107-NN), as a core case names one of Spec 106.
"""
from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import cases as catalog  # noqa: E402
from suites import repl, workloads_olap, workloads_txn  # noqa: E402
from suites.common import selftest  # noqa: E402,F401

SUITES = {
    'workloads': (workloads_txn.CASES + workloads_olap.CASES, ('delta', 'txn', 'ddl', 'pool', 'olap', 'feat', 'mix')),
    'repl': (repl.CASES, ('repl',)),
}
CLASSES = tuple(dict.fromkeys(catalog.CLASSES + tuple(c for _, classes in SUITES.values() for c in classes)))


def cases(suite: str) -> tuple:
    """the cases of SUITE (core, a name of SUITES, or all: the crash case of core still runs last, sweep() orders it)"""
    core = catalog.CASES if suite in ('core', 'all') else ()
    return core + tuple(c for name, (cs, _) in SUITES.items() if suite in ('all', name) for c in cs)
