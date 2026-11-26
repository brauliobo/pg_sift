"""The catalog of the bytes-per-row gate (scripts/bpr/run.py): one frozen BprCase a measured path, and the ceilings its measurements may not pass.

A case builds in setup (outside the window), measures `op`, and optionally `maint` (the settle: VACUUM, sift.maintain) and `idle` (a window that must write nothing).
`{dir}` in a statement is the directory of the run (the COPY file); a `-- @hold_snapshot: SQL` line in setup keeps a REPEATABLE READ snapshot open while `op` runs.
`open` names the debt (W-NN or HIST-NN of Spec 104) of a case whose ceilings are the targets of a fix not yet made: it must fail them until the fix, and passes as
OPEN; the case that stops failing them is reported as FAIL until its `open` is dropped.
"""
from __future__ import annotations

from dataclasses import dataclass

ENGINES = ('table', 'col', 'bm25', 'core')
N = 16384
WIDE_COLS = 'id, grp, ts, v1, v2, amount, score, cat, txt, doc, body'


@dataclass(frozen=True)
class Ceilings:
    disk_bpr: float | None = None
    total_disk_bpr: float | None = None
    used_bpr: float | None = None
    alloc_overhead: float | None = None
    wal_bpr: float | None = None
    sift_wal_bpr: float | None = None
    fpi_bpr: float | None = None
    heap_wal_bpr: float | None = None
    wal_recs_per_row: float | None = None
    temp_bpr: float | None = 0.0
    vs_base_disk: float | None = None
    vs_base_wal: float | None = None
    vs_f1_disk: float | None = None
    vs_f1_wal: float | None = None
    post_maint_disk_bpr: float | None = None
    maint_wal_bpr: float | None = None
    idle_wal_bytes: float | None = None


@dataclass(frozen=True)
class BprCase:
    id: str
    engine: str
    rows: int
    rel: str
    setup: str
    op: str
    limits: Ceilings
    maint: str = ''
    idle: str = ''
    base_rel: str = ''
    base_setup: str = ''
    base_op: str = ''
    settings: str = ''
    include_indexes: bool = False
    dual_format: bool = False
    verify: bool = True
    open: str = ''


CASES = (
    BprCase('tbl_copy_wide', 'table', N, 't',
            setup='CREATE TABLE t (LIKE bpr_base_wide) USING sift_table',
            op="COPY t FROM '{dir}/copy_wide.tsv'", maint='VACUUM t',
            limits=Ceilings(disk_bpr=25.0, wal_bpr=18.0, post_maint_disk_bpr=25.0)),
    BprCase('col_create_serial', 'col', N, 't_ix',
            setup='CREATE TABLE t AS SELECT * FROM bpr_base_wide; VACUUM (FREEZE) t',
            op=f'CREATE INDEX t_ix ON t USING sift_col ({WIDE_COLS})',
            limits=Ceilings(disk_bpr=21.0, wal_bpr=15.5)),
    BprCase('tbl_idle_vac_zero_wal', 'table', N, 't',
            setup='CREATE TABLE t (LIKE bpr_base_wide) USING sift_table',
            op='INSERT INTO t SELECT * FROM bpr_base_wide', maint="VACUUM t; SELECT sift.maintain('t')", idle='VACUUM t',
            open='W-01', limits=Ceilings(idle_wal_bytes=0)),
    BprCase('f2_vs_f1_parity_copy', 'table', N, 't',
            setup='CREATE TABLE t (LIKE bpr_base_wide) USING sift_table',
            op="COPY t FROM '{dir}/copy_wide.tsv'", dual_format=True,
            open='HIST-34', limits=Ceilings(disk_bpr=25.0, wal_bpr=18.0)),
)
