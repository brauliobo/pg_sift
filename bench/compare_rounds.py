#!/usr/bin/env python3
"""
Compare two benchmark harness result files (TSV, a baseline round and a candidate round)
and emit a side-by-side Markdown delta table highlighting PASS / IMPROVED / REGRESSED gates.
Each file is read at its own largest
scale (the `scales` meta row; 160000 for files without one), so a 10k..160k round compares
against a 10k..1M sign-off round.

  bench/compare_rounds.py BASE.tsv CANDIDATE.tsv [--tol-pct 5] [-o FILE] [--fail-on-gate]
  bench/compare_rounds.py BASE.tsv CANDIDATE.tsv --strict-v6-gates   Spec 28 scorecard (M/P/F/S/U), exit 1 on a FAIL

Zero external Python dependencies.
"""

import argparse
import os
import re
import sys


def load_tsv(path):
    """Return (meta_dict, metrics_dict) keyed by (section, metric, scale)."""
    meta = {}
    data = {}
    with open(path, "r", encoding="utf-8") as f:
        for raw in f:
            line = raw.rstrip("\n")
            if not line or line.startswith("#") or line.startswith("section\t"):
                continue
            parts = line.split("\t")
            if len(parts) < 5:
                continue
            section, metric, scale, value, unit = parts[:5]
            notes = parts[5] if len(parts) > 5 else ""
            if section == "meta":
                meta[metric] = (value, notes)
            data[(section, metric, scale)] = {
                "value_str": value,
                "unit": unit,
                "notes": notes,
            }
    return meta, data


def klabel(n):
    return f"{n // 1000000}M" if n >= 1000000 and n % 1000000 == 0 else f"{n // 1000}k"


def ladder(meta):
    """(rows of the largest point, key of the slope rows) of a file: from its `scales` meta row, else the 10k..160k of the first rounds"""
    value = meta.get("scales", ("", ""))[0]
    if not value:
        return "160000", "10k-160k"
    ns = sorted(int(x) for x in value.split(","))
    return str(ns[-1]), f"{klabel(ns[0])}-{klabel(ns[-1])}"


def bind(key, n, slope):
    section, metric, scale = key
    return section, metric, {"@N": n, "@S": slope}.get(scale, scale)


def parse_float(val_str):
    if val_str is None:
        return None
    s = val_str.strip()
    if not s or s.lower() == "n/a":
        return None
    try:
        return float(s)
    except ValueError:
        return None


def extract_note_float(notes, pattern):
    m = re.search(pattern, notes or "")
    return float(m.group(1)) if m else None


