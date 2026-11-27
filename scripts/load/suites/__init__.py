"""The suites of the load gate beside the core catalog (cases.py): workloads (transactions, analytics under a tight work_mem, online DDL, features, pooled sessions, the parallel pool) on a
cluster sized for many sessions and prepared transactions (test/t/core/load_workloads.sh), and repl (a hot standby and its promotion, logical decoding to a subscriber) on a primary
with wal_level logical (test/t/core/load_repl.sh); workloads_next adds the cases of Spec 111 to both, workloads_prod2 those of Spec 114, workloads_prod3 those of Spec 115,
workloads_prod4 those of Spec 116; stress (workloads_prod5, the cases of Spec 117) runs on test/t/core/load_stress.sh, and workloads_prod6 adds those of Spec 118 to
stress and repl. A case names the finding it waits for in `open` (Snnn-NN), as a core case names one of Spec 106.
"""
from __future__ import annotations

import sys

sys.dont_write_bytecode = True

import cases as catalog  # noqa: E402
from suites import repl, workloads_next, workloads_olap, workloads_prod2, workloads_prod3, workloads_prod4, workloads_prod5, workloads_prod6, workloads_txn  # noqa: E402
from suites.common import selftest  # noqa: E402,F401

SUITES = {
    'workloads': (workloads_txn.CASES + workloads_olap.CASES + workloads_next.WORKLOADS + workloads_prod2.WORKLOADS + workloads_prod3.WORKLOADS
                  + workloads_prod4.WORKLOADS,
                  ('delta', 'txn', 'ddl', 'pool', 'olap', 'feat', 'mix', 'conc', 'tenant', 'fault')),
    'stress': (workloads_prod5.STRESS + workloads_prod6.STRESS, ('txn', 'olap', 'tenant', 'mix', 'ddl', 'fault', 'feat', 'bp', 'pool')),
    'repl': (repl.CASES + workloads_next.REPL + workloads_prod2.REPL + workloads_prod3.REPL + workloads_prod4.REPL + workloads_prod6.REPL, ('repl',)),
}
CLASSES = tuple(dict.fromkeys(catalog.CLASSES + tuple(c for _, classes in SUITES.values() for c in classes)))


def cases(suite: str) -> tuple:
    """the cases of SUITE (core, a name of SUITES, or all: the crash case of core still runs last, sweep() orders it)"""
    core = catalog.CASES if suite in ('core', 'all') else ()
    return core + tuple(c for name, (cs, _) in SUITES.items() if suite in ('all', name) for c in cs)
