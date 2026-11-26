"""The catalog of the bytes-per-row gate (scripts/bpr/run.py): one frozen BprCase a measured path, and the ceilings its measurements may not pass.

A case builds in setup (outside the window), measures `op`, and optionally `maint` (the settle: VACUUM, sift.maintain), `idle` (a window that must write nothing) and `cold` (the same after a restart of the server).
`{dir}` in a statement is the directory of the run (the COPY files); a `-- @hold_snapshot: SQL` line in setup keeps a REPEATABLE READ snapshot open while `op` and `maint` run.
`rows` is what the bytes are divided by (`maint_rows` for the settle window); `grow_bpr` is the growth of the relation over `op`.
`open` names the debt (W-NN or HIST-NN of Spec 104) of a case whose ceilings are the targets of a fix not yet made: it must fail them until the fix, and passes as
OPEN; the case that stops failing them is reported as FAIL until its `open` is dropped.
"""
from __future__ import annotations

from dataclasses import dataclass

ENGINES = ('table', 'col', 'bm25', 'core')
N = 16384
SPREAD = 0.005
WIDE_COLS = 'id, grp, ts, v1, v2, amount, score, cat, txt, doc, body'


@dataclass(frozen=True)
class Ceilings:
    disk_bpr: float | None = None
    total_disk_bpr: float | None = None
    grow_bpr: float | None = None
    used_bpr: float | None = None
    alloc_overhead: float | None = None
    wal_bpr: float | None = None
    sift_wal_bpr: float | None = None
    sift_fpi_bpr: float | None = None
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
    maint_sift_wal_bpr: float | None = None
    idle_wal_bytes: float | None = None
    cold_wal_bytes: float | None = None


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
    cold: str = ''
    maint_rows: int = 0
    base_rel: str = ''
    base_setup: str = ''
    base_op: str = ''
    settings: str = ''
    include_indexes: bool = False
    dual_format: bool = False
    verify: bool = True
    testing: bool = False
    wal_level: str = 'replica'
    spread: float = SPREAD
    open: str = ''


def per_tx(n: int, stmt: str) -> str:
    return f'DO $$ BEGIN FOR i IN 1..{n} LOOP {stmt}; COMMIT; END LOOP; END $$'


def batches(sizes: tuple, into: str = 't', src: str = 'bpr_base_wide', start: int = 0) -> str:
    edges = [start + sum(sizes[:i]) for i in range(len(sizes) + 1)]
    return '; '.join(f'INSERT INTO {into} SELECT * FROM {src} WHERE id > {a} AND id <= {b}' for a, b in zip(edges, edges[1:]))


def wide_index(name: str = 't_ix', on: str = 't', cols: str = WIDE_COLS, opts: str = '') -> str:
    return f'CREATE INDEX {name} ON {on} USING sift_col ({cols})' + (f' WITH ({opts})' if opts else '')


TRACK = "track_changes = 'ctid'"
DOC_PATHS = "paths = 'doc.k int4, doc.flag bool'"
TBL = 'CREATE TABLE t (LIKE bpr_base_wide) USING sift_table'
HEAP = 'CREATE TABLE t AS SELECT * FROM bpr_base_wide; VACUUM (FREEZE) t'
HEAP_IDX = f'{HEAP}; {wide_index()}'
EMPTY_IDX = f"CREATE TABLE t (LIKE bpr_base_wide); {wide_index(opts=DOC_PATHS)}"
TBL_FULL = f"{TBL}; INSERT INTO t SELECT * FROM bpr_base_wide; VACUUM t; SELECT sift.maintain('t')"
WIDE_UPD = ("UPDATE t SET grp = grp + 1, v1 = v1 + 1, v2 = v2 + 1, amount = amount + 1, score = score + 1, cat = cat || 'x', "
            "txt = txt || 'y', body = body || 'z'")
