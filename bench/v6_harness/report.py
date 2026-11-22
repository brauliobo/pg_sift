import os
import time
from datetime import datetime, timezone
from .config import (
    BACKEND_CACHE_CAP,
    INDEX_KEY_ORDER,
    MAINT_MEM,
    MEDIAN_ENTITY_ID,
    MEGA_ENTITY_ID,
    MEGA_PARENT_ID,
    MID_PARENT_ID,
    PAR_WORK_MEM,
    REPO,
    SCALES,
    WORK_MEM,
    cpu_model,
    klabel,
)
from .measure import BREAKDOWN_SECTIONS
from .queries import QUERIES, SCALING_QUERY_KEYS


GROWTH_ROWS = (
    ("Scoped CIC Peak `RssAnon`", "cic_rss_anon_b_per_row", "cic", "peak_anon_mb", 1.0, ".1f"),
    ("Scoped CIC Peak `VmHWM`", "cic_vm_hwm_b_per_row", "cic", "peak_hwm_mb", 1.0, ".1f"),
    ("Scoped CIC Temp Bytes", "cic_temp_b_per_row", "cic", "temp_bytes", 1.0 / (1024 * 1024), ".2f"),
    ("Scoped CIC Disk Size", "cic_disk_b_per_row", "cic", "size_mb", 1.0, ".2f"),
    ("Scoped CIC WAL Bytes", "cic_wal_b_per_row", "cic", "wal_bytes", 1.0 / (1024 * 1024), ".2f"),
    ("First VACUUM Peak `RssAnon`", "vac1_rss_anon_b_per_row", "vac1", "peak_anon_mb", 1.0, ".1f"),
    ("First VACUUM Peak `VmHWM`", "vac1_vm_hwm_b_per_row", "vac1", "peak_hwm_mb", 1.0, ".1f"),
)


def growth_rows(scale_data, slopes):
    rows = []
    for label, slope_key, stage, field, mult, fmt in GROWTH_ROWS:
        cells = " | ".join(f"{scale_data[n][stage][field] * mult:{fmt}} MB" for n in SCALES)
        rows.append(f"| **{label}** | **{slopes[slope_key]:.1f} B/row** | {cells} |")
    return rows


def query_growth_rows(scale_data):
    rows = []
    for qk in SCALING_QUERY_KEYS:
        qs = [scale_data[n]["queries"][qk] for n in SCALES]
        tg = qs[-1]["exec_ms"] / qs[0]["exec_ms"] if qs[0]["exec_ms"] > 0 else 0.0
        bg = qs[-1]["total_blks"] / qs[0]["total_blks"] if qs[0]["total_blks"] > 0 else 0.0
        cells = " | ".join(f"{q['exec_ms']:.2f} ms / {q['total_blks']:,}" for q in qs)
        rows.append(f"| `{qk}` | {cells} | **{tg:.2f}×** | **{bg:.2f}×** |")
    return rows


def write_tsv(path, rows, meta=None):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, "w", encoding="utf-8", newline="\n") as f:
        for k, v in (meta or {}).items():
            f.write(f"# {k}={v}\n")
        f.write("section\tmetric\tscale\tvalue\tunit\tnotes\n")
        for row in rows:
            f.write("\t".join(row) + "\n")


def write_results(st):
    args, rec = st.args, st.rec
    st.suite_sec = time.monotonic() - st.t_suite_start
    rec("meta", "bench_suite_time", "-", f"{st.suite_sec:.2f}", "s", "end-to-end SQL telemetry")

    # --- Write TSV ---
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    meta = {"version": st.ext_ver, "git": args.git_sha, "pg": st.pg_ver, "cpu": cpu_model(), "date": today}
    st.tsv_path = os.path.join(args.out_dir, f"{args.round}.tsv")
    write_tsv(st.tsv_path, st.tsv_rows, meta)
    if args.docs:
        write_tsv(os.path.join(REPO, "docs", "results", "v6-core.tsv"), st.tsv_rows, meta)