def resolve_metric(data, key, n="160000"):
    """
    Look up (section, metric, scale) in data, with backward-compatible fallbacks
    for older TSVs (e.g. round0-v510.tsv generated before Workload G was added).
    """
    if key in data:
        return parse_float(data[key]["value_str"]), data[key]["unit"]

    section, metric, scale = key
    rows = float(n)
    if section == "G_size" and scale == n:
        if metric == "scoped_b_per_row":
            row = data.get(("A_build_cic", "size_mb", n))
            if row:
                v = extract_note_float(row["notes"], r"([0-9.]+)\s*B/row")
                if v is not None:
                    return v, "B/row"
                mb = parse_float(row["value_str"])
                if mb is not None:
                    return (mb * 1024 * 1024) / rows, "B/row"
        elif metric == "entity_b_per_row":
            row = data.get(("A_build_entity", "size_mb", n))
            if row:
                v = extract_note_float(row["notes"], r"([0-9.]+)\s*B/row")
                if v is not None:
                    return v, "B/row"
        elif metric == "scope_overhead_pct":
            row = data.get(("A_build_cic", "size_mb", n))
            if row:
                v = extract_note_float(row["notes"], r"\+([0-9.]+)%\s*vs\s*entity")
                if v is not None:
                    return v, "%"
        elif metric == "scoped_ex_uuid_b_per_row":
            v, unit = resolve_metric(data, ("G_size", "scoped_b_per_row", n), n)
            if v is not None:
                return v - UUID_BYTES, unit
    elif section == "A_build_cic" and metric == "pass2_ratio" and scale == n:
        pass1 = parse_float(data.get(("A_build_cic", "pass1_ms", n), {}).get("value_str"))
        pass2 = parse_float(data.get(("A_build_cic", "pass2_ms", n), {}).get("value_str"))
        if pass1 and pass2 is not None:
            return pass2 / pass1, "x"
    elif section == "G_wal" and scale == n:
        if metric == "cic_wal_b_per_row":
            row = data.get(("A_build_cic", "wal_mb", n))
            if row:
                v = extract_note_float(row["notes"], r"([0-9.]+)\s*B/row")
                if v is not None:
                    return v, "B/row"
        elif metric == "oltp_ins1_wal_b_per_row":
            row = data.get(("C_oltp", "insert_1000_wal_mb", n))
            if row:
                mb = parse_float(row["value_str"])
                if mb is not None:
                    return (mb * 1024 * 1024) / 1000.0, "B/row"
        elif metric == "update_2pct_wal_b_per_row":
            row = data.get(("C_oltp", "update_2pct_wal_mb", n))
            if row:
                mb = parse_float(row["value_str"])
                if mb is not None:
                    return (mb * 1024 * 1024) / (rows * 0.02), "B/row"
        elif metric == "delete_2pct_wal_b_per_row":
            row = data.get(("C_oltp", "delete_2pct_wal_mb", n))
            if row:
                mb = parse_float(row["value_str"])
                if mb is not None:
                    return (mb * 1024 * 1024) / (rows * 0.02), "B/row"

    return None, ""


