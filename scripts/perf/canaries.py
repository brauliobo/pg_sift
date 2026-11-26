"""The 22 black-box canaries of the PR gate (scripts/perf/gate.py) over the fixture of scripts/perf/fixture.sql.

A canary is SQL and what a user can see of it: the plan family of EXPLAIN, the blocks and the rows it reads, the workers it launches, the instructions it retires
against the same statement with sift off (the native plan). Nothing here names a C symbol, a storage format or an executor version.
"""
from __future__ import annotations

from dataclasses import dataclass

ROWS = 64000
AREAS = ('star', 'clickbench', 'coexist', 'jsonb', 'table', 'bm25', 'gpu')

AGG_FAMILY = ('SiftAgg', 'SiftProjSortScan')
BM25_FAMILY = ('SiftScan', 'SiftHybridScan', 'SiftHybridAgg')
SIFT_FAMILY = AGG_FAMILY + BM25_FAMILY


@dataclass(frozen=True)
class Canary:
    id: str
    area: str
    mode: str                       # '1T' | '4T': max_parallel_workers_per_gather = 0 | 4, every cost left at its stock value
    sql: str
    plan: tuple = ('agg',)          # one of these families must be in the plan: agg, bm25, idx:<index>, none
    smoke: bool = False
    kind: str = 'read'              # read | copy | dml | gpu
    pre: str = ''                   # statements run before the measured one, outside the window (TRUNCATE of the table a COPY loads)
    native: str | None = None       # the baseline statement when it is not `sql` with the sift planner switches off
    native_pre: str = ''
    baseline: bool = True           # False: no statement of the native planner answers it (a BM25 ranking)
    rel: str = ''                   # the table whose bytes on disk are measured after the statement; native_rel for the baseline
    native_rel: str = ''
    blocks: int | None = None       # Shared Hit + Shared Read of the root node
    temp: int = 0                   # Temp Read + Written Blocks of every node
    heap_zero: bool = False         # Heap Fetches / Verifies / Vet Rows are 0
    no_seq: bool = False
    workers: int = 0                # 4T: at least this many Workers Planned; every parallel node launches all the workers it plans
    balance: bool = False           # a Gather: workers read >= 50% of the rows, no worker more than 60% of theirs
    ratio: float | None = None      # instructions:u of sift / native
    per_row: float | None = None    # instructions:u per fixture row
    total: int | None = None        # instructions:u of the statement
    parity: bool = True
    extra: tuple = ()               # kind specific limits: (name, ceiling)


def c(id, area, mode, sql, **kw):
    return Canary(id, area, mode, sql, **kw)


STAR2 = ("SELECT r.tier, p.brand, count(*), sum(e.amount) FROM sf_events e JOIN sf_dim_region r ON r.reg_id = e.reg_id JOIN sf_dim_product p ON p.prod_id = e.prod_id "
         "WHERE r.active AND p.price > 50 GROUP BY 1, 2 ORDER BY 1, 2")
STAR_SIP = ("SELECT p.cat_id, count(*), sum(e.qty) FROM sf_events e JOIN sf_dim_product p ON p.prod_id = e.prod_id WHERE p.cat_id IN (1, 2, 3) GROUP BY 1 ORDER BY 1")
CB_TEXT = "SELECT url, count(*) AS c, sum(amount) FROM sf_events GROUP BY url ORDER BY c DESC, url LIMIT 20"
JSON_SHRED = ("SELECT doc->>'channel' AS ch, doc->>'flag' AS flag, count(*), sum((doc->>'score')::float8) FROM sf_events WHERE reg_id <= 32 GROUP BY 1, 2 ORDER BY 1, 2")
GPU_SMALL = "SELECT status, count(*), sum(amount) FROM sf_events WHERE reg_id = 1 GROUP BY status ORDER BY status"
GPU_DENSE = "SELECT status, count(*), sum(amount), sum(qty), avg(amount) FROM sf_events GROUP BY status ORDER BY status"
DML = "BEGIN; UPDATE {t} SET amount = amount + 1 WHERE id % 20 = 0; DELETE FROM {t} WHERE id % 50 = 0; ROLLBACK"

