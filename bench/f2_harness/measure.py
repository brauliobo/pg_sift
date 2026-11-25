import subprocess
import time
from .config import (
    BACKEND_CACHE_CAP,
    INDEX_COLS,
    INDEX_KEY_ORDER,
    INDEX_PATHS,
    INDEX_SCOPE,
    MAINT_MEM,
    POOL_EXTRA_INDEXES,
    VACUUM_RSS_GUARD_MB,
    WORK_MEM,
    read_proc_mem_kb,
    vacuum_timeout,
)
from .queries import F_PROBE_SQL, QUERIES, TOPN_ORDER_KEY
from .schema import events_insert_sql, events_table_ddl


def result_checksum(pg, sql, key=None, heap_scan=False):
    """md5 of the rows of sql in any order; a Top-N is cut to its ORDER BY column (ties pick different rows legitimately); heap_scan runs the stock plan"""
    cols = f"t.{key}" if key else "t"
    stock = "SET sift.enable_agg = off;\n" if heap_scan else ""
    r = pg._run_psql(["-At"], sql=(
        f"SET work_mem = '{WORK_MEM}'; SET max_parallel_workers_per_gather = 0; SET jit = off; SET extra_float_digits = -3;\n{stock}"
        f"SELECT md5(coalesce(string_agg({cols}::text, E'\\n' ORDER BY {cols}::text), '')) FROM ({sql}) t;\n"))
    return r.stdout.strip().splitlines()[-1]


def verify_diff(pg, rec, nrows):
    """every Q1-Q8 and F4-F8 probe answered by the sift plan and by the heap scan: the checksums are equal or the probe is returned as a mismatch"""
    bad = []
    for name, sql in {**QUERIES, **F_PROBE_SQL}.items():
        sift, heap = (result_checksum(pg, sql, TOPN_ORDER_KEY.get(name), heap_scan=h) for h in (False, True))
        rec("V_diff", f"{name}_match", nrows, int(sift == heap), "bool", f"sift={sift} heap={heap}")
        bad += [name] if sift != heap else []
    rec("V_diff", "mismatches", nrows, len(bad), "queries", ",".join(bad) or "none")
    return bad


def run_workload_e_idle_pooled(pg):
    """
    Create POOL_EXTRA_INDEXES additional sift_col indexes (total 2 + POOL_EXTRA_INDEXES
    with idx_events_scoped + entity_parents_journal), set sift.backend_cache_kb = '2MB',
    run mixed queries + writes across all of them in one persistent backend session,
    then measure resident RssAnon after 1s idle.
    """
    n_extra = POOL_EXTRA_INDEXES
    n_total = n_extra + 2
    pg.exec_sql(f"""
        DO $$
        DECLARE k int;
        BEGIN
            FOR k IN 1..{n_extra} LOOP
                EXECUTE format('DROP TABLE IF EXISTS pool_t%s CASCADE', k);
                EXECUTE format(
                    'CREATE TABLE pool_t%s (id int PRIMARY KEY, entity_id int, event_date date, val numeric(10,2), meta jsonb) WITH (autovacuum_enabled = false)',
                    k
                );
                EXECUTE format(
                    'INSERT INTO pool_t%s SELECT i, 1 + (i %% 100), DATE ''2024-01-01'' + (i %% 365), (i %% 1000) / 10.0, jsonb_build_object(''k'', i %% 20) FROM generate_series(1, 1000) i',
                    k
                );
                EXECUTE format(
                    'CREATE INDEX idx_pool_t%s ON pool_t%s USING sift_col (id, entity_id, event_date, val, meta) WITH (paths = ''meta.k int4'')',
                    k, k
                );
            END LOOP;
        END $$;
    """)

    proc = subprocess.Popen(
        pg.base_cmd + ["-At"],
        stdin=subprocess.PIPE,
        stdout=subprocess.PIPE,
        stderr=subprocess.PIPE,
        text=True,
        bufsize=1,
    )
    proc.stdin.write("SELECT pg_backend_pid();\n")
    proc.stdin.flush()
    pid = int(proc.stdout.readline().strip())

    _, anon_before_kb, rss_before_kb = read_proc_mem_kb(pid)

    # Touch all indexes under tight backend_cache_kb cap with mixed queries + writes
    cache_cap_stmts = [f"SET sift.backend_cache_kb = '{BACKEND_CACHE_CAP}';"] if guc_registered(pg, "sift.backend_cache_kb") else []
    stmts = [
        f"SET work_mem = '{WORK_MEM}';",
        *cache_cap_stmts,
        "SELECT count(*), sum(amount) FROM events;",
        QUERIES["Q1_scope_whole"] + ";",
        QUERIES["Q3_scope_groupby_parent"] + ";",
        "SELECT count(*) FROM entity_parents;",
    ]
    for k in range(1, n_extra + 1):
        stmts.append(f"SELECT count(*), sum(val) FROM pool_t{k} WHERE (meta->>'k')::int < 10;")
        stmts.append(f"INSERT INTO pool_t{k} VALUES (99999, 1, DATE '2025-01-01', 12.34, '{{\"k\": 1}}'::jsonb);")
        stmts.append(f"DELETE FROM pool_t{k} WHERE id = 99999;")
    stmts.append("SELECT 'IDLE_READY';")

    proc.stdin.write("\n".join(stmts) + "\n")
    proc.stdin.flush()

    while True:
        line = proc.stdout.readline()
        if not line or "IDLE_READY" in line:
            break

    hwm_active_kb, anon_active_kb, rss_active_kb = read_proc_mem_kb(pid)
    time.sleep(1.0)
    hwm_idle_kb, anon_idle_kb, rss_idle_kb = read_proc_mem_kb(pid)

    proc.stdin.write("\\q\n")
    proc.stdin.flush()
    proc.wait(timeout=10)

    # Clean up pool tables
    pg.exec_sql(f"""
        DO $$
        DECLARE k int;
        BEGIN
            FOR k IN 1..{n_extra} LOOP
                EXECUTE format('DROP TABLE IF EXISTS pool_t%s CASCADE', k);
            END LOOP;
        END $$;
    """)

    return {
        "indexes_touched": n_total,
        "extra_tables": n_extra,
        "cache_cap": BACKEND_CACHE_CAP,
        "anon_before_mb": anon_before_kb / 1024.0,
        "anon_active_mb": anon_active_kb / 1024.0,
        "anon_idle_mb": anon_idle_kb / 1024.0,
        "anon_delta_mb": (anon_idle_kb - anon_before_kb) / 1024.0,
        "rss_idle_mb": rss_idle_kb / 1024.0,
        "hwm_mb": hwm_idle_kb / 1024.0,
        "delta_kb_per_index": (anon_idle_kb - anon_before_kb) / float(n_total),
    }