# Gate specifications (scale "@N" is the largest point of each file, "@S" its slope key):
# (group, label, (section, metric, scale), direction, gate_op, gate_val, unit)
# direction: "lower" (lower is better) or "higher" (higher is better)
# gate_op: "<=" or ">=" or None
GATES = [
    # --- Build & Scaling Slopes ---
    ("Build & Scaling", "Entity Build Time (largest)", ("A_build_entity", "time_ms", "@N"), "lower", None, None, "ms"),
    ("Build & Scaling", "Plain Scoped Build Time (largest)", ("A_build_scoped_plain", "time_ms", "@N"), "lower", None, None, "ms"),
    ("Build & Scaling", "Plain Scoped / Entity Ratio (largest)", ("A_build_scoped_plain", "ratio_vs_entity", "@N"), "lower", "<=", 1.25, "x"),
    ("Build & Scaling", "Scoped CIC Total Time (largest)", ("A_build_cic", "total_ms", "@N"), "lower", None, None, "ms"),
    ("Build & Scaling", "Scoped CIC Pass 2 Time (largest)", ("A_build_cic", "pass2_ms", "@N"), "lower", None, None, "ms"),
    ("Build & Scaling", "CIC RssAnon Slope (ladder)", ("S_slopes", "cic_rss_anon_b_per_row", "@S"), "lower", "<=", 50.0, "B/row"),
    ("Build & Scaling", "CIC VmHWM Slope (ladder)", ("S_slopes", "cic_vm_hwm_b_per_row", "@S"), "lower", None, None, "B/row"),
    ("Build & Scaling", "CIC Temp Bytes Slope (ladder)", ("S_slopes", "cic_temp_b_per_row", "@S"), "lower", "<=", 50.0, "B/row"),
    ("Build & Scaling", "CIC Disk Size Slope (ladder)", ("S_slopes", "cic_disk_b_per_row", "@S"), "lower", "<=", 42.0, "B/row"),
    ("Build & Scaling", "CIC WAL Slope (ladder)", ("S_slopes", "cic_wal_b_per_row", "@S"), "lower", "<=", 45.0, "B/row"),
    ("Build & Scaling", "First VACUUM RssAnon Slope", ("S_slopes", "vac1_rss_anon_b_per_row", "@S"), "lower", "<=", 50.0, "B/row"),

    # --- Workload G: Size, Utilization, WAL & Compaction WA ---
    ("Size, WAL & WA (G)", "Scoped Index Size (largest)", ("G_size", "scoped_b_per_row", "@N"), "lower", "<=", 42.0, "B/row"),
    ("Size, WAL & WA (G)", "Entity Index Size (largest)", ("G_size", "entity_b_per_row", "@N"), "lower", None, None, "B/row"),
    ("Size, WAL & WA (G)", "Scoped / Heap Ratio (largest)", ("G_size", "scoped_to_heap_ratio", "@N"), "lower", "<=", 0.15, "x"),
    ("Size, WAL & WA (G)", "Scope Size Overhead (largest)", ("G_size", "scope_overhead_pct", "@N"), "lower", "<=", 5.0, "%"),
    ("Size, WAL & WA (G)", "Page Utilization (post-CIC)", ("G_size", "page_utilization_pct", "@N"), "higher", ">=", 95.0, "%"),
    ("Size, WAL & WA (G)", "CIC Build WAL (largest)", ("G_wal", "cic_wal_b_per_row", "@N"), "lower", "<=", 45.0, "B/row"),
    ("Size, WAL & WA (G)", "OLTP 1-row INSERT WAL", ("G_wal", "oltp_ins1_wal_b_per_row", "@N"), "lower", "<=", 220.0, "B/row"),
    ("Size, WAL & WA (G)", "2% UPDATE WAL", ("G_wal", "update_2pct_wal_b_per_row", "@N"), "lower", "<=", 120.0, "B/row"),
    ("Size, WAL & WA (G)", "2% DELETE WAL", ("G_wal", "delete_2pct_wal_b_per_row", "@N"), "lower", "<=", 45.0, "B/row"),
    ("Size, WAL & WA (G)", "Compaction Write Amplification", ("G_compaction", "write_amplification", "40000"), "lower", "<=", 3.5, "x"),
    ("Size, WAL & WA (G)", "Post-Churn Dead Space", ("G_compaction", "dead_space_pct", "40000"), "lower", "<=", 15.0, "%"),
    ("Size, WAL & WA (G)", "Post-Churn Index Growth Ratio", ("G_compaction", "index_growth_ratio", "40000"), "lower", "<=", 1.15, "x"),

    # --- VACUUM, OLTP & Dirty-State ---
    ("VACUUM & OLTP", "First VACUUM Time (largest)", ("B_vac_after_cic", "time_ms", "@N"), "lower", None, None, "ms"),
    ("VACUUM & OLTP", "Post-Delete VACUUM Time (largest)", ("B_vac_post_del", "time_ms", "@N"), "lower", None, None, "ms"),
    ("VACUUM & OLTP", "1,000x 1-row INSERT Time", ("C_oltp", "insert_1000_total_ms", "@N"), "lower", None, None, "ms"),
    ("VACUUM & OLTP", "2% UPDATE Time", ("C_oltp", "update_2pct_ms", "@N"), "lower", None, None, "ms"),
    ("VACUUM & OLTP", "2% DELETE Time", ("C_oltp", "delete_2pct_ms", "@N"), "lower", None, None, "ms"),
    ("Dirty-State", "Q1 Scope Whole Dirty Ratio", ("C_dirty", "Q1_scope_whole_dirty_ratio", "@N"), "lower", "<=", 1.50, "x"),
    ("Dirty-State", "Q2 Scope Month Dirty Ratio", ("C_dirty", "Q2_scope_month_dirty_ratio", "@N"), "lower", "<=", 1.50, "x"),
    ("Dirty-State", "Q3 GroupBy Parent Dirty Ratio", ("C_dirty", "Q3_scope_groupby_parent_dirty_ratio", "@N"), "lower", "<=", 1.50, "x"),
    ("Dirty-State", "Q7 Top-20 Dirty Ratio", ("C_dirty", "Q7_scope_top20_unfilt_dirty_ratio", "@N"), "lower", "<=", 1.50, "x"),

    # --- Analytical, Parallel 1v4, Probes & Idle RSS ---
    ("Queries & Parallel", "Q1 Scope Whole (1w)", ("D_analytical", "Q1_scope_whole_1w_ms", "@N"), "lower", None, None, "ms"),
    ("Queries & Parallel", "Q3 GroupBy Parent (1w)", ("D_analytical", "Q3_scope_groupby_parent_1w_ms", "@N"), "lower", None, None, "ms"),
    ("Queries & Parallel", "Q4 High-Card GroupBy (1w)", ("D_analytical", "Q4_highcard_groupby_1w_ms", "@N"), "lower", None, None, "ms"),
    ("Queries & Parallel", "Q5 Count Distinct (1w)", ("D_analytical", "Q5_count_distinct_1w_ms", "@N"), "lower", None, None, "ms"),
    ("Queries & Parallel", "Q6 JSONB Array Shred (1w)", ("D_analytical", "Q6_jsonb_shred_array_1w_ms", "@N"), "lower", None, None, "ms"),
    ("Queries & Parallel", "Q4 Parallel 1v4 Speedup", ("D_parallel", "Q4_highcard_groupby_speedup", "@N"), "higher", ">=", 2.00, "x"),
    ("Queries & Parallel", "Q5 Parallel 1v4 Speedup", ("D_parallel", "Q5_count_distinct_speedup", "@N"), "higher", ">=", 2.00, "x"),
    ("Probes & Idle RSS", "6-CTE Planning Latency", ("F_probes", "cte6_plan_ms", "@N"), "lower", "<=", 2.00, "ms"),
    ("Probes & Idle RSS", "6-CTE Planning Buffers", ("F_probes", "cte6_plan_blks", "@N"), "lower", "<=", 20.0, "blks"),
    ("Probes & Idle RSS", "Planning Shared Buffers (med)", ("F_probes", "plan_buffers_median", "@N"), "lower", "<=", 10.0, "blks"),
    ("Probes & Idle RSS", "Pair-Edit Refresh (1st Q1)", ("F_probes", "pair_edit_q1_first_ms", "@N"), "lower", None, None, "ms"),
    ("Probes & Idle RSS", "JSONB Variant Exception Query", ("F_probes", "jsonb_variant_exc_ms", "@N"), "lower", None, None, "ms"),
    ("Probes & Idle RSS", "F4 Soft-Delete IS NULL", ("F_probes", "f4_soft_del_null_ms", "@N"), "lower", None, None, "ms"),
    ("Probes & Idle RSS", "F4 Soft-Delete IS NOT NULL", ("F_probes", "f4_soft_del_not_null_ms", "@N"), "lower", None, None, "ms"),
    ("Probes & Idle RSS", "F5 Single-Entity Scoped Agg", ("F_probes", "f5_single_entity_ms", "@N"), "lower", None, None, "ms"),
    ("Probes & Idle RSS", "F5 Mega-Entity Scoped Agg", ("F_probes", "f5_mega_entity_ms", "@N"), "lower", None, None, "ms"),
    ("Probes & Idle RSS", "F6 MATERIALIZED CTE Scope Join", ("F_probes", "f6_materialized_cte_ms", "@N"), "lower", None, None, "ms"),
    ("Probes & Idle RSS", "F7 JSONB Exists (?) Latency", ("F_probes", "f7_jsonb_exists_ms", "@N"), "lower", None, None, "ms"),
    ("Probes & Idle RSS", "F7 JSONB Exists Heap Blks", ("F_probes", "f7_jsonb_exists_heap_blks", "@N"), "lower", "<=", 0.0, "blks"),
    ("Probes & Idle RSS", "F8 ORDER BY updated_at Top-20", ("F_probes", "f8_updated_at_top20_ms", "@N"), "lower", None, None, "ms"),
    ("Probes & Idle RSS", "Idle Pooled Backend RssAnon", ("E_idle_backend", "rss_anon_idle_mb", "34"), "lower", "<=", 8.0, "MB"),
    ("Probes & Idle RSS", "Idle Pooled RssAnon Delta", ("E_idle_backend", "rss_anon_delta_mb", "34"), "lower", "<=", 4.0, "MB"),
    ("Probes & Idle RSS", "Total Benchmark Suite Time", ("meta", "bench_suite_time", "-"), "lower", "<=", 90.0, "s"),
]


