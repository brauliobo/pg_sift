import statistics
import time
from .config import (
    MAINT_MEM,
    MEDIAN_ENTITY_ID,
    MEGA_ENTITY_ID,
    MEGA_PARENT_ID,
    PAR_WORK_MEM,
    SCALES,
    UUID_BYTES,
    VACUUM_RSS_GUARD_MB,
    vacuum_timeout,
)
from .measure import BREAKDOWN_SECTIONS, run_workload_e_idle_pooled, run_workload_g_compaction_wa, verify_diff
from .queries import (
    CTE6_PROBE_SQL,
    F4_SOFT_DEL_NOT_NULL_SQL,
    F4_SOFT_DEL_NULL_SQL,
    F5_MEGA_ENTITY_SQL,
    F5_SINGLE_ENTITY_SQL,
    F6_MATERIALIZED_CTE_SQL,
    F7_JSONB_EXISTS_SQL,
    F8_UPDATED_AT_TOP20_SQL,
    QUERIES,
    VARIANT_EXC_SQL,
)


def workload_d(st):
    pg, rec = st.pg, st.rec
    # --- Workload D (Full Q1-Q8 + 1v4 Parallel Scaling on N=160k) ---
    N_MAX = SCALES[-1]
    print(f"[v6-core] === Workload D (Analytical Q1-Q8 & 1v4 Parallel at N={N_MAX}) ===", flush=True)
    st.full_q = {}
    for qk, qsql in QUERIES.items():
        if qk in st.scale_data[N_MAX]["queries"]:
            st.full_q[qk] = st.scale_data[N_MAX]["queries"][qk]
        else:
            st.full_q[qk] = pg.explain_query(qsql, workers=0, runs=3, warmup=1)
        q = st.full_q[qk]
        rec("D_analytical", f"{qk}_1w_ms", N_MAX, f"{q['exec_ms']:.2f}", "ms",
            f"hit={q['shared_hit']} read={q['shared_read']} plan_blks={q['plan_blks']} rss={q['peak_anon_mb']:.1f}MB plan={q['plan_tag']}")

    # 1-vs-4 worker parallel runs for Q4 and Q5 (use PAR_WORK_MEM=16MB so Q4 does not refuse parallel due to hash-spill threshold)
    st.par_q = {}
    for qk in ("Q4_highcard_groupby", "Q5_count_distinct"):
        q1w_par = pg.explain_query(QUERIES[qk], workers=0, runs=3, warmup=1, work_mem=PAR_WORK_MEM)
        q4w = pg.explain_query(QUERIES[qk], workers=4, runs=3, warmup=1, work_mem=PAR_WORK_MEM)
        speedup = q1w_par["exec_ms"] / q4w["exec_ms"] if q4w["exec_ms"] > 0 else 0.0
        q4w["exec_1w_par_ms"] = q1w_par["exec_ms"]
        q4w["speedup_vs_1w"] = speedup
        st.par_q[qk] = q4w
        rec("D_parallel", f"{qk}_1w_par_ms", N_MAX, f"{q1w_par['exec_ms']:.2f}", "ms",
            f"work_mem={PAR_WORK_MEM} plan={q1w_par['plan_tag']}")
        rec("D_parallel", f"{qk}_4w_ms", N_MAX, f"{q4w['exec_ms']:.2f}", "ms",
            f"work_mem={PAR_WORK_MEM} workers={q4w['workers_launched']} speedup={speedup:.2f}x plan={q4w['plan_tag']}")
        rec("D_parallel", f"{qk}_workers_launched", N_MAX, str(q4w["workers_launched"]), "workers",
            f"planned={q4w['workers_planned']} requested=4")
        rec("D_parallel", f"{qk}_speedup", N_MAX, f"{speedup:.2f}", "x", f"1w / 4w (work_mem={PAR_WORK_MEM})")