COPY_WIDE = "COPY t FROM '{dir}/copy_wide.tsv'"
SETTLE = "VACUUM t; SELECT sift.maintain('t')"
BM25 = 'CREATE INDEX t_ix ON t USING sift (body)'
TEXT = 'CREATE TABLE t AS SELECT * FROM bpr_base_text; VACUUM (FREEZE) t'
READ = "SELECT count(*) FROM (SELECT id FROM t WHERE body @@> 'wire AND switch' ORDER BY sift.score(ctid) DESC LIMIT 10) r"
PROJ = "SELECT sift.create_projection('t_ix', 'by_grp', 'group by grp measures count(*), sum(v1)')"
SORT = "SELECT sift.create_projection('t_ix', 'by_v2', 'order by v2, id include (grp)')"
PAIRS = 'CREATE TABLE pairs (member int NOT NULL, key int NOT NULL, PRIMARY KEY (member, key)); INSERT INTO pairs SELECT a, b FROM bpr_pairs'
SCOPE_SRC = 'SELECT id, 1 + id % 2048 AS member, ts, v1, v2 FROM bpr_base_wide'
SCOPE_COLS = 'id, member, ts, v1, v2'
SCOPE_OPTS = f"scope = 'k: member -> pairs(member, key)', {TRACK}"
L0_PARTS = (4779, 4779, 4778)
TINY_PARTS = """CREATE TABLE t (v int); CREATE INDEX t_ix ON t USING sift_store_test (v); SELECT sift._test_f2_init('t_ix');
SELECT sift._test_f2_split_many('t_ix', 1, NULL, 15, 1000);
SELECT count(sift._test_f2_add_part('t_ix', part_id, key_lo, 8, COALESCE(key_lo, 0) + 10, part_id * 100, 0, 1)) FROM sift._test_f2_partitions('t_ix')"""