UUID_BYTES = 16
V6_CATEGORIES = {"M": "Memory", "P": "Perf & Scale", "F": "Footprint", "S": "MVCC & Safety", "U": "Usability & CI"}
V6_ORDER = [f"{c}{i}" for c, n in (("M", 4), ("P", 7), ("F", 6), ("S", 4), ("U", 7)) for i in range(1, n + 1)]

# Spec 28 section 3.1 hard gates measured by the harness TSV: (gate, label, key, op, value, unit)
V6_GATES = [
    ("M1", "Scoped CIC RssAnon slope", ("S_slopes", "cic_rss_anon_b_per_row", "@S"), "<=", 0.5, "B/row"),
    ("M2", "First VACUUM RssAnon slope", ("S_slopes", "vac1_rss_anon_b_per_row", "@S"), "<=", 0.5, "B/row"),
    ("M4", "Idle pooled backend RssAnon", ("E_idle_backend", "rss_anon_idle_mb", "34"), "<=", 8.0, "MB"),
    ("M4", "Idle pooled RssAnon delta", ("E_idle_backend", "rss_anon_delta_mb", "34"), "<=", 4.0, "MB"),
    ("P1", "Q4 high-card GROUP BY 4w speedup", ("D_parallel", "Q4_highcard_groupby_speedup", "@N"), ">=", 3.2, "x"),
    ("P1", "Q5 count(DISTINCT) 4w speedup", ("D_parallel", "Q5_count_distinct_speedup", "@N"), ">=", 3.2, "x"),
    ("P3", "Q1 dirty ratio", ("C_dirty", "Q1_scope_whole_dirty_ratio", "@N"), "<", 1.10, "x"),
    ("P3", "Q2 dirty ratio", ("C_dirty", "Q2_scope_month_dirty_ratio", "@N"), "<", 1.10, "x"),
    ("P3", "Q3 dirty ratio", ("C_dirty", "Q3_scope_groupby_parent_dirty_ratio", "@N"), "<", 1.10, "x"),
    ("P3", "Q7 dirty ratio", ("C_dirty", "Q7_scope_top20_unfilt_dirty_ratio", "@N"), "<", 1.10, "x"),
    ("P4", "F7 jsonb_exists heap blks", ("F_probes", "f7_jsonb_exists_heap_blks", "@N"), "<=", 0.0, "blks"),
    ("P4", "JSONB variant-exception heap blks", ("F_probes", "jsonb_variant_heap_blks", "@N"), "<=", 0.0, "blks"),
    ("P5", "Warm planning buffers (median)", ("F_probes", "plan_buffers_median", "@N"), "<=", 4.0, "blks"),
    ("P5", "6-CTE planning latency", ("F_probes", "cte6_plan_ms", "@N"), "<", 2.0, "ms"),
    ("P6", "CIC pass 2 / pass 1", ("A_build_cic", "pass2_ratio", "@N"), "<=", 0.15, "x"),
    ("P6", "CIC temp bytes slope", ("S_slopes", "cic_temp_b_per_row", "@S"), "<=", 0.0, "B/row"),
    ("P6", "CIC temp files", ("A_build_cic", "temp_mb", "@N"), "<=", 0.0, "MB"),
    ("P6", "Plain scoped temp files", ("A_build_scoped_plain", "temp_mb", "@N"), "<=", 0.0, "MB"),
    ("F1", "Scoped index size", ("G_size", "scoped_b_per_row", "@N"), "<=", 33.0, "B/row"),
    ("F1", "Scoped index size ex-UUID", ("G_size", "scoped_ex_uuid_b_per_row", "@N"), "<=", 16.0, "B/row"),
    ("F2", "Scope size overhead", ("G_size", "scope_overhead_pct", "@N"), "<=", 5.0, "%"),
    ("F3", "Page utilization", ("G_size", "page_utilization_pct", "@N"), ">=", 95.0, "%"),
    ("F4", "Tail free space after churn", ("G_compaction", "tail_free_space_pct", "40000"), "<=", 5.0, "%"),
    ("F5", "Compaction write amplification", ("G_compaction", "write_amplification", "40000"), "<=", 2.35, "x"),
    ("F6", "CIC build WAL", ("G_wal", "cic_wal_b_per_row", "@N"), "<=", 45.0, "B/row"),
    ("F6", "OLTP 1-row INSERT WAL", ("G_wal", "oltp_ins1_wal_b_per_row", "@N"), "<=", 220.0, "B/row"),
    ("F6", "2% UPDATE WAL", ("G_wal", "update_2pct_wal_b_per_row", "@N"), "<=", 120.0, "B/row"),
    ("F6", "2% DELETE WAL", ("G_wal", "delete_2pct_wal_b_per_row", "@N"), "<=", 45.0, "B/row"),
]