def workload_c(st):
    pg, rec = st.pg, st.rec
    N_MAX = SCALES[-1]
    # --- Workload C: OLTP Mutations & 2% Dirty-State Resilience (on N=160k) ---
    print(f"[v6-core] === Workload C (OLTP & Dirty-State at N={N_MAX}) ===", flush=True)

    # C1: 1,000 single-row INSERTs (individual autocommit statements)
    ins_stmts = []
    for k in range(1, 1001):
        row_id = N_MAX + k
        ent_id = MEGA_ENTITY_ID if (k % 20) == 0 else (2 + ((row_id * 7919) % 1999))
        analysis_lit = (
            '\'{"flag":"ok","items":[{"kind":"k1","weight":1.5}]}\'::jsonb'
            if (k % 4) == 0
            else "'{}'::jsonb"
        )
        ins_stmts.append(
            f"INSERT INTO events VALUES ({row_id}, {ent_id}, "
            f"DATE '2025-12-01' + ({k % 30}), NULL, "
            f"TIMESTAMPTZ '2025-12-20 00:00:00+00' + ({k} * INTERVAL '1 second'), "
            f"{1 + (row_id % 50000)}, 'tag-ins-{k}', {k % 100}, "
            f"{(k % 1000) / 10.0:.2f}, {(k % 100) / 10.0:.1f}, {1 + k % 10}, md5('{row_id}')::uuid, "
            f"'{{\"channel\":\"web\",\"latency\":12.5,\"region\":\"na\"}}'::jsonb, "
            f"'{{\"tag\":\"n-1\",\"priority\":1,\"summary\":\"insert-test\"}}'::jsonb, "
            f"{analysis_lit});"
        )
    st.c_ins = pg.run_measured("\n".join(ins_stmts))
    if st.c_ins["status"] != "OK":
        raise RuntimeError(f"OLTP 1,000 INSERTs failed: {st.c_ins['stderr']}")
    rec("C_oltp", "insert_1000_total_ms", N_MAX, f"{st.c_ins['elapsed_ms']:.1f}", "ms", f"{st.c_ins['elapsed_ms']/1000.0:.3f} ms/row")
    rec("C_oltp", "insert_1000_wal_mb", N_MAX, f"{st.c_ins['wal_bytes']/(1024*1024):.2f}", "MB", f"rss={st.c_ins['peak_anon_mb']:.1f}MB")

    # C2: Column-localized 2% UPDATE (3,200 rows; 0% entity_id changes):
    #   73% (2,336 rows): JSONB (notes/analysis) + updated_at
    #   13% (  416 rows): deleted_at soft-delete + updated_at
    #   14% (  448 rows): scalar (amount, score) + updated_at
    upd_sql = """
        UPDATE events
        SET notes = jsonb_set(notes, '{summary}', to_jsonb('upd-' || id::text)),
            updated_at = TIMESTAMPTZ '2026-06-01 00:00:00+00' + (id * INTERVAL '1 second')
        WHERE id BETWEEN 1 AND 1200;

        UPDATE events
        SET analysis = CASE
                WHEN analysis ? 'flag' THEN jsonb_set(analysis, '{flag}', '"upd"'::jsonb)
                ELSE '{"flag":"upd","items":[{"kind":"k0","weight":1.0}]}'::jsonb
            END,
            updated_at = TIMESTAMPTZ '2026-06-01 00:00:00+00' + (id * INTERVAL '1 second')
        WHERE id BETWEEN 1201 AND 2336;

        UPDATE events
        SET deleted_at = TIMESTAMPTZ '2026-06-01 00:00:00+00' + (id * INTERVAL '1 second'),
            updated_at = TIMESTAMPTZ '2026-06-01 00:00:00+00' + (id * INTERVAL '1 second')
        WHERE id BETWEEN 2337 AND 2752;

        UPDATE events
        SET amount = amount + 1.00,
            score = score + 0.5,
            updated_at = TIMESTAMPTZ '2026-06-01 00:00:00+00' + (id * INTERVAL '1 second')
        WHERE id BETWEEN 2753 AND 3200;
    """
    st.c_upd = pg.run_measured(upd_sql)
    if st.c_upd["status"] != "OK":
        raise RuntimeError(f"OLTP 2% UPDATE failed: {st.c_upd['stderr']}")
    rec("C_oltp", "update_2pct_ms", N_MAX, f"{st.c_upd['elapsed_ms']:.1f}", "ms",
        f"3200 rows (73% JSONB, 13% soft-del, 14% scalar, 0% entity_id), rss={st.c_upd['peak_anon_mb']:.1f}MB")
    rec("C_oltp", "update_2pct_wal_mb", N_MAX, f"{st.c_upd['wal_bytes']/(1024*1024):.2f}", "MB",
        "73% JSONB+updated_at, 13% deleted_at, 14% scalar")

    # Clean baseline before 2% deletes (re-vacuum after insert+update so dirty-state isolates 2% unvacuumed deletes)
    pg.exec_sql(f"SET maintenance_work_mem = '{MAINT_MEM}'; VACUUM events;")
    st.dirty_probe_keys = ["Q1_scope_whole", "Q2_scope_month", "Q3_scope_groupby_parent", "Q7_scope_top20_unfilt"]
    st.clean_q = {}
    for qk in st.dirty_probe_keys:
        st.clean_q[qk] = pg.explain_query(QUERIES[qk], workers=0, runs=3, warmup=1)

    # C3: Bulk DELETE 2% scattered rows (every 50th row -> 3,200 rows across all entities/dates)
    st.c_del = pg.run_measured("DELETE FROM events WHERE id % 50 = 0;")
    rec("C_oltp", "delete_2pct_ms", N_MAX, f"{st.c_del['elapsed_ms']:.1f}", "ms", f"scattered 2%, rss={st.c_del['peak_anon_mb']:.1f}MB")
    rec("C_oltp", "delete_2pct_wal_mb", N_MAX, f"{st.c_del['wal_bytes']/(1024*1024):.2f}", "MB", "")

    # Dirty-state query latency AFTER 2% unvacuumed deletes
    st.dirty_q = {}
    st.dirty_ratios = {}
    for qk in st.dirty_probe_keys:
        st.dirty_q[qk] = pg.explain_query(QUERIES[qk], workers=0, runs=3, warmup=1)
        ratio = st.dirty_q[qk]["exec_ms"] / st.clean_q[qk]["exec_ms"] if st.clean_q[qk]["exec_ms"] > 0 else 0.0
        st.dirty_ratios[qk] = ratio
        rec("C_dirty", f"{qk}_dirty_ratio", N_MAX, f"{ratio:.2f}", "x",
            f"clean={st.clean_q[qk]['exec_ms']:.2f}ms dirty={st.dirty_q[qk]['exec_ms']:.2f}ms")


