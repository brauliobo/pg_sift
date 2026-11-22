import time
from .config import (
    INDEX_COLS,
    INDEX_KEY_ORDER,
    INDEX_PATHS,
    INDEX_SCOPE,
    MAINT_MEM,
    SCALES,
    VACUUM_RSS_GUARD_MB,
    klabel,
    ols_slope,
    vacuum_timeout,
)
from .measure import probe_index_size_breakdown
from .queries import QUERIES, SCALING_QUERY_KEYS
from .schema import load_events_table


def run_ladder(st):
    pg, rec = st.pg, st.rec
    st.scale_data = {}

    for nrows in SCALES:
        print(f"[v6-core] === Scale N={nrows} ===", flush=True)
        load_events_table(pg, nrows)
        sd = {"nrows": nrows}

        # --- Workload A1: Entity-only build ---
        ent_sql = f"""
            SET maintenance_work_mem = '{MAINT_MEM}';
            SET max_parallel_maintenance_workers = 0;
            CREATE INDEX idx_events_entity ON events USING sift_col ({INDEX_COLS})
            WITH (key_order = '{INDEX_KEY_ORDER}', paths = '{INDEX_PATHS}');
        """
        ent = pg.run_measured(ent_sql)
        if ent["status"] != "OK":
            raise RuntimeError(f"Entity build failed at N={nrows}: {ent['stderr']}")
        ent_bytes, ent_blks = pg.get_rel_size("idx_events_entity")
        ent["size_bytes"] = ent_bytes
        ent["size_mb"] = ent_bytes / (1024 * 1024)
        ent["size_blks"] = ent_blks
        sd["entity"] = ent
        pg.exec_sql("DROP INDEX idx_events_entity;")

        rec("A_build_entity", "time_ms", nrows, f"{ent['elapsed_ms']:.1f}", "ms", f"{ent['elapsed_ms']*1000/nrows:.2f} us/row")
        rec("A_build_entity", "size_mb", nrows, f"{ent['size_mb']:.2f}", "MB", f"{ent_blks} blks, {ent_bytes/nrows:.1f} B/row")
        rec("A_build_entity", "wal_mb", nrows, f"{ent['wal_bytes']/(1024*1024):.2f}", "MB", f"{ent['wal_bytes']/nrows:.1f} B/row")
        rec("A_build_entity", "temp_mb", nrows, f"{ent['temp_bytes']/(1024*1024):.2f}", "MB", f"{ent['temp_bytes']/nrows:.1f} B/row")
        rec("A_build_entity", "peak_rss_anon_mb", nrows, f"{ent['peak_anon_mb']:.2f}", "MB", f"VmHWM={ent['peak_hwm_mb']:.2f} MB")

        # --- Workload A1b: Plain Scoped CREATE INDEX (non-concurrent) ---
        sp_sql = f"""
            SET maintenance_work_mem = '{MAINT_MEM}';
            SET max_parallel_maintenance_workers = 0;
            CREATE INDEX idx_events_scoped_plain ON events USING sift_col ({INDEX_COLS})
            WITH (
                key_order = '{INDEX_KEY_ORDER}',
                paths = '{INDEX_PATHS}',
                scope = '{INDEX_SCOPE}'
            );
        """
        sp = pg.run_measured(sp_sql)
        if sp["status"] != "OK":
            raise RuntimeError(f"Plain scoped build failed at N={nrows}: {sp['stderr']}")
        sp_bytes, sp_blks = pg.get_rel_size("idx_events_scoped_plain")
        sp["size_bytes"] = sp_bytes
        sp["size_mb"] = sp_bytes / (1024 * 1024)
        sp["size_blks"] = sp_blks
        sp["ratio_vs_entity"] = (sp["elapsed_ms"] / ent["elapsed_ms"]) if ent["elapsed_ms"] > 0 else 0.0
        sp["overhead_pct"] = ((sp_bytes - ent_bytes) / ent_bytes * 100.0) if ent_bytes else 0.0
        sd["scoped_plain"] = sp
        pg.exec_sql("DROP INDEX idx_events_scoped_plain;")

        rec("A_build_scoped_plain", "time_ms", nrows, f"{sp['elapsed_ms']:.1f}", "ms",
            f"{sp['ratio_vs_entity']:.2f}x vs entity, {sp['elapsed_ms']*1000/nrows:.2f} us/row")
        rec("A_build_scoped_plain", "ratio_vs_entity", nrows, f"{sp['ratio_vs_entity']:.2f}", "x", "plain scoped / entity")
        rec("A_build_scoped_plain", "size_mb", nrows, f"{sp['size_mb']:.2f}", "MB", f"{sp_blks} blks, +{sp['overhead_pct']:.1f}% vs entity")
        rec("A_build_scoped_plain", "wal_mb", nrows, f"{sp['wal_bytes']/(1024*1024):.2f}", "MB", f"{sp['wal_bytes']/nrows:.1f} B/row")
        rec("A_build_scoped_plain", "temp_mb", nrows, f"{sp['temp_bytes']/(1024*1024):.2f}", "MB", f"{sp['temp_bytes']/nrows:.1f} B/row")
        rec("A_build_scoped_plain", "peak_rss_anon_mb", nrows, f"{sp['peak_anon_mb']:.2f}", "MB", f"VmHWM={sp['peak_hwm_mb']:.2f} MB")

        # --- Workload A3: Parallel-Pass-1 Scoped CIC (max_parallel_maintenance_workers = 2) at N_MAX ---
        if nrows == SCALES[-1]:
            cic_par_sql = f"""
                SET maintenance_work_mem = '{MAINT_MEM}';
                SET max_parallel_maintenance_workers = 2;
                SET min_parallel_table_scan_size = 0;
                CREATE INDEX CONCURRENTLY idx_events_scoped_par ON events USING sift_col ({INDEX_COLS})
                WITH (
                    key_order = '{INDEX_KEY_ORDER}',
                    paths = '{INDEX_PATHS}',
                    scope = '{INDEX_SCOPE}'
                );
            """
            cic_par = pg.run_measured(cic_par_sql, watch_cic=True)
            if cic_par["status"] != "OK":
                raise RuntimeError(f"Parallel-Pass-1 Scoped CIC failed at N={nrows}: {cic_par['stderr']}")
            cp_bytes, cp_blks = pg.get_rel_size("idx_events_scoped_par")
            cic_par["size_bytes"] = cp_bytes
            cic_par["size_mb"] = cp_bytes / (1024 * 1024)
            cic_par["size_blks"] = cp_blks
            cp1 = cic_par["cic_pass1_ms"]
            cp2 = cic_par["cic_pass2_ms"]
            cic_par["pass1_ms"] = cp1
            cic_par["pass2_ms"] = cp2
            cic_par["pass2_ratio"] = (cp2 / cp1) if cp1 > 0 else 0.0
            sd["cic_par2"] = cic_par
            pg.exec_sql("DROP INDEX idx_events_scoped_par;")

            rec("A_build_cic_par2", "total_ms", nrows, f"{cic_par['elapsed_ms']:.1f}", "ms",
                f"maint_workers=2, {cic_par['elapsed_ms']*1000/nrows:.2f} us/row")
            rec("A_build_cic_par2", "pass1_ms", nrows, f"{cp1:.1f}", "ms", "parallel pass 1 (2 workers)")
            rec("A_build_cic_par2", "pass2_ms", nrows, f"{cp2:.1f}", "ms", f"pass2/pass1={cic_par['pass2_ratio']:.2f}x")
            rec("A_build_cic_par2", "pass2_ratio", nrows, f"{cic_par['pass2_ratio']:.2f}", "x", "serial pass2 / par2 pass1")
            rec("A_build_cic_par2", "wal_mb", nrows, f"{cic_par['wal_bytes']/(1024*1024):.2f}", "MB", f"{cic_par['wal_bytes']/nrows:.1f} B/row")
            rec("A_build_cic_par2", "temp_mb", nrows, f"{cic_par['temp_bytes']/(1024*1024):.2f}", "MB", f"{cic_par['temp_bytes']/nrows:.1f} B/row")
            rec("A_build_cic_par2", "peak_rss_anon_mb", nrows, f"{cic_par['peak_anon_mb']:.2f}", "MB", f"VmHWM={cic_par['peak_hwm_mb']:.2f} MB")

        # --- Workload A2: Scoped CIC build (serial Pass 1) ---
        cic_sql = f"""
            SET maintenance_work_mem = '{MAINT_MEM}';
            SET max_parallel_maintenance_workers = 0;
            CREATE INDEX CONCURRENTLY idx_events_scoped ON events USING sift_col ({INDEX_COLS})
            WITH (
                key_order = '{INDEX_KEY_ORDER}',
                paths = '{INDEX_PATHS}',
                scope = '{INDEX_SCOPE}'
            );
        """
        cic = pg.run_measured(cic_sql, watch_cic=True)
        if cic["status"] != "OK":
            raise RuntimeError(f"Scoped CIC failed at N={nrows}: {cic['stderr']}")
        cic_bytes, cic_blks = pg.get_rel_size("idx_events_scoped")
        cic["size_bytes"] = cic_bytes
        cic["size_mb"] = cic_bytes / (1024 * 1024)
        cic["size_blks"] = cic_blks
        cic["overhead_pct"] = ((cic_bytes - ent_bytes) / ent_bytes * 100.0) if ent_bytes else 0.0
        # Derive pass2 ratio: prefer observed phase split, fall back to (cic - ent)/ent if phase transition was missed
        p1 = cic["cic_pass1_ms"]
        p2 = cic["cic_pass2_ms"]
        if p2 <= 0.0 and cic["elapsed_ms"] > ent["elapsed_ms"]:
            p1 = ent["elapsed_ms"]
            p2 = cic["elapsed_ms"] - ent["elapsed_ms"]
        cic["pass1_ms"] = p1
        cic["pass2_ms"] = p2
        cic["pass2_ratio"] = (p2 / p1) if p1 > 0 else 0.0
        sd["cic"] = cic

        rec("A_build_cic", "total_ms", nrows, f"{cic['elapsed_ms']:.1f}", "ms", f"{cic['elapsed_ms']*1000/nrows:.2f} us/row")
        rec("A_build_cic", "pass1_ms", nrows, f"{p1:.1f}", "ms", "entity pass")
        rec("A_build_cic", "pass2_ms", nrows, f"{p2:.1f}", "ms", f"pass2/pass1={cic['pass2_ratio']:.2f}x")
        rec("A_build_cic", "pass2_ratio", nrows, f"{cic['pass2_ratio']:.3f}", "x", "pass2_ms / pass1_ms")
        rec("A_build_cic", "size_mb", nrows, f"{cic['size_mb']:.2f}", "MB", f"{cic_blks} blks, +{cic['overhead_pct']:.1f}% vs entity, {cic_bytes/nrows:.1f} B/row")
        rec("A_build_cic", "wal_mb", nrows, f"{cic['wal_bytes']/(1024*1024):.2f}", "MB", f"{cic['wal_bytes']/nrows:.1f} B/row")
        rec("A_build_cic", "temp_mb", nrows, f"{cic['temp_bytes']/(1024*1024):.2f}", "MB", f"{cic['temp_bytes']/nrows:.1f} B/row")
        rec("A_build_cic", "peak_rss_anon_mb", nrows, f"{cic['peak_anon_mb']:.2f}", "MB", f"VmHWM={cic['peak_hwm_mb']:.2f} MB")

        # Capture clean post-CIC heap/index size and breakdown at N_MAX before mutations
        if nrows == SCALES[-1]:
            sd["heap_table_bytes"] = pg.get_table_size("events")
            sd["heap_rel_bytes"], _ = pg.get_rel_size("events")
            sd["heap_total_bytes"] = pg.get_total_rel_size("events")
            sd["clean_breakdown"] = probe_index_size_breakdown(pg, "idx_events_scoped", cic_bytes, nrows)

        # --- Workload B1: First VACUUM after CIC ---
        io_before = pg.get_idx_blks_io("idx_events_scoped")
        vac1 = pg.run_measured(
            f"SET maintenance_work_mem = '{MAINT_MEM}'; VACUUM events;",
            rss_guard_mb=VACUUM_RSS_GUARD_MB,
            timeout_sec=vacuum_timeout(),
        )
        time.sleep(0.05)
        io_after = pg.get_idx_blks_io("idx_events_scoped")
        vac1_bytes, vac1_blks = pg.get_rel_size("idx_events_scoped")
        vac1["blks_touched"] = max(0, io_after - io_before)
        vac1["size_after_mb"] = vac1_bytes / (1024 * 1024)
        vac1["size_after_blks"] = vac1_blks
        sd["vac1"] = vac1

        rec("B_vac_after_cic", "time_ms", nrows, f"{vac1['elapsed_ms']:.1f}", "ms", f"status={vac1['status']}")
        rec("B_vac_after_cic", "peak_rss_anon_mb", nrows, f"{vac1['peak_anon_mb']:.2f}", "MB", f"VmHWM={vac1['peak_hwm_mb']:.2f} MB")
        rec("B_vac_after_cic", "wal_mb", nrows, f"{vac1['wal_bytes']/(1024*1024):.2f}", "MB", f"{vac1['wal_bytes']/nrows:.1f} B/row")
        rec("B_vac_after_cic", "blks_touched", nrows, str(vac1["blks_touched"]), "blks", f"size_after={vac1['size_after_mb']:.2f} MB")

        # --- Query Scaling Probes at each scale (1x -> 4x -> 16x) ---
        sd["queries"] = {}
        for qk in SCALING_QUERY_KEYS:
            qres = pg.explain_query(QUERIES[qk], workers=0, runs=3, warmup=1)
            sd["queries"][qk] = qres
            rec("D_scaling", f"{qk}_ms", nrows, f"{qres['exec_ms']:.2f}", "ms", f"blks={qres['total_blks']} plan={qres['plan_tag']}")
            rec("D_scaling", f"{qk}_blks", nrows, str(qres["total_blks"]), "blks", f"hit={qres['shared_hit']} read={qres['shared_read']}")

        st.scale_data[nrows] = sd