# Spec 28 gates verified by test suites, not by the harness TSV
V6_SUITE_GATES = {
    "M3": "test/t/mem/slope.sh + scripts/check-memory.sh",
    "P2": "bench_scaling.sh Suite B",
    "P7": "opt_adaptive_bench.py + bench_scaling.sh Suite C",
    "S1": "test/isolation/specs/f2_mvcc_abort_2pc.spec",
    "S2": "030_v6_crash_standby_torture.pl",
    "S3": "v6_mvcc_epq_trunc.spec",
    "S4": "test/sql/f2_subtrans_overflow.sql",
    "U1": "test/sql/guc_surface.sql",
    "U2": "test/sql/guc_surface.sql",
    "U3": "scripts/ci.sh warm wall-clock",
    "U4": "make SANITIZE=1 + scripts/leakcheck.sh",
    "U5": "strict -Wall -Wextra build",
    "U6": "scripts/check-neutral.sh",
    "U7": "test/t/upgrade_51_to_60.sh",
}


def fmt_val(val, unit):
    if val is None:
        return "n/a"
    if unit == "x":
        return f"{val:.2f}×" if val >= 1.0 or val == 0.0 else f"{val:.3f}×"
    if unit == "%":
        return f"{val:.2f}%"
    if unit in ("blks", "rows"):
        return f"{val:.0f} {unit}" if abs(val - round(val)) < 1e-6 else f"{val:.1f} {unit}"
    if unit in ("ms", "MB", "B/row", "s"):
        return f"{val:.2f} {unit}"
    return f"{val:.2f}"