def workload_b2(st):
    pg, rec = st.pg, st.rec
    N_MAX = SCALES[-1]
    # --- Workload B2: Post-Delete VACUUM (on N=160k after 2% scattered deletes) ---
    print(f"[v6-core] === Workload B2 (Post-Delete VACUUM at N={N_MAX}) ===", flush=True)
    io_b2_0 = pg.get_idx_blks_io("idx_events_scoped")
    st.vac2 = pg.run_measured(
        f"SET maintenance_work_mem = '{MAINT_MEM}'; VACUUM events;",
        rss_guard_mb=VACUUM_RSS_GUARD_MB,
        timeout_sec=vacuum_timeout(),
    )
    time.sleep(0.05)
    io_b2_1 = pg.get_idx_blks_io("idx_events_scoped")
    vac2_bytes, vac2_blks = pg.get_rel_size("idx_events_scoped")
    st.vac2["blks_touched"] = max(0, io_b2_1 - io_b2_0)
    st.vac2["size_after_mb"] = vac2_bytes / (1024 * 1024)
    st.vac2["size_after_blks"] = vac2_blks

    rec("B_vac_post_del", "time_ms", N_MAX, f"{st.vac2['elapsed_ms']:.1f}", "ms", f"status={st.vac2['status']}")
    rec("B_vac_post_del", "peak_rss_anon_mb", N_MAX, f"{st.vac2['peak_anon_mb']:.2f}", "MB", f"VmHWM={st.vac2['peak_hwm_mb']:.2f} MB")
    rec("B_vac_post_del", "wal_mb", N_MAX, f"{st.vac2['wal_bytes']/(1024*1024):.2f}", "MB", "")
    rec("B_vac_post_del", "blks_touched", N_MAX, str(st.vac2["blks_touched"]), "blks", f"size_after={st.vac2['size_after_mb']:.2f} MB")