def compute_slopes(st):
    rec = st.rec
    # --- Compute Explicit Scaling Slopes & Growth Ratios (10k -> 40k -> 160k) ---
    xs = [float(n) for n in SCALES]
    SLOPE_KEY = f"{klabel(SCALES[0])}-{klabel(SCALES[-1])}"
    st.slopes = {
        "cic_rss_anon_b_per_row": ols_slope(xs, [st.scale_data[n]["cic"]["peak_anon_mb"] * 1024 * 1024 for n in SCALES]),
        "cic_vm_hwm_b_per_row": ols_slope(xs, [st.scale_data[n]["cic"]["peak_hwm_mb"] * 1024 * 1024 for n in SCALES]),
        "cic_temp_b_per_row": ols_slope(xs, [st.scale_data[n]["cic"]["temp_bytes"] for n in SCALES]),
        "cic_disk_b_per_row": ols_slope(xs, [st.scale_data[n]["cic"]["size_bytes"] for n in SCALES]),
        "cic_wal_b_per_row": ols_slope(xs, [st.scale_data[n]["cic"]["wal_bytes"] for n in SCALES]),
        "vac1_rss_anon_b_per_row": ols_slope(xs, [st.scale_data[n]["vac1"]["peak_anon_mb"] * 1024 * 1024 for n in SCALES]),
        "vac1_vm_hwm_b_per_row": ols_slope(xs, [st.scale_data[n]["vac1"]["peak_hwm_mb"] * 1024 * 1024 for n in SCALES]),
    }
    for k, v in st.slopes.items():
        rec("S_slopes", k, SLOPE_KEY, f"{v:.2f}", "B/row", "OLS across " + ", ".join(klabel(n) for n in SCALES))