def write_markdown(st):
    args = st.args
    N_MAX = SCALES[-1]
    # --- Write Markdown Report ---
    st.md_path = os.path.join(args.out_dir, f"{args.round}.md")
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    lines = [
        f"# `pg_sift` Benchmark Report — `{args.round}`",
        "",
        f"- **Timestamp:** {ts}",
        f"- **Version / Git:** `pg_sift` `{st.ext_ver}` (`{args.git_branch}` @ `{args.git_sha}`), PostgreSQL `{st.pg_ver}`",
        f"- **Runtime:** build `{args.build_sec:.2f}s`, benchmark suite `{st.suite_sec:.2f}s`",
        f"- **Budgets:** `maintenance_work_mem = '{MAINT_MEM}'`, `work_mem = '{WORK_MEM}'` (parallel 1v4: `{PAR_WORK_MEM}`), `sift.backend_cache_kb = '{BACKEND_CACHE_CAP}'` (Workload E), `shared_buffers = '256MB'`",
        f"- **Schema:** 15-col wide `events` (`deleted_at` 0.1% historical soft-delete, `updated_at`, `NUMERIC(12,2)`, `UUID`, 3 JSONB cols with sparse `analysis` + `flag`), `key_order = '{INDEX_KEY_ORDER}'`, Zipf-skewed M:N `scope` (`{st.mult:.2f}x` avg mult, `{st.scope_info['single_parent_pct']:.1f}%` 1-parent, max `{st.scope_info['max_mult']}`, top-10 parents `{st.scope_info['top10_pct']:.1f}%`, mega-entity `5%` rows)",
        "",
        "---",
        "",
        f"## Scaling Growth Factors ({' → '.join(klabel(n) for n in SCALES)}, {SCALES[1] // SCALES[0]}× Jumps)",
        "",
        f"| Metric | OLS Slope (`{klabel(SCALES[0])}..{klabel(SCALES[-1])}`) | " + " | ".join(klabel(n) for n in SCALES) + " |",
        "|---|---:|" + "---:|" * len(SCALES),
    ] + growth_rows(st.scale_data, st.slopes) + [
        "",
        "## Workload A — Scoped Wide+JSONB Index Build & CIC",
        "",
        "### Plain `CREATE INDEX` (Entity vs Scoped) & Serial CIC",
        "",
        "| Rows | Entity Build | Plain Scoped (`vs ent`) | Plain Temp | CIC Total (`us/row`) | CIC Pass 1 | CIC Pass 2 | Pass2/Pass1 | Entity Size | Scoped Size (`+ovh%`) | CIC WAL | CIC Temp | CIC `RssAnon`/`VmHWM` |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for n in SCALES:
        e = st.scale_data[n]["entity"]
        sp = st.scale_data[n]["scoped_plain"]
        c = st.scale_data[n]["cic"]
        lines.append(
            f"| {n:,} | {e['elapsed_ms']:.0f} ms | {sp['elapsed_ms']:.0f} ms ({sp['ratio_vs_entity']:.2f}×) | "
            f"{sp['temp_bytes']/(1024*1024):.2f} MB | "
            f"{c['elapsed_ms']:.0f} ms ({c['elapsed_ms']*1000/n:.1f}) | "
            f"{c['pass1_ms']:.0f} ms | {c['pass2_ms']:.0f} ms | {c['pass2_ratio']:.2f}× | "
            f"{e['size_mb']:.2f} MB ({e['size_blks']} blks) | {c['size_mb']:.2f} MB (+{c['overhead_pct']:.1f}%) | "
            f"{c['wal_bytes']/(1024*1024):.2f} MB | {c['temp_bytes']/(1024*1024):.2f} MB | "
            f"{c['peak_anon_mb']:.1f} / {c['peak_hwm_mb']:.1f} MB |"
        )

    cp = st.scale_data[N_MAX]["cic_par2"]
    cs = st.scale_data[N_MAX]["cic"]
    lines += [
        "",
        f"### Parallel-Pass-1 CIC vs Serial CIC (`N = {N_MAX:,}`, `maintenance_work_mem = {MAINT_MEM}`)",
        "",
        "| Mode | Total | Pass 1 | Pass 2 (Scope) | Pass2 / Pass1 | WAL | Temp | Peak `RssAnon`/`VmHWM` |",
        "|---|---:|---:|---:|---:|---:|---:|---:|",
        f"| Serial CIC (`maint_workers = 0`) | {cs['elapsed_ms']:.0f} ms | {cs['pass1_ms']:.0f} ms | {cs['pass2_ms']:.0f} ms | **{cs['pass2_ratio']:.2f}×** | {cs['wal_bytes']/(1024*1024):.2f} MB | {cs['temp_bytes']/(1024*1024):.2f} MB | {cs['peak_anon_mb']:.1f} / {cs['peak_hwm_mb']:.1f} MB |",
        f"| Parallel-Pass-1 CIC (`maint_workers = 2`) | {cp['elapsed_ms']:.0f} ms | {cp['pass1_ms']:.0f} ms | {cp['pass2_ms']:.0f} ms | **{cp['pass2_ratio']:.2f}×** | {cp['wal_bytes']/(1024*1024):.2f} MB | {cp['temp_bytes']/(1024*1024):.2f} MB | {cp['peak_anon_mb']:.1f} / {cp['peak_hwm_mb']:.1f} MB |",
    ]

    lines += [
        "",
        "## Workload B — VACUUM Memory & Speed (`maintenance_work_mem = 4MB`)",
        "",
        "| Stage | Rows | Status | Time | Peak `RssAnon` | Peak `VmHWM` | WAL | Index Blks Touched | Size After |",
        "|---|---:|---|---:|---:|---:|---:|---:|---:|",
    ]
    for n in SCALES:
        v = st.scale_data[n]["vac1"]
        lines.append(
            f"| First VACUUM after CIC | {n:,} | {v['status']} | {v['elapsed_ms']:.0f} ms | "
            f"{v['peak_anon_mb']:.1f} MB | {v['peak_hwm_mb']:.1f} MB | {v['wal_bytes']/(1024*1024):.2f} MB | "
            f"{v['blks_touched']:,} | {v['size_after_mb']:.2f} MB |"
        )
    lines.append(
        f"| Post-Delete VACUUM (2% del) | {N_MAX:,} | {st.vac2['status']} | {st.vac2['elapsed_ms']:.0f} ms | "
        f"{st.vac2['peak_anon_mb']:.1f} MB | {st.vac2['peak_hwm_mb']:.1f} MB | {st.vac2['wal_bytes']/(1024*1024):.2f} MB | "
        f"{st.vac2['blks_touched']:,} | {st.vac2['size_after_mb']:.2f} MB |"
    )

    lines += [
        "",
        f"## Workload C — OLTP Mutations & Dirty-State Resilience (`N = {N_MAX:,}`)",
        "",
        "| Operation | Latency | Per-Row | WAL | Peak `RssAnon` / `VmHWM` |",
        "|---|---:|---:|---:|---:|",
        f"| Single-row INSERT (1,000 rows) | {st.c_ins['elapsed_ms']:.1f} ms | {st.c_ins['elapsed_ms']/1000.0:.3f} ms/row | {st.c_ins['wal_bytes']/(1024*1024):.2f} MB | {st.c_ins['peak_anon_mb']:.1f} / {st.c_ins['peak_hwm_mb']:.1f} MB |",
        f"| Bulk UPDATE (2% = 3,200 rows: 73% JSONB, 13% soft-del, 14% scalar) | {st.c_upd['elapsed_ms']:.1f} ms | {st.c_upd['elapsed_ms']/3200.0:.3f} ms/row | {st.c_upd['wal_bytes']/(1024*1024):.2f} MB | {st.c_upd['peak_anon_mb']:.1f} / {st.c_upd['peak_hwm_mb']:.1f} MB |",
        f"| Bulk DELETE (2% scattered = 3,200 rows) | {st.c_del['elapsed_ms']:.1f} ms | {st.c_del['elapsed_ms']/3200.0:.3f} ms/row | {st.c_del['wal_bytes']/(1024*1024):.2f} MB | {st.c_del['peak_anon_mb']:.1f} / {st.c_del['peak_hwm_mb']:.1f} MB |",
        "",
        "### Dirty-State Query Latency (Before vs After 2% Unvacuumed Deletes)",
        "",
        "| Query | Clean (ms) | Dirty 2% (ms) | Dirty/Clean Ratio | Clean Blks | Dirty Blks |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for qk in st.dirty_probe_keys:
        lines.append(
            f"| `{qk}` | {st.clean_q[qk]['exec_ms']:.2f} | {st.dirty_q[qk]['exec_ms']:.2f} | "
            f"**{st.dirty_ratios[qk]:.2f}×** | {st.clean_q[qk]['total_blks']:,} | {st.dirty_q[qk]['total_blks']:,} |"
        )

    lines += [
        "",
        "## Workload D — Analytical Aggregations & Query Scaling",
        "",
        f"### Query Complexity Scaling (`{' → '.join(klabel(n) for n in SCALES)}`, 1× → {SCALES[1] // SCALES[0]}× → {SCALES[-1] // SCALES[0]}× Data Size)",
        "",
        "| Query | " + " | ".join(f"{klabel(n)} (`ms` / `blks`)" for n in SCALES) + f" | {SCALES[-1] // SCALES[0]}× Time Growth | {SCALES[-1] // SCALES[0]}× Blks Growth |",
        "|---|" + "---:|" * (len(SCALES) + 2),
    ] + query_growth_rows(st.scale_data)

    lines += [
        "",
        f"### Full Analytical Suite Q1–Q8 (`N = {N_MAX:,}`, `work_mem = {WORK_MEM}`; 1v4 Parallel at `work_mem = {PAR_WORK_MEM}`)",
        "",
        "| Query | 1w (`4MB`) | 1w (`16MB`) | 4w (`16MB`) | Workers Launched | 4w Speedup | Shared Hit / Read | Plan Blks | Peak `RssAnon` | Plan |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for qk in QUERIES:
        q1 = st.full_q[qk]
        if qk in st.par_q:
            q4 = st.par_q[qk]
            w1p_s = f"{q4['exec_1w_par_ms']:.2f} ms"
            w4_s = f"{q4['exec_ms']:.2f} ms"
            wl_s = f"{q4['workers_launched']}"
            sp_s = f"**{q4['speedup_vs_1w']:.2f}×**"
        else:
            w1p_s = "-"
            w4_s = "-"
            wl_s = "-"
            sp_s = "-"
        lines.append(
            f"| `{qk}` | {q1['exec_ms']:.2f} ms | {w1p_s} | {w4_s} | {wl_s} | {sp_s} | "
            f"{q1['shared_hit']:,} / {q1['shared_read']:,} | {q1['plan_blks']:,} | {q1['peak_anon_mb']:.1f} MB | `{q1['plan_tag']}` |"
        )

    page_util_str = f"{st.clean_bd['page_utilization_pct']:.2f}%" if st.clean_bd["available"] else "n/a"
    clean_dead_str = f"{st.clean_bd['dead_space_pct']:.2f}%" if st.clean_bd["available"] else "n/a"
    clean_bd_src = st.clean_bd["fn"] if st.clean_bd["available"] else "sift_index_size_breakdown unavailable"
    churn_dead_str = f"{st.g_wa['breakdown']['dead_space_pct']:.2f}%" if st.g_wa["breakdown"]["available"] else "n/a"
    churn_bd_src = st.g_wa["breakdown"]["fn"] if st.g_wa["breakdown"]["available"] else "sift_index_size_breakdown unavailable"
    sec_row_md = (
        "| " + " | ".join(f"{st.clean_bd['sections_b_per_row'][s]:.2f}" for s in BREAKDOWN_SECTIONS) + " |"
        if st.clean_bd["available"]
        else "| " + " | ".join("n/a" for _ in BREAKDOWN_SECTIONS) + " |"
    )

    lines += [
        "",
        f"## Workload F — Compact v6 & Real-World Probes (`N = {N_MAX:,}`)",
        "",
        "| Probe | Measurement | Details |",
        "|---|---:|---|",
        f"| **F1: 6-CTE Planning Latency** | **{st.cte6['plan_ms']:.2f} ms** | `EXPLAIN (SUMMARY, BUFFERS)` median of 5 runs, plan blks `{st.cte6['plan_blks']:,}` (`plan={st.cte6['plan_tag']}`) |",
        f"| **F2: Pair-Edit Refresh (`-5/+5` pairs)** | **{st.pair_first['exec_ms']:.2f} ms** (1st) / **{st.pair_med['exec_ms']:.2f} ms** (med) | DML `{st.pair_dml['elapsed_ms']:.2f} ms`, clean `{st.pair_before['exec_ms']:.2f} ms`, blks `{st.pair_first['total_blks']:,}` (`plan={st.pair_first['plan_tag']}`) |",
        f"| **F3: JSONB Variant-Exception (0.5% str in float8)** | **{st.var_q['exec_ms']:.2f} ms** | total blks `{st.var_q['total_blks']:,}`, heap scan blks `{st.var_q['heap_scan_blks']:,}` (`plan={st.var_q['plan_tag']}`) |",
        f"| **F4a: Soft-Delete `deleted_at IS NULL`** | **{st.f4_null['exec_ms']:.2f} ms** | parent `{MID_PARENT_ID}`, blks `{st.f4_null['total_blks']:,}`, plan blks `{st.f4_null['plan_blks']:,}` (`plan={st.f4_null['plan_tag']}`) |",
        f"| **F4b: Soft-Delete `deleted_at IS NOT NULL`** | **{st.f4_not_null['exec_ms']:.2f} ms** | parent `{MEGA_PARENT_ID}`, blks `{st.f4_not_null['total_blks']:,}`, plan blks `{st.f4_not_null['plan_blks']:,}` (`plan={st.f4_not_null['plan_tag']}`) |",
        f"| **F5a: Single-Entity Scoped Agg** | **{st.f5_single['exec_ms']:.2f} ms** | entity `{MEDIAN_ENTITY_ID}`, blks `{st.f5_single['total_blks']:,}`, plan blks `{st.f5_single['plan_blks']:,}` (`plan={st.f5_single['plan_tag']}`) |",
        f"| **F5b: Mega-Entity Scoped Agg (`5%` rows)** | **{st.f5_mega['exec_ms']:.2f} ms** | entity `{MEGA_ENTITY_ID}`, blks `{st.f5_mega['total_blks']:,}`, plan blks `{st.f5_mega['plan_blks']:,}` (`plan={st.f5_mega['plan_tag']}`) |",
        f"| **F6: `MATERIALIZED` CTE Scope Join** | **{st.f6_cte['exec_ms']:.2f} ms** | parent `{MEGA_PARENT_ID}`, blks `{st.f6_cte['total_blks']:,}`, plan blks `{st.f6_cte['plan_blks']:,}` (`plan={st.f6_cte['plan_tag']}`) |",
        f"| **F7: JSONB Exists (`analysis ? 'flag'`)** | **{st.f7_exists['exec_ms']:.2f} ms** | blks `{st.f7_exists['total_blks']:,}`, heap blks `{st.f7_exists['heap_scan_blks']:,}`, re-read rows `{st.f7_exists['heap_reread_rows']:,}`, plan blks `{st.f7_exists['plan_blks']:,}` (`plan={st.f7_exists['plan_tag']}`) |",
        f"| **F8: Scoped `ORDER BY updated_at DESC LIMIT 20`** | **{st.f8_upd_top20['exec_ms']:.2f} ms** | parent `{MEGA_PARENT_ID}`, blks `{st.f8_upd_top20['total_blks']:,}`, plan blks `{st.f8_upd_top20['plan_blks']:,}` (`plan={st.f8_upd_top20['plan_tag']}`) |",
        f"| **Planning Shared Buffers (Q1–Q8, F4–F8)** | **{st.plan_buffers_median:.1f} blks** (med) | max `{st.plan_buffers_max:,}` blks, median planning time `{st.plan_ms_median:.2f} ms` across `{len(st.all_plan_blks)}` queries |",
        "",
        "## Workload E — Idle Pooled Backend Memory",
        "",
        f"- **Cache cap & indexes touched:** `sift.backend_cache_kb = '{st.idle['cache_cap']}'`, {st.idle['indexes_touched']} indexes (`idx_events_scoped` + `entity_parents_journal` + {st.idle['extra_tables']} `sift_col` tables)",
        f"- **Resident `RssAnon` (before → active → idle):** `{st.idle['anon_before_mb']:.2f} MB` → `{st.idle['anon_active_mb']:.2f} MB` → **`{st.idle['anon_idle_mb']:.2f} MB`** (`+{st.idle['anon_delta_mb']:.2f} MB` delta, `VmRSS = {st.idle['rss_idle_mb']:.2f} MB`, `VmHWM = {st.idle['hwm_mb']:.2f} MB`, `+{st.idle['delta_kb_per_index']:.1f} KB/index`)",
        "",
        f"## Workload G — Index Size, Page Utilization, WAL Efficiency & Compaction WA (`N = {N_MAX:,}` & WA Probe `N = {st.g_wa['n_base']:,}`)",
        "",
        "### Size & Page Utilization",
        "",
        "| Metric | Value | Notes |",
        "|---|---:|---|",
        f"| **Heap + TOAST (`pg_table_size`)** | **{st.g_size['heap_b_per_row']:.1f} B/row** ({st.g_size['heap_size_mb']:.2f} MB) | `pg_relation_size={st.heap_rel_bytes/(1024*1024):.2f} MB`, `pg_total_relation_size={st.heap_total_bytes/(1024*1024):.2f} MB` |",
        f"| **Entity Index** | **{st.g_size['entity_b_per_row']:.1f} B/row** ({st.ent_bytes_max/(1024*1024):.2f} MB) | entity-only `sift_col` |",
        f"| **Scoped Index (CIC)** | **{st.g_size['scoped_b_per_row']:.1f} B/row** ({st.cic_bytes_max/(1024*1024):.2f} MB) | `+{st.g_size['scope_overhead_pct']:.1f}%` vs entity |",
        f"| **Scoped / Heap Ratio** | **{st.g_size['scoped_to_heap_ratio']:.3f}×** | `scoped_bytes / pg_table_size('events')` |",
        f"| **Page Utilization (post-CIC)** | **{page_util_str}** | {clean_bd_src} |",
        f"| **Dead Space (post-CIC)** | **{clean_dead_str}** | {clean_bd_src} |",
        "",
        "### Section Breakdown (`B/row`)",
        "",
        "| `granules` | `dicts` | `zones` | `preagg` | `delvec` | `scope` | `slack` |",
        "|---:|---:|---:|---:|---:|---:|---:|",
        sec_row_md,
        "",
        "### WAL Efficiency & Multi-Wave Compaction Write Amplification",
        "",
        "| Metric | Value | Notes |",
        "|---|---:|---|",
        f"| **CIC Build WAL** | **{st.g_wal['cic_wal_b_per_row']:.1f} B/row** | {st.cic_wal_max/(1024*1024):.2f} MB total at `N = {N_MAX:,}` |",
        f"| **OLTP 1-row INSERT WAL** | **{st.g_wal['oltp_ins1_wal_b_per_row']:.1f} B/row** | 1,000 × 1-row autocommit INSERTs |",
        f"| **2% UPDATE WAL** | **{st.g_wal['update_2pct_wal_b_per_row']:.1f} B/row** | {int(st.n_2pct):,} rows |",
        f"| **2% DELETE WAL** | **{st.g_wal['delete_2pct_wal_b_per_row']:.1f} B/row** | {int(st.n_2pct):,} scattered rows |",
        f"| **Compaction Write Amplification ({st.g_wa['waves']}× 2% churn + VAC)** | **{st.g_wa['write_amplification']:.2f}×** | `WAL {st.g_wa['total_wal_bytes']/(1024*1024):.2f} MB` / `logical {st.g_wa['logical_inserted_bytes']/(1024*1024):.2f} MB` (`{st.g_wa['elapsed_ms']:.0f} ms` at `N = {st.g_wa['n_base']:,}`) |",
        f"| **Post-Churn Index Growth Ratio** | **{st.g_wa['index_growth_ratio']:.3f}×** | `{st.g_wa['init_idx_bytes']/(1024*1024):.2f} MB` → `{st.g_wa['post_idx_bytes']/(1024*1024):.2f} MB` |",
        f"| **Post-Churn Dead Space** | **{churn_dead_str}** | {churn_bd_src} |",
        "",
    ]

    with open(st.md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print(f"[v6-core] wrote {st.tsv_path} and {st.md_path}" + (" and docs/results/v6-core.tsv" if args.docs else ""), flush=True)