def workload_f(st):
    args, pg, rec = st.args, st.pg, st.rec
    N_MAX = SCALES[-1]
    # --- Workload F: Compact v6 Feature Probes (< 3s total, on N=160k) ---
    print(f"[v6-core] === Workload F (Compact Probes at N={N_MAX}) ===", flush=True)

    # F1: 6-CTE planning latency & shared-buffer probe (EXPLAIN SUMMARY, BUFFERS; no execution)
    st.cte6 = pg.explain_query(CTE6_PROBE_SQL, workers=0, runs=5, warmup=1, analyze=False)
    rec("F_probes", "cte6_plan_ms", N_MAX, f"{st.cte6['plan_ms']:.2f}", "ms",
        f"plan_blks={st.cte6['plan_blks']} plan={st.cte6['plan_tag']}")
    rec("F_probes", "cte6_plan_blks", N_MAX, str(st.cte6["plan_blks"]), "blks",
        f"plan_ms={st.cte6['plan_ms']:.2f}ms plan={st.cte6['plan_tag']}")

    # F2: Pair-edit refresh probe (remove 5 + add 5 pairs in entity_parents for parent_id=42)
    st.pair_before = pg.explain_query(QUERIES["Q1_scope_whole"], workers=0, runs=3, warmup=1)
    st.pair_dml = pg.run_measured("""
        BEGIN;
        WITH del_ids AS (
            SELECT entity_id FROM entity_parents
            WHERE parent_id = 42
            ORDER BY entity_id LIMIT 5
        )
        DELETE FROM entity_parents
        WHERE parent_id = 42 AND entity_id IN (SELECT entity_id FROM del_ids);

        INSERT INTO entity_parents (entity_id, parent_id)
        SELECT e, 42
        FROM generate_series(100, 2000) e
        WHERE NOT EXISTS (
            SELECT 1 FROM entity_parents ep WHERE ep.entity_id = e AND ep.parent_id = 42
        )
        ORDER BY e LIMIT 5;
        COMMIT;
    """)
    if st.pair_dml["status"] != "OK":
        raise RuntimeError(f"Pair-edit DML failed: {st.pair_dml['stderr']}")
    st.pair_first = pg.explain_query(QUERIES["Q1_scope_whole"], workers=0, runs=1, warmup=0)
    st.pair_med = pg.explain_query(QUERIES["Q1_scope_whole"], workers=0, runs=3, warmup=0)
    rec("F_probes", "pair_edit_dml_ms", N_MAX, f"{st.pair_dml['elapsed_ms']:.2f}", "ms",
        f"-5/+5 pairs, wal={st.pair_dml['wal_bytes']/1024.0:.1f}KB")
    rec("F_probes", "pair_edit_q1_first_ms", N_MAX, f"{st.pair_first['exec_ms']:.2f}", "ms",
        f"clean={st.pair_before['exec_ms']:.2f}ms blks={st.pair_first['total_blks']} plan={st.pair_first['plan_tag']}")
    rec("F_probes", "pair_edit_q1_med_ms", N_MAX, f"{st.pair_med['exec_ms']:.2f}", "ms",
        f"blks={st.pair_med['total_blks']} plan={st.pair_med['plan_tag']}")

    # F3: JSONB variant-exception probe (0.5% string values in shredded float8 path metrics.latency)
    var_upd = pg.run_measured(
        "UPDATE events SET metrics = jsonb_set(metrics, '{latency}', '\"err\"'::jsonb) WHERE id % 200 = 0;"
    )
    if var_upd["status"] != "OK":
        raise RuntimeError(f"JSONB variant update failed: {var_upd['stderr']}")
    pg.exec_sql(f"SET maintenance_work_mem = '{MAINT_MEM}'; VACUUM events;")
    st.var_q = pg.explain_query(VARIANT_EXC_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "jsonb_variant_exc_ms", N_MAX, f"{st.var_q['exec_ms']:.2f}", "ms",
        f"0.5% str in float8, blks={st.var_q['total_blks']} heap_blks={st.var_q['heap_scan_blks']} plan={st.var_q['plan_tag']}")
    rec("F_probes", "jsonb_variant_exc_blks", N_MAX, str(st.var_q["total_blks"]), "blks",
        f"hit={st.var_q['shared_hit']} read={st.var_q['shared_read']}")
    rec("F_probes", "jsonb_variant_heap_blks", N_MAX, str(st.var_q["heap_scan_blks"]), "blks",
        f"plan={st.var_q['plan_tag']}")

    # F4: Soft-delete probes (WHERE deleted_at IS NULL and WHERE deleted_at IS NOT NULL)
    st.f4_null = pg.explain_query(F4_SOFT_DEL_NULL_SQL, workers=0, runs=3, warmup=1)
    st.f4_not_null = pg.explain_query(F4_SOFT_DEL_NOT_NULL_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "f4_soft_del_null_ms", N_MAX, f"{st.f4_null['exec_ms']:.2f}", "ms",
        f"blks={st.f4_null['total_blks']} plan_blks={st.f4_null['plan_blks']} plan={st.f4_null['plan_tag']}")
    rec("F_probes", "f4_soft_del_null_blks", N_MAX, str(st.f4_null["total_blks"]), "blks",
        f"hit={st.f4_null['shared_hit']} read={st.f4_null['shared_read']}")
    rec("F_probes", "f4_soft_del_not_null_ms", N_MAX, f"{st.f4_not_null['exec_ms']:.2f}", "ms",
        f"blks={st.f4_not_null['total_blks']} plan_blks={st.f4_not_null['plan_blks']} plan={st.f4_not_null['plan_tag']}")
    rec("F_probes", "f4_soft_del_not_null_blks", N_MAX, str(st.f4_not_null["total_blks"]), "blks",
        f"hit={st.f4_not_null['shared_hit']} read={st.f4_not_null['shared_read']}")

    # F5: Single-entity (median) & mega-entity (5% rows) scoped aggregate
    st.f5_single = pg.explain_query(F5_SINGLE_ENTITY_SQL, workers=0, runs=3, warmup=1)
    st.f5_mega = pg.explain_query(F5_MEGA_ENTITY_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "f5_single_entity_ms", N_MAX, f"{st.f5_single['exec_ms']:.2f}", "ms",
        f"entity={MEDIAN_ENTITY_ID} blks={st.f5_single['total_blks']} plan_blks={st.f5_single['plan_blks']} plan={st.f5_single['plan_tag']}")
    rec("F_probes", "f5_single_entity_blks", N_MAX, str(st.f5_single["total_blks"]), "blks",
        f"hit={st.f5_single['shared_hit']} read={st.f5_single['shared_read']}")
    rec("F_probes", "f5_mega_entity_ms", N_MAX, f"{st.f5_mega['exec_ms']:.2f}", "ms",
        f"entity={MEGA_ENTITY_ID} (5% rows) blks={st.f5_mega['total_blks']} plan_blks={st.f5_mega['plan_blks']} plan={st.f5_mega['plan_tag']}")
    rec("F_probes", "f5_mega_entity_blks", N_MAX, str(st.f5_mega["total_blks"]), "blks",
        f"hit={st.f5_mega['shared_hit']} read={st.f5_mega['shared_read']}")

    # F6: MATERIALIZED CTE scope join (catches 509x Lookup Join overlapping-segment regression)
    st.f6_cte = pg.explain_query(F6_MATERIALIZED_CTE_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "f6_materialized_cte_ms", N_MAX, f"{st.f6_cte['exec_ms']:.2f}", "ms",
        f"parent={MEGA_PARENT_ID} blks={st.f6_cte['total_blks']} plan_blks={st.f6_cte['plan_blks']} plan={st.f6_cte['plan_tag']}")
    rec("F_probes", "f6_materialized_cte_blks", N_MAX, str(st.f6_cte["total_blks"]), "blks",
        f"hit={st.f6_cte['shared_hit']} read={st.f6_cte['shared_read']}")

    # F7: JSONB key-exists (`?`) probe (catches 30x jsonb_exists heap re-read)
    st.f7_exists = pg.explain_query(F7_JSONB_EXISTS_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "f7_jsonb_exists_ms", N_MAX, f"{st.f7_exists['exec_ms']:.2f}", "ms",
        f"blks={st.f7_exists['total_blks']} heap_blks={st.f7_exists['heap_scan_blks']} reread_rows={st.f7_exists['heap_reread_rows']} plan_blks={st.f7_exists['plan_blks']} plan={st.f7_exists['plan_tag']}")
    rec("F_probes", "f7_jsonb_exists_blks", N_MAX, str(st.f7_exists["total_blks"]), "blks",
        f"hit={st.f7_exists['shared_hit']} read={st.f7_exists['shared_read']}")
    rec("F_probes", "f7_jsonb_exists_heap_blks", N_MAX, str(st.f7_exists["heap_scan_blks"]), "blks",
        f"reread_rows={st.f7_exists['heap_reread_rows']} plan={st.f7_exists['plan_tag']}")
    rec("F_probes", "f7_jsonb_exists_reread_rows", N_MAX, str(st.f7_exists["heap_reread_rows"]), "rows",
        f"heap_blks={st.f7_exists['heap_scan_blks']} plan={st.f7_exists['plan_tag']}")

    # F8: Scoped secondary-sort Top-20 (ORDER BY updated_at DESC LIMIT 20; catches 60x regression)
    st.f8_upd_top20 = pg.explain_query(F8_UPDATED_AT_TOP20_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "f8_updated_at_top20_ms", N_MAX, f"{st.f8_upd_top20['exec_ms']:.2f}", "ms",
        f"parent={MEGA_PARENT_ID} blks={st.f8_upd_top20['total_blks']} plan_blks={st.f8_upd_top20['plan_blks']} plan={st.f8_upd_top20['plan_tag']}")
    rec("F_probes", "f8_updated_at_top20_blks", N_MAX, str(st.f8_upd_top20["total_blks"]), "blks",
        f"hit={st.f8_upd_top20['shared_hit']} read={st.f8_upd_top20['shared_read']}")

    st.verify_bad = verify_diff(pg, rec, N_MAX) if args.verify_diff else []

    # Planning shared-buffer gate across Q1-Q8 + F4-F8 at N_MAX
    plan_probe_results = list(st.full_q.values()) + [
        st.f4_null, st.f4_not_null, st.f5_single, st.f5_mega, st.f6_cte, st.f7_exists, st.f8_upd_top20
    ]
    st.all_plan_blks = [r["plan_blks"] for r in plan_probe_results]
    all_plan_ms = [r["plan_ms"] for r in plan_probe_results]
    st.plan_buffers_median = statistics.median(st.all_plan_blks) if st.all_plan_blks else 0.0
    st.plan_buffers_max = max(st.all_plan_blks) if st.all_plan_blks else 0
    st.plan_ms_median = statistics.median(all_plan_ms) if all_plan_ms else 0.0
    rec("F_probes", "plan_buffers_median", N_MAX, f"{st.plan_buffers_median:.1f}", "blks",
        f"max={st.plan_buffers_max} plan_ms_med={st.plan_ms_median:.2f}ms across {len(st.all_plan_blks)} queries (Q1-Q8, F4-F8)")
    rec("F_probes", "plan_buffers_max", N_MAX, str(st.plan_buffers_max), "blks",
        f"median={st.plan_buffers_median:.1f} across {len(st.all_plan_blks)} queries")