def fmt_gate(op, val, unit):
    if op is None or val is None:
        return "—"
    return f"`{op} {fmt_val(val, unit)}`"


def eval_gate(val, op, target):
    if val is None or op is None or target is None:
        return None
    if op == "<":
        return val < target
    if op == "<=":
        return val <= target
    if op == ">=":
        return val >= target
    return None


def eval_delta(base, cand, direction, tol_pct):
    if base is None or cand is None:
        return "n/a", None
    diff = cand - base
    if base == 0.0:
        if cand == 0.0:
            return "0.0%", "SAME"
        pct_str = "+inf%" if cand > 0 else "-inf%"
        is_better = (cand < 0) if direction == "lower" else (cand > 0)
        return pct_str, ("IMPROVED" if is_better else "REGRESSED")

    pct = (diff / abs(base)) * 100.0
    sign = "+" if pct > 0 else ""
    pct_str = f"{sign}{pct:.1f}%"
    if abs(pct) <= tol_pct:
        return pct_str, "SAME"
    is_better = (diff < 0) if direction == "lower" else (diff > 0)
    return pct_str, ("IMPROVED" if is_better else "REGRESSED")


def compare_rounds(base_path, cand_path, tol_pct=5.0):
    base_meta, base_data = load_tsv(base_path)
    cand_meta, cand_data = load_tsv(cand_path)

    base_n, base_s = ladder(base_meta)
    cand_n, cand_s = ladder(cand_meta)
    base_name = base_meta.get("round", (os.path.basename(base_path), ""))[0]
    cand_name = cand_meta.get("round", (os.path.basename(cand_path), ""))[0]
    base_ver = base_meta.get("pg_sift_version", ("?", ""))[0]
    cand_ver = cand_meta.get("pg_sift_version", ("?", ""))[0]

    counts = {
        "PASS": 0,
        "FAIL": 0,
        "IMPROVED": 0,
        "REGRESSED": 0,
        "SAME": 0,
        "NA": 0,
    }

    rows_md = []
    for group, label, key, direction, gate_op, gate_val, unit in GATES:
        bval, bunit = resolve_metric(base_data, bind(key, base_n, base_s), base_n)
        cval, cunit = resolve_metric(cand_data, bind(key, cand_n, cand_s), cand_n)
        u = unit or cunit or bunit

        delta_str, trend = eval_delta(bval, cval, direction, tol_pct)
        gate_ok = eval_gate(cval, gate_op, gate_val)

        if gate_ok is True:
            counts["PASS"] += 1
        elif gate_ok is False:
            counts["FAIL"] += 1

        if trend == "IMPROVED":
            counts["IMPROVED"] += 1
        elif trend == "REGRESSED":
            counts["REGRESSED"] += 1
        elif trend == "SAME":
            counts["SAME"] += 1
        else:
            counts["NA"] += 1

        badges = []
        if gate_ok is True:
            badges.append("**PASS**")
        elif gate_ok is False:
            badges.append("**FAIL**")
        if trend == "IMPROVED":
            badges.append("**IMPROVED**")
        elif trend == "REGRESSED":
            badges.append("**REGRESSED**")
        elif trend == "SAME":
            badges.append("SAME")
        elif not badges:
            badges.append("N/A")

        verdict = " / ".join(badges)
        rows_md.append(
            f"| {group} | {label} | {fmt_val(bval, u)} | {fmt_val(cval, u)} | "
            f"{delta_str} | {fmt_gate(gate_op, gate_val, u)} | {verdict} |"
        )

    lines = [
        f"# `pg_sift` Round Comparison: `{base_name}` vs `{cand_name}`",
        "",
        f"- **Baseline:** `{base_name}` (`pg_sift` `{base_ver}`, `{base_path}`)",
        f"- **Candidate:** `{cand_name}` (`pg_sift` `{cand_ver}`, `{cand_path}`)",
        f"- **Noise Tolerance:** `±{tol_pct:.1f}%`",
        (
            f"- **Gate Summary:** **{counts['PASS']} PASS**, **{counts['FAIL']} FAIL** | "
            f"**Delta Summary:** **{counts['IMPROVED']} IMPROVED**, **{counts['REGRESSED']} REGRESSED**, "
            f"{counts['SAME']} SAME, {counts['NA']} N/A"
        ),
        "",
        f"| Group | Metric | `{base_name}` | `{cand_name}` | Delta | Target Gate | Status |",
        "|---|---|---:|---:|---:|---:|---|",
    ] + rows_md + [""]

    return "\n".join(lines), counts