CANARIES = (
    c('star_2dim_1t', 'star', '1T', STAR2, smoke=True, blocks=150, heap_zero=True, ratio=0.70, per_row=1260),
    c('star_2dim_4t', 'star', '4T', STAR2, blocks=150, balance=True, ratio=0.70, per_row=1260),
    c('star_sip_filter_4t', 'star', '4T', STAR_SIP, blocks=7900, ratio=1.55, per_row=1120),
    c('cb_text_groupby_1t', 'clickbench', '1T', CB_TEXT, smoke=True, blocks=140, ratio=0.40, per_row=820),
    c('cb_text_groupby_4t', 'clickbench', '4T', CB_TEXT, blocks=140, balance=True, ratio=0.40, per_row=820),
    c('cb_like_substr_4t', 'clickbench', '4T', "SELECT status, count(*), sum(qty) FROM sf_events WHERE url LIKE '%google%' GROUP BY status ORDER BY status",
      blocks=160, ratio=0.20, per_row=310),
    c('cb_topk_order_4t', 'clickbench', '4T', "SELECT id, event_ts, amount FROM sf_events WHERE reg_id <= 16 ORDER BY event_ts DESC, id DESC LIMIT 20",
      blocks=80, ratio=0.30, per_row=170),
    c('cb_point_seek_1t', 'clickbench', '1T', "SELECT id, reg_id, amount, status FROM sf_events WHERE reg_id = 7 AND event_ts = (SELECT min(event_ts) FROM sf_events WHERE reg_id = 7)",
      plan=('none',), no_seq=True, blocks=40, ratio=0.55, total=3500000),
    c('tpch_btree_pk_1t', 'coexist', '1T', "SELECT id, reg_id, amount FROM sf_events WHERE id = 42197", plan=('idx:sf_events_pkey',), smoke=True, no_seq=True, blocks=10, ratio=1.05,
      total=300000),
    c('tpch_btree_sel_range_1t', 'coexist', '1T', "SELECT id, amount FROM sf_events WHERE reg_id = 5 AND day BETWEEN DATE '2024-06-01' AND DATE '2024-06-05' ORDER BY id",
      plan=('idx:sf_events_btree_day_reg', 'idx:sf_events_col', 'agg'), no_seq=True, blocks=30, ratio=7.35, total=3700000, extra=(('Heap Fetches', 60),)),
    c('tpch_q6_analytical_4t', 'coexist', '4T', "SELECT status, count(*), sum(amount), sum(qty), avg(metric_num) FROM sf_events WHERE day >= DATE '2024-04-01' AND day < DATE '2025-04-01' "
      "AND qty BETWEEN 2 AND 7 GROUP BY status ORDER BY status", blocks=220, ratio=0.55, per_row=770),
    c('jsonb_shred_agg_1t', 'jsonb', '1T', JSON_SHRED, smoke=True, blocks=60, heap_zero=True, ratio=0.30, per_row=700),
    c('jsonb_shred_agg_4t', 'jsonb', '4T', JSON_SHRED, blocks=60, ratio=0.30, per_row=700),
    c('jsonb_unnest_agg_1t', 'jsonb', '1T', "SELECT elem->>'kind' AS k, count(*), sum((elem->>'score')::float8) FROM sf_events, LATERAL jsonb_array_elements(doc->'items') AS elem "
      "WHERE doc IS NOT NULL GROUP BY 1 ORDER BY 1", plan=('none',), blocks=4300, ratio=1.30, per_row=11400),
    c('jsonb_null_presence_1t', 'jsonb', '1T', "SELECT count(*) FROM sf_events WHERE doc ? 'flag'", blocks=100, heap_zero=True, ratio=0.15, per_row=180),
    c('tbl_bulk_copy_64k', 'table', '1T', "COPY tbl_copy_stage FROM '{copy_tsv}'", plan=('none',), smoke=True, kind='copy', pre='TRUNCATE tbl_copy_stage', rel='tbl_copy_stage',
      native="COPY tbl_copy_heap FROM '{copy_tsv}'", native_pre='TRUNCATE tbl_copy_heap', native_rel='tbl_copy_heap', ratio=2.05, per_row=69300,
      extra=(('bytes_per_row', 25), ('wal_per_row', 25)), parity=False),
    c('tbl_point_pk_1t', 'table', '1T', "SELECT id, reg_id, amount, status FROM tbl_store WHERE id = 42197", plan=('idx:tbl_store_pkey',), no_seq=True, blocks=10, total=200000,
      native="SELECT id, reg_id, amount, status FROM sf_events WHERE id = 42197"),
    c('tbl_range_prune_1t', 'table', '1T', "SELECT qty, count(*), sum(amount) FROM tbl_store WHERE reg_id = 7 AND day BETWEEN DATE '2024-06-01' AND DATE '2024-09-01' GROUP BY qty ORDER BY qty",
      blocks=30, ratio=0.10, per_row=120),
    c('tbl_dml_delta_track_1t', 'table', '1T', DML.format(t='tbl_col_track'), plan=('none',), kind='dml', native=DML.format(t='tbl_untracked'), ratio=1.10, per_row=5800, parity=False,
      extra=(('wal_ratio', 1.25),)),
    c('bm25_topk_common_1t', 'bm25', '1T', "SELECT id FROM sf_events WHERE body @@> 'wire AND switch' ORDER BY sift.score(ctid) DESC, id LIMIT 10", plan=('bm25',), smoke=True,
      blocks=4300, total=26300000, baseline=False),
    c('bm25_hybrid_facet_4t', 'bm25', '4T', "SELECT status, count(*), sum(amount) FROM sf_events WHERE body @@> 'quantum OR clean' AND reg_id <= 16 GROUP BY status ORDER BY status",
      plan=('bm25', 'agg'), blocks=110, heap_zero=True, ratio=0.50, per_row=390,
      native="SELECT status, count(*), sum(amount) FROM sf_events WHERE (body LIKE '%quantum%' OR body LIKE '%clean%') AND reg_id <= 16 GROUP BY status ORDER BY status"),
    c('gpu_arbitration_auto', 'gpu', '1T', GPU_DENSE, plan=('agg',), smoke=True, kind='gpu', ratio=1.05, extra=(('offload_ratio', 0.70),)),
)

SMOKE_IDS = tuple(x.id for x in CANARIES if x.smoke)


def select(areas=None, smoke=False):
    """The canaries of the areas named (all when None), or the smoke subset: one fast canary per subsystem."""
    unknown = set(areas or ()) - set(AREAS)
    if unknown:
        raise ValueError(f'unknown area {sorted(unknown)}: choose from {", ".join(AREAS)}')
    picked = [x for x in CANARIES if (not areas or x.area in areas) and (x.smoke or not smoke)]
    return picked


def limit(canary, name, default=None):
    return dict(canary.extra).get(name, default)