BREAKDOWN_SECTIONS = ("granules", "dicts", "zones", "preagg", "delvec", "scope", "slack")


def guc_registered(pg, name):
    return pg.query_scalar(f"SELECT count(*) FROM pg_settings WHERE name = '{name}'") == "1"


def probe_index_size_breakdown(pg, idxname, total_bytes, nrows):
    """
    Call sift.index_size_breakdown(regclass) or sift_index_size_breakdown(regclass)
    when available (pg_sift 6.0+). Gracefully returns available=False on v5.1.0.
    """
    fn = pg.query_scalar(
        "SELECT n.nspname || '.' || p.proname "
        "FROM pg_proc p "
        "JOIN pg_namespace n ON n.oid = p.pronamespace "
        "WHERE p.proname = 'sift_index_size_breakdown' "
        "   OR (n.nspname = 'sift' AND p.proname = 'index_size_breakdown') "
        "ORDER BY CASE WHEN n.nspname = 'sift' THEN 0 ELSE 1 END "
        "LIMIT 1"
    )
    if not fn:
        return {"available": False, "fn": None}

    try:
        rows = pg.query_rows(f"SELECT * FROM {fn}('{idxname}'::regclass)")
    except Exception as exc:
        return {"available": False, "fn": fn, "error": str(exc)}

    raw = {}
    for row in rows:
        if len(row) < 2:
            continue
        cat = row[0].strip().lower()
        try:
            b = int(float(row[1]))
        except ValueError:
            b = 0
        raw[cat] = raw.get(cat, 0) + b

    sections = {k: 0 for k in BREAKDOWN_SECTIONS}
    free_dead = 0
    sum_all = 0
    for cat, b in raw.items():
        sum_all += b
        if cat == "granules" or cat.startswith("granule") or cat == "l0_slab":
            sections["granules"] += b
        elif cat in ("dicts", "dict", "sec_dicts"):
            sections["dicts"] += b
        elif cat in ("zones", "zones_tstride", "sec_zones", "tid_index"):
            sections["zones"] += b
        elif cat in ("preagg", "sec_preagg"):
            sections["preagg"] += b
        elif cat in ("delvec", "delvec_penddel", "pend_del"):
            sections["delvec"] += b
        elif cat in ("scope", "scope_meta", "sec_scope_meta"):
            sections["scope"] += b
        elif cat in ("slack", "page_slack", "extent_slack"):
            sections["slack"] += b
        elif cat in ("fsm_free", "dead_space", "dead", "free"):
            free_dead += b

    denom = float(max(total_bytes, sum_all))
    if denom > 0.0:
        slack_bytes = float(sections["slack"])
        live_bytes = max(0.0, denom - slack_bytes - float(free_dead))
        page_util_pct = (live_bytes / denom) * 100.0
        dead_space_pct = ((float(free_dead) + slack_bytes) / denom) * 100.0
        tail_free_space_pct = (float(free_dead) / denom) * 100.0
    else:
        page_util_pct = 0.0
        dead_space_pct = 0.0
        tail_free_space_pct = 0.0

    sections_b_per_row = {
        k: (sections[k] / float(nrows) if nrows > 0 else 0.0)
        for k in BREAKDOWN_SECTIONS
    }

    return {
        "available": True,
        "fn": fn,
        "raw": raw,
        "sections_bytes": sections,
        "sections_b_per_row": sections_b_per_row,
        "free_dead_bytes": free_dead,
        "page_utilization_pct": page_util_pct,
        "dead_space_pct": dead_space_pct,
        "tail_free_space_pct": tail_free_space_pct,
    }