CASES = (
    BprCase('tbl_copy_wide', 'table', N, 't', setup=TBL, op=COPY_WIDE, maint='VACUUM t', dual_format=True,
            open='W-05,W-10,HIST-34', limits=Ceilings(disk_bpr=25.0, wal_bpr=18.0, post_maint_disk_bpr=25.0, maint_wal_bpr=0.0)),
    BprCase('tbl_ctas_ins_sel', 'table', N, 't', setup='SELECT 1',
            op='CREATE TABLE t USING sift_table AS SELECT * FROM bpr_base_wide WHERE id <= 8192; '
               'INSERT INTO t SELECT * FROM bpr_base_wide WHERE id > 8192', maint='VACUUM t',
            open='W-10', limits=Ceilings(disk_bpr=25.0, wal_bpr=18.92, maint_wal_bpr=0.0)),
    BprCase('tbl_copy_minimal', 'table', N, 't', setup='SELECT 1', wal_level='minimal', op=f'BEGIN; {TBL}; {COPY_WIDE}; COMMIT',
            open='W-05', limits=Ceilings(disk_bpr=25.0, wal_bpr=1.0)),
    BprCase('tbl_copy_freeze', 'table', N, 't', setup='SELECT 1',
            op=f"BEGIN; {TBL}; COPY t FROM '{{dir}}/copy_wide.tsv' WITH (FREEZE); COMMIT", maint='VACUUM t',
            open='W-05', limits=Ceilings(disk_bpr=25.0, wal_bpr=18.91, maint_wal_bpr=0.0)),
    BprCase('tbl_copy_wide_blobs', 'table', N // 8, 't', setup='CREATE TABLE t (LIKE bpr_base_blob) USING sift_table',
            op="COPY t FROM '{dir}/copy_blob.tsv'",
            open='W-12', limits=Ceilings(alloc_overhead=1.14, wal_recs_per_row=0.25)),
    BprCase('tbl_batch_ins_1k_10k', 'table', N, 't', setup=TBL, op=batches((1024,) * 6 + (10240,)),
            maint="SELECT sift.maintain('t')", dual_format=True,
            limits=Ceilings(disk_bpr=52.32, wal_bpr=44.11, maint_wal_bpr=31.09)),
    BprCase('tbl_oltp_1row', 'table', 200, 't', setup=TBL,
            op=per_tx(200, 'INSERT INTO t SELECT * FROM bpr_base_wide WHERE id = i'), dual_format=True,
            open='W-09', limits=Ceilings(wal_bpr=273.73, vs_f1_wal=0.70)),
    BprCase('tbl_upd_narrow_int', 'table', N, 't', setup=TBL_FULL, op='UPDATE t SET v2 = v2 + 1',
            limits=Ceilings(grow_bpr=7.5, wal_bpr=6.2)),
    BprCase('tbl_upd_narrow_str_null', 'table', N, 't', setup=TBL_FULL,
            op="UPDATE t SET txt = CASE WHEN id % 4 = 0 THEN NULL ELSE 'shipped' END",
            open='W-07', limits=Ceilings(grow_bpr=18.0, wal_bpr=24.0, fpi_bpr=6.0)),
    BprCase('tbl_upd_wide_all', 'table', N, 't', setup=TBL_FULL, op=WIDE_UPD, maint=SETTLE,
            limits=Ceilings(wal_bpr=55.64, maint_wal_bpr=166.16, post_maint_disk_bpr=27.02)),
    BprCase('tbl_upd_rr_snapshot', 'table', 800, 't', maint=WIDE_UPD, maint_rows=N,
            setup=f'{TBL_FULL}; ALTER TABLE t ADD PRIMARY KEY (id); CREATE INDEX t_grp ON t (grp);\n-- @hold_snapshot: SELECT count(*) FROM t\n',
            op=per_tx(800, 'UPDATE t SET grp = grp + 1 WHERE id = i'),
            open='W-06', limits=Ceilings(sift_fpi_bpr=77.5, maint_sift_wal_bpr=55.0)),
    BprCase('tbl_del_50_vac_shrink', 'table', N, 't', setup=TBL_FULL, op='DELETE FROM t WHERE id % 2 = 0', maint=SETTLE,
            limits=Ceilings(wal_bpr=1.98, maint_wal_bpr=24.07, post_maint_disk_bpr=16.0)),
    BprCase('tbl_idle_vac_zero_wal', 'table', N, 't', setup=TBL, op='INSERT INTO t SELECT * FROM bpr_base_wide', maint=SETTLE,
            idle='VACUUM t',
            limits=Ceilings(idle_wal_bytes=0)),
    BprCase('tbl_aux_sec_overhead', 'table', N, 't', setup=f'{TBL}; ALTER TABLE t ADD PRIMARY KEY (id)', op=COPY_WIDE, maint=SETTLE,
            base_rel='tb', base_setup='CREATE TABLE tb (LIKE bpr_base_wide) USING sift_table',
            base_op="COPY tb FROM '{dir}/copy_wide.tsv'",
            limits=Ceilings(disk_bpr=40.25, total_disk_bpr=67.27, vs_base_disk=1.5)),

    BprCase('col_create_serial', 'col', N, 't_ix', setup=HEAP, op=wide_index(), dual_format=True,
            open='HIST-34', limits=Ceilings(disk_bpr=21.28, wal_bpr=15.84)),
    BprCase('col_create_parallel', 'col', N, 't_ix', setup=f'{HEAP}; ALTER TABLE t SET (parallel_workers = 4)',
            settings='SET max_parallel_maintenance_workers = 4', op=wide_index(), maint='REINDEX INDEX CONCURRENTLY t_ix',
            spread=0.30,
            limits=Ceilings(disk_bpr=32.0, wal_bpr=29.0)),
    BprCase('col_copy_wide_bulk', 'col', N, 't_ix', setup=EMPTY_IDX, settings="SET work_mem = '256kB'", op=COPY_WIDE,
            maint="SELECT sift.maintain('t_ix')",
            open='W-02', limits=Ceilings(disk_bpr=26.0, sift_wal_bpr=28.0, maint_sift_wal_bpr=5.0)),
    BprCase('col_batch_ins_1k_10k', 'col', N, 't_ix', setup=EMPTY_IDX, op=batches((1024,) * 6 + (10240,)), maint='VACUUM t',
            limits=Ceilings(disk_bpr=32.0, sift_wal_bpr=26.82, maint_sift_wal_bpr=52.76)),
    BprCase('col_oltp_1row', 'col', 200, 't_ix', setup=HEAP_IDX,
            op=per_tx(200, 'INSERT INTO t SELECT * FROM bpr_base_wide WHERE id = i'), dual_format=True,
            limits=Ceilings(sift_wal_bpr=100.35)),
    BprCase('col_upd_narrow_tracked', 'col', N, 't_ix', setup=f'{HEAP}; {wide_index(opts=TRACK)}', op='UPDATE t SET v2 = v2 + 1',
            limits=Ceilings(grow_bpr=14.95, sift_wal_bpr=6.67)),
    BprCase('col_upd_unchanged_hot', 'col', N, 't_ix', op="UPDATE t SET pad = 'x'",
            setup=f"CREATE TABLE t WITH (fillfactor = 50) AS SELECT *, ''::text AS pad FROM bpr_base_wide; VACUUM (FREEZE) t; {wide_index()}",
            limits=Ceilings(grow_bpr=1.0, sift_wal_bpr=1.17)),
    BprCase('col_upd_with_proj_uniq', 'col', N, 't_ix', op='UPDATE t SET v2 = v2 + 1',
            setup=f'{HEAP}; {wide_index(opts=TRACK)}; VACUUM t; {PROJ}; VACUUM t',
            limits=Ceilings(grow_bpr=12.65, sift_wal_bpr=6.66)),
    BprCase('col_del_50_vac_merge', 'col', N, 't_ix', setup=HEAP_IDX, op='DELETE FROM t WHERE id % 2 = 0',
            maint="VACUUM t; SELECT sift.merge('t_ix', true)",
            limits=Ceilings(post_maint_disk_bpr=16.0, maint_sift_wal_bpr=18.16)),
    BprCase('col_pack_tails', 'col', 128, 't_ix', setup=TINY_PARTS, op="SELECT sift._test_f2_pack_tails('t_ix')", testing=True,
            verify=False,
            open='W-09', limits=Ceilings(wal_bpr=256.0)),
    BprCase('col_idle_vac_zero_wal', 'col', N, 't_ix', setup=HEAP_IDX, op='SELECT 1', maint='VACUUM t; VACUUM t',
            idle='VACUUM t; SELECT count(*) FROM t WHERE grp = 3', cold='VACUUM t; SELECT count(*) FROM t WHERE grp = 3',
            limits=Ceilings(idle_wal_bytes=0, cold_wal_bytes=0)),

    BprCase('bm25_create_serial_par', 'bm25', N, 't_ix', setup=f'{TEXT}; ALTER TABLE t SET (parallel_workers = 4)', op=BM25,
            maint='SET max_parallel_maintenance_workers = 4; DROP INDEX t_ix; CREATE INDEX t_ix ON t USING sift (body) INCLUDE (grp)',
            limits=Ceilings(disk_bpr=12.07, wal_bpr=9.7, post_maint_disk_bpr=12.65, maint_wal_bpr=10.59)),
    BprCase('bm25_copy_bulk', 'bm25', N, 't_ix', setup=f'CREATE TABLE t (LIKE bpr_base_text); {BM25}',
            op="COPY t FROM '{dir}/copy_text.tsv'", maint="VACUUM t; SELECT sift.maintain('t_ix')",
            limits=Ceilings(disk_bpr=78.77, wal_bpr=129.88, wal_recs_per_row=0.036)),
    BprCase('bm25_upd_non_text', 'bm25', N, 't_ix', setup=f'{TEXT}; {BM25}', op='UPDATE t SET grp = grp + 1',
            open='W-04', limits=Ceilings(sift_wal_bpr=0.0, grow_bpr=0.0)),
    BprCase('bm25_del_50_vac_merge', 'bm25', N, 't_ix', setup=f'{TEXT}; {BM25}', op='DELETE FROM t WHERE id % 2 = 0',
            maint="VACUUM t; SELECT sift.merge('t_ix', true)",
            limits=Ceilings(post_maint_disk_bpr=10.35, maint_sift_wal_bpr=15.48)),
    BprCase('bm25_read_zero_wal', 'bm25', N, 't_ix', op='; '.join([READ] * 20),
            setup=f'CREATE TABLE t (LIKE bpr_base_text); {BM25}; INSERT INTO t SELECT * FROM bpr_base_text WHERE id <= 1500; SELECT count(*) FROM t',
            maint='INSERT INTO t SELECT * FROM bpr_base_text WHERE id > 1500 AND id <= 3500; SELECT count(*) FROM t', idle='; '.join([READ] * 20),
            limits=Ceilings(wal_bpr=0.0, idle_wal_bytes=0)),

    BprCase('proj_build_sort_agg', 'col', N, 't_ix', spread=0.01, setup=f'{HEAP_IDX}; VACUUM t', op=f'{PROJ}; {SORT}; VACUUM t',
            limits=Ceilings(grow_bpr=13.8, sift_wal_bpr=16.6)),
    BprCase('proj_unrelated_compact', 'col', N, 't_ix', op="SELECT sift.merge('t_ix', true)", base_rel='t2_ix',
            base_op="SELECT sift.merge('t2_ix', true)",
            setup=f"CREATE TABLE t AS SELECT * FROM bpr_base_wide WHERE id <= 2048; VACUUM (FREEZE) t; {wide_index()}; VACUUM t; {PROJ}; VACUUM t; "
                  f'{batches(L0_PARTS, start=2048)}',
            base_setup=f"CREATE TABLE t2 AS SELECT * FROM bpr_base_wide WHERE id <= 2048; VACUUM (FREEZE) t2; {wide_index('t2_ix', 't2')}; VACUUM t2; "
                       f"{batches(L0_PARTS, 't2', start=2048)}",
            open='W-08', limits=Ceilings(sift_wal_bpr=33.25, vs_base_disk=1.0, vs_base_wal=1.0)),
    BprCase('proj_churn_refresh', 'col', 3277, 't_ix', setup=f'{HEAP_IDX}; VACUUM t; {PROJ}; VACUUM t',
            op='UPDATE t SET v1 = v1 + 1 WHERE id % 10 = 0; DELETE FROM t WHERE id % 10 = 1; VACUUM t',
            limits=Ceilings(grow_bpr=31.62, sift_wal_bpr=167.02)),
    BprCase('scope_build_and_churn', 'col', N, 't_ix', maint_rows=600, spread=0.01, base_rel='t2_ix',
            setup=f'{PAIRS}; CREATE TABLE t AS {SCOPE_SRC}; VACUUM (FREEZE) t', op=wide_index(cols=SCOPE_COLS, opts=SCOPE_OPTS),
            maint='INSERT INTO pairs SELECT member, key + 1 FROM pairs WHERE member <= 100; UPDATE pairs SET key = key + 2 WHERE member <= 100; '
                  'DELETE FROM pairs WHERE member <= 50; UPDATE t SET v1 = v1 + 1 WHERE id <= 600; VACUUM t',
            base_setup='CREATE TABLE t2 AS SELECT * FROM t; VACUUM (FREEZE) t2', base_op=wide_index('t2_ix', 't2', SCOPE_COLS, TRACK),
            open='W-13', limits=Ceilings(disk_bpr=13.23, vs_base_disk=1.05, maint_sift_wal_bpr=123.6)),
    BprCase('scope_unrelated_compact', 'col', N, 't_ix', op="SELECT sift.merge('t_ix', true)", base_rel='t2_ix',
            base_op="SELECT sift.merge('t2_ix', true)",
            setup=f'{PAIRS}; CREATE TABLE t AS {SCOPE_SRC} WHERE id <= 2048; VACUUM (FREEZE) t; {wide_index(cols=SCOPE_COLS, opts=SCOPE_OPTS)}; '
                  f'VACUUM t; {batches(L0_PARTS, src=f"({SCOPE_SRC}) w", start=2048)}',
            base_setup=f'CREATE TABLE t2 AS {SCOPE_SRC} WHERE id <= 2048; VACUUM (FREEZE) t2; {wide_index("t2_ix", "t2", SCOPE_COLS, TRACK)}; '
                       f'VACUUM t2; {batches(L0_PARTS, "t2", src=f"({SCOPE_SRC}) w", start=2048)}',
            idle='SELECT count(*), sum(v1) FROM t WHERE member BETWEEN 10 AND 90',
            open='W-13', limits=Ceilings(sift_wal_bpr=8.02, vs_base_disk=1.05, vs_base_wal=1.05, idle_wal_bytes=0)),
    BprCase('f2_vs_f1_parity_matrix', 'col', N, 't_ix', setup=EMPTY_IDX, dual_format=True,
            op=f"{COPY_WIDE}; {batches((4096,))}; {per_tx(100, 'INSERT INTO t SELECT * FROM bpr_base_wide WHERE id = i')}; "
               f"UPDATE t SET v2 = v2 + 1 WHERE id <= 4096; {wide_index('t_ix2')}; VACUUM t",
            open='HIST-34', limits=Ceilings(grow_bpr=24.15, sift_wal_bpr=122.45)),
)