def gate_verdict(statuses):
    if "FAIL" in statuses:
        return "FAIL"
    return "PASS" if "PASS" in statuses else "N/A"


def strict_v6_scorecard(base_path, cand_path, tol_pct=5.0):
    base_meta, base_data = load_tsv(base_path)
    cand_meta, cand_data = load_tsv(cand_path)
    base_n, base_s = ladder(base_meta)
    cand_n, cand_s = ladder(cand_meta)
    base_name = base_meta.get("round", (os.path.basename(base_path), ""))[0]
    cand_name = cand_meta.get("round", (os.path.basename(cand_path), ""))[0]

    statuses = {gate: [] for gate in V6_ORDER}
    rows = []
    for gate, label, key, op, target, unit in V6_GATES:
        direction = "higher" if op == ">=" else "lower"
        bval, _ = resolve_metric(base_data, bind(key, base_n, base_s), base_n)
        cval, _ = resolve_metric(cand_data, bind(key, cand_n, cand_s), cand_n)
        ok = eval_gate(cval, op, target)
        status = "N/A" if ok is None else ("PASS" if ok else "FAIL")
        statuses[gate].append(status)
        delta_str, trend = eval_delta(bval, cval, direction, tol_pct)
        rows.append((gate, f"| {gate} | {label} | {fmt_val(bval, unit)} | {fmt_val(cval, unit)} | {delta_str} "
                           f"({trend or 'n/a'}) | {fmt_gate(op, target, unit)} | **{status}** |"))
    for gate, source in V6_SUITE_GATES.items():
        statuses[gate].append("N/A")
        rows.append((gate, f"| {gate} | suite gate: `{source}` | — | — | — | — | N/A |"))
    rows_md = [row for _, row in sorted(rows, key=lambda r: V6_ORDER.index(r[0]))]

    verdicts = {gate: gate_verdict(statuses[gate]) for gate in V6_ORDER}
    failed = [g for g in V6_ORDER if verdicts[g] == "FAIL"]
    passed = sum(v == "PASS" for v in verdicts.values())
    unmeasured = sum(v == "N/A" for v in verdicts.values())
    per_category = [
        f"**{V6_CATEGORIES[c]}:** " + " ".join(f"{g} {verdicts[g]}" for g in V6_ORDER if g[0] == c)
        for c in V6_CATEGORIES
    ]
    lines = [
        f"# `pg_sift` Spec 28 Strict v6.0 Gate Scorecard: `{base_name}` vs `{cand_name}`",
        "",
        f"- **Baseline:** `{base_name}` (`{base_path}`)",
        f"- **Candidate:** `{cand_name}` (`{cand_path}`)",
        f"- **Result:** **{'FAIL' if failed else 'PASS'}** | **{passed} PASS**, **{len(failed)} FAIL**, "
        f"{unmeasured} N/A of {len(V6_ORDER)} gates",
        f"- **Failed gates:** {', '.join(failed) if failed else 'none'}",
    ] + [f"- {line}" for line in per_category] + [
        "",
        f"| Gate | Check | `{base_name}` | `{cand_name}` | Delta | Hard Gate | Verdict |",
        "|---|---|---:|---:|---:|---:|---|",
    ] + rows_md + [""]
    return "\n".join(lines), failed