def workload_e(st):
    pg, rec = st.pg, st.rec
    # --- Workload E: Idle Pooled Backend Memory ---
    print("[v6-core] === Workload E (Idle Pooled Backend Memory) ===", flush=True)
    st.idle = run_workload_e_idle_pooled(pg)
    rec("E_idle_backend", "rss_anon_idle_mb", st.idle["indexes_touched"], f"{st.idle['anon_idle_mb']:.2f}", "MB",
        f"cap={st.idle['cache_cap']} before={st.idle['anon_before_mb']:.2f}MB active={st.idle['anon_active_mb']:.2f}MB "
        f"delta={st.idle['anon_delta_mb']:.2f}MB hwm={st.idle['hwm_mb']:.2f}MB {st.idle['delta_kb_per_index']:.1f}KB/idx")
    rec("E_idle_backend", "rss_anon_delta_mb", st.idle["indexes_touched"], f"{st.idle['anon_delta_mb']:.2f}", "MB",
        f"cap={st.idle['cache_cap']} ({st.idle['delta_kb_per_index']:.1f}KB/idx)")


def workload_g(st):
    pg, rec = st.pg, st.rec
    N_MAX = SCALES[-1]
    # --- Workload G: Index Size, Page Utilization, WAL Efficiency & Compaction WA ---
    print("[v6-core] === Workload G (Size, Page Utilization, WAL & Compaction WA) ===", flush=True)
    sd_max = st.scale_data[N_MAX]
    heap_table_bytes = sd_max["heap_table_bytes"]
    st.heap_rel_bytes = sd_max["heap_rel_bytes"]
    st.heap_total_bytes = sd_max["heap_total_bytes"]
    st.ent_bytes_max = sd_max["entity"]["size_bytes"]
    st.cic_bytes_max = sd_max["cic"]["size_bytes"]
    st.cic_wal_max = sd_max["cic"]["wal_bytes"]
    st.clean_bd = sd_max["clean_breakdown"]

    st.g_size = {
        "heap_size_mb": heap_table_bytes / (1024 * 1024),
        "heap_b_per_row": heap_table_bytes / float(N_MAX),
        "entity_b_per_row": st.ent_bytes_max / float(N_MAX),
        "scoped_b_per_row": st.cic_bytes_max / float(N_MAX),
        "scoped_ex_uuid_b_per_row": (st.cic_bytes_max - UUID_BYTES * N_MAX) / float(N_MAX),
        "scoped_to_heap_ratio": (st.cic_bytes_max / float(heap_table_bytes)) if heap_table_bytes > 0 else 0.0,
        "scope_overhead_pct": sd_max["cic"]["overhead_pct"],
        "breakdown": st.clean_bd,
    }
    rec("G_size", "heap_size_mb", N_MAX, f"{st.g_size['heap_size_mb']:.2f}", "MB",
        f"pg_table_size; rel={st.heap_rel_bytes/(1024*1024):.2f}MB total={st.heap_total_bytes/(1024*1024):.2f}MB")
    rec("G_size", "heap_b_per_row", N_MAX, f"{st.g_size['heap_b_per_row']:.2f}", "B/row", "heap+TOAST via pg_table_size")
    rec("G_size", "entity_b_per_row", N_MAX, f"{st.g_size['entity_b_per_row']:.2f}", "B/row", "entity-only sift_col")
    rec("G_size", "scoped_b_per_row", N_MAX, f"{st.g_size['scoped_b_per_row']:.2f}", "B/row", "scoped CIC sift_col")
    rec("G_size", "scoped_ex_uuid_b_per_row", N_MAX, f"{st.g_size['scoped_ex_uuid_b_per_row']:.2f}", "B/row",
        f"scoped CIC minus {UUID_BYTES} B MD5 UUID per row")
    rec("G_size", "scoped_to_heap_ratio", N_MAX, f"{st.g_size['scoped_to_heap_ratio']:.3f}", "x", "scoped_bytes / pg_table_size(events)")
    rec("G_size", "scope_overhead_pct", N_MAX, f"{st.g_size['scope_overhead_pct']:.2f}", "%", "scoped vs entity size overhead")

    if st.clean_bd["available"]:
        rec("G_size", "page_utilization_pct", N_MAX, f"{st.clean_bd['page_utilization_pct']:.2f}", "%", f"via {st.clean_bd['fn']}")
        rec("G_size", "dead_space_pct", N_MAX, f"{st.clean_bd['dead_space_pct']:.2f}", "%", f"post-CIC via {st.clean_bd['fn']}")
        for sec in BREAKDOWN_SECTIONS:
            sb = st.clean_bd["sections_bytes"][sec]
            sbpr = st.clean_bd["sections_b_per_row"][sec]
            rec("G_size", f"section_bytes_{sec}", N_MAX, f"{sbpr:.2f}", "B/row", f"{sb} bytes")
    else:
        rec("G_size", "page_utilization_pct", N_MAX, "n/a", "%", "sift_index_size_breakdown unavailable")
        rec("G_size", "dead_space_pct", N_MAX, "n/a", "%", "sift_index_size_breakdown unavailable")
        for sec in BREAKDOWN_SECTIONS:
            rec("G_size", f"section_bytes_{sec}", N_MAX, "n/a", "B/row", "sift_index_size_breakdown unavailable")

    st.n_2pct = max(1.0, N_MAX * 0.02)
    st.g_wal = {
        "cic_wal_b_per_row": st.cic_wal_max / float(N_MAX),
        "oltp_ins1_wal_b_per_row": st.c_ins["wal_bytes"] / 1000.0,
        "update_2pct_wal_b_per_row": st.c_upd["wal_bytes"] / st.n_2pct,
        "delete_2pct_wal_b_per_row": st.c_del["wal_bytes"] / st.n_2pct,
    }
    rec("G_wal", "cic_wal_b_per_row", N_MAX, f"{st.g_wal['cic_wal_b_per_row']:.2f}", "B/row", f"{st.cic_wal_max/(1024*1024):.2f} MB total")
    rec("G_wal", "oltp_ins1_wal_b_per_row", N_MAX, f"{st.g_wal['oltp_ins1_wal_b_per_row']:.2f}", "B/row", "1000x 1-row INSERT")
    rec("G_wal", "update_2pct_wal_b_per_row", N_MAX, f"{st.g_wal['update_2pct_wal_b_per_row']:.2f}", "B/row", f"{int(st.n_2pct)} rows")
    rec("G_wal", "delete_2pct_wal_b_per_row", N_MAX, f"{st.g_wal['delete_2pct_wal_b_per_row']:.2f}", "B/row", f"{int(st.n_2pct)} rows")

    st.g_wa = run_workload_g_compaction_wa(pg, n_base=40000, waves=5, churn_pct=0.02)
    rec("G_compaction", "write_amplification", st.g_wa["n_base"], f"{st.g_wa['write_amplification']:.2f}", "x",
        f"{st.g_wa['waves']} waves x 2% ins/del+VAC, wal={st.g_wa['total_wal_bytes']/(1024*1024):.2f}MB logical={st.g_wa['logical_inserted_bytes']/(1024*1024):.2f}MB")
    rec("G_compaction", "index_growth_ratio", st.g_wa["n_base"], f"{st.g_wa['index_growth_ratio']:.3f}", "x",
        f"init={st.g_wa['init_idx_bytes']/(1024*1024):.2f}MB post={st.g_wa['post_idx_bytes']/(1024*1024):.2f}MB")
    if st.g_wa["breakdown"]["available"]:
        rec("G_compaction", "dead_space_pct", st.g_wa["n_base"], f"{st.g_wa['breakdown']['dead_space_pct']:.2f}", "%",
            f"post-churn via {st.g_wa['breakdown']['fn']}")
        rec("G_compaction", "tail_free_space_pct", st.g_wa["n_base"], f"{st.g_wa['breakdown']['tail_free_space_pct']:.2f}", "%",
            f"free_dead_bytes / total_bytes post-churn via {st.g_wa['breakdown']['fn']}")
    else:
        rec("G_compaction", "dead_space_pct", st.g_wa["n_base"], "n/a", "%", "sift_index_size_breakdown unavailable")
        rec("G_compaction", "tail_free_space_pct", st.g_wa["n_base"], "n/a", "%", "sift_index_size_breakdown unavailable")
    rec("G_compaction", "churn_total_wal_mb", st.g_wa["n_base"], f"{st.g_wa['total_wal_bytes']/(1024*1024):.2f}", "MB",
        f"statuses={','.join(st.g_wa['statuses'])}")
    rec("G_compaction", "churn_elapsed_ms", st.g_wa["n_base"], f"{st.g_wa['elapsed_ms']:.1f}", "ms",
        f"{st.g_wa['waves']} waves at N={st.g_wa['n_base']}")