def run_workload_g_compaction_wa(pg, n_base=40000, waves=5, churn_pct=0.02):
    """
    Fast multi-wave OLTP + compaction write-amplification probe on a dedicated
    events_wa table (N=40,000, 5 waves of 2% insert + 2% scattered delete + VACUUM).
    Measures total WAL written / net logical bytes inserted (write_amplification),
    post-churn index growth ratio, and post-churn dead_space_pct (when breakdown exists).
    """
    wave_rows = max(1, int(n_base * churn_pct))
    pg.exec_sql(
        events_table_ddl("events_wa")
        + events_insert_sql("events_wa", 1, n_base)
        + f"""
        VACUUM (ANALYZE, FREEZE) events_wa;

        SET maintenance_work_mem = '{MAINT_MEM}';
        SET max_parallel_maintenance_workers = 0;
        CREATE INDEX idx_events_wa_scoped ON events_wa USING sift_col ({INDEX_COLS})
        WITH (
            key_order = '{INDEX_KEY_ORDER}',
            paths = '{INDEX_PATHS}',
            scope = '{INDEX_SCOPE}'
        );
    """
    )

    init_heap_bytes = pg.get_table_size("events_wa")
    init_idx_bytes, init_idx_blks = pg.get_rel_size("idx_events_wa_scoped")
    logical_row_bytes = init_heap_bytes / float(n_base)

    total_wal_bytes = 0
    total_elapsed_ms = 0.0
    total_inserted_rows = 0
    wave_statuses = []

    for w in range(1, waves + 1):
        id_lo = n_base + (w - 1) * wave_rows + 1
        id_hi = n_base + w * wave_rows
        wave_sql = (
            events_insert_sql("events_wa", id_lo, id_hi, date_offset=w * 30)
            + f"""
            DELETE FROM events_wa WHERE id <= {n_base} AND (id % 50) = {w};

            SET maintenance_work_mem = '{MAINT_MEM}';
            VACUUM events_wa;
        """
        )
        wres = pg.run_measured(
            wave_sql,
            rss_guard_mb=VACUUM_RSS_GUARD_MB,
            timeout_sec=vacuum_timeout(),
        )
        total_wal_bytes += wres["wal_bytes"]
        total_elapsed_ms += wres["elapsed_ms"]
        total_inserted_rows += wave_rows
        wave_statuses.append(wres["status"])
        if wres["status"] != "OK":
            break

    post_idx_bytes, post_idx_blks = pg.get_rel_size("idx_events_wa_scoped")
    post_bd = probe_index_size_breakdown(pg, "idx_events_wa_scoped", post_idx_bytes, n_base)

    pg.exec_sql("DROP TABLE IF EXISTS events_wa CASCADE;")

    logical_inserted_bytes = total_inserted_rows * logical_row_bytes
    wa = (total_wal_bytes / logical_inserted_bytes) if logical_inserted_bytes > 0 else 0.0
    growth_ratio = (post_idx_bytes / float(init_idx_bytes)) if init_idx_bytes > 0 else 0.0

    return {
        "n_base": n_base,
        "waves": waves,
        "wave_rows": wave_rows,
        "total_inserted_rows": total_inserted_rows,
        "init_heap_bytes": init_heap_bytes,
        "init_idx_bytes": init_idx_bytes,
        "init_idx_blks": init_idx_blks,
        "post_idx_bytes": post_idx_bytes,
        "post_idx_blks": post_idx_blks,
        "logical_inserted_bytes": logical_inserted_bytes,
        "total_wal_bytes": total_wal_bytes,
        "write_amplification": wa,
        "index_growth_ratio": growth_ratio,
        "elapsed_ms": total_elapsed_ms,
        "statuses": wave_statuses,
        "breakdown": post_bd,
    }