def main():
    ap = argparse.ArgumentParser(description="Compare two pg_sift benchmark TSV files.")
    ap.add_argument("baseline_tsv", help="Path to baseline round TSV (e.g. bench/baselines/v510.tsv)")
    ap.add_argument("candidate_tsv", help="Path to candidate round TSV (e.g. docs/results/v6-core.tsv)")
    ap.add_argument("--tol-pct", type=float, default=5.0, help="Noise band %% for SAME vs IMPROVED/REGRESSED (default: 5.0)")
    ap.add_argument("-o", "--output", default=None, help="Optional Markdown output file path")
    ap.add_argument("--fail-on-gate", action="store_true", help="exit 1 when a target gate of the candidate fails")
    ap.add_argument("--strict-v6-gates", action="store_true",
                    help="print the Spec 28 hard-gate PASS/FAIL scorecard (M/P/F/S/U) and exit 1 on any FAIL")
    args = ap.parse_args()

    if args.strict_v6_gates:
        md, failed = strict_v6_scorecard(args.baseline_tsv, args.candidate_tsv, tol_pct=args.tol_pct)
        if args.output:
            with open(args.output, "w", encoding="utf-8") as f:
                f.write(md)
        else:
            sys.stdout.write(md)
        sys.exit(1 if failed else 0)

    md, counts = compare_rounds(args.baseline_tsv, args.candidate_tsv, tol_pct=args.tol_pct)
    if args.output:
        with open(args.output, "w", encoding="utf-8") as f:
            f.write(md)
    else:
        sys.stdout.write(md)
    if args.fail_on_gate and counts["FAIL"]:
        sys.exit(1)


if __name__ == "__main__":
    main()
