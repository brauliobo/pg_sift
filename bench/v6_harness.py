#!/usr/bin/env python3
"""
pg_sift 6.0 focused scaling benchmark and telemetry harness (bench/bench v6-core).

Measures scaling growth factors across a 3-point series (default 10k -> 40k -> 160k rows,
4x jumps; --scales / --release-scale change the ladder) under tight memory budgets
(maintenance_work_mem=4MB, work_mem=4MB), plus OLTP mutations, 2% dirty-state resilience,
analytical queries Q1-Q8 with 1-vs-4 worker parallel scaling, idle pooled backend memory,
index size, page utilization, WAL per row and compaction write amplification.

Runs against a running cluster that has pg_sift preloaded (bench/run_round.sh builds a
throwaway one). Writes <out-dir>/<round>.tsv and .md; with --docs also docs/results/v6-core.tsv,
the file scripts/gen-docs.py reads (bench/compare_rounds.py diffs two such files).

Uses strictly neutral public-repo naming (events, entity_id, parent_id,
entity_parents, metrics, notes, analysis) and zero external Python dependencies.
"""

import argparse
import json
import os
import platform
import statistics
import subprocess
import sys
import threading
import time
from datetime import datetime, timezone


DEFAULT_SCALES = [10000, 40000, 160000]
RELEASE_SCALES = [10000, 100000, 1000000]
SCALES = list(DEFAULT_SCALES)
REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAINT_MEM = "4MB"
WORK_MEM = "4MB"
PAR_WORK_MEM = "16MB"
BACKEND_CACHE_CAP = "2MB"
POOL_EXTRA_INDEXES = 32
UUID_BYTES = 16
VACUUM_RSS_GUARD_MB = 6144.0  # 6 GB safety cap against v5.1.0 VACUUM OOM spikes
VACUUM_TIMEOUT_SEC = 90.0

INDEX_COLS = (
    "id, entity_id, event_date, deleted_at, updated_at, group_key, distinct_tag, status, "
    "amount, score, qty, event_uuid, metrics, notes, analysis"
)
INDEX_PATHS = (
    "metrics.channel text, metrics.latency float8, "
    "notes.tag text, notes.priority int4, "
    "analysis.flag text, "
    "analysis.items[]{kind text, weight float8}"
)
INDEX_KEY_ORDER = "entity_id, event_date"
INDEX_SCOPE = "parent_id: entity_id -> entity_parents(entity_id, parent_id)"
MEGA_ENTITY_ID = 1
MEDIAN_ENTITY_ID = 1500
MEGA_PARENT_ID = 1
MID_PARENT_ID = 42


def klabel(n):
    """10000 -> 10k, 1000000 -> 1M"""
    return f"{n // 1000000}M" if n >= 1000000 and n % 1000000 == 0 else f"{n // 1000}k"


def vacuum_timeout():
    """the VACUUM guard grows with the ladder: 90 s up to 160k rows"""
    return VACUUM_TIMEOUT_SEC * max(1.0, SCALES[-1] / DEFAULT_SCALES[-1])


def cpu_model():
    try:
        with open("/proc/cpuinfo", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("model name"):
                    return line.split(":", 1)[1].strip()
    except OSError:
        pass
    return platform.processor() or platform.machine()


def read_proc_mem_kb(pid):
    """Return (vm_hwm_kb, rss_anon_kb, vm_rss_kb) from /proc/<pid>/status."""
    hwm = anon = rss = 0.0
    try:
        with open(f"/proc/{pid}/status", "r", encoding="utf-8") as f:
            for line in f:
                if line.startswith("VmHWM:"):
                    hwm = float(line.split()[1])
                elif line.startswith("RssAnon:"):
                    anon = float(line.split()[1])
                elif line.startswith("VmRSS:"):
                    rss = float(line.split()[1])
    except OSError:
        pass
    return hwm, anon, rss


def ols_slope(xs, ys):
    """Compute ordinary least-squares slope dy/dx."""
    n = len(xs)
    if n < 2:
        return 0.0
    mx = sum(xs) / n
    my = sum(ys) / n
    num = sum((x - mx) * (y - my) for x, y in zip(xs, ys))
    den = sum((x - mx) ** 2 for x in xs)
    return num / den if den > 0 else 0.0


class PgClient:
    def __init__(self, host, port, db, user):
        self.base_cmd = [
            "psql", "-X", "-q", "-v", "ON_ERROR_STOP=1",
            "-h", host, "-p", str(port), "-U", user, "-d", db,
        ]

    def _run_psql(self, extra_args, sql=None):
        cmd = self.base_cmd + extra_args
        r = subprocess.run(
            cmd,
            input=sql,
            capture_output=True, text=True,
        )
        if r.returncode != 0:
            snippet = (sql or " ".join(extra_args))[:1000]
            raise RuntimeError(
                f"psql failed (rc={r.returncode}):\nSTDERR:\n{r.stderr.strip()}\nSQL:\n{snippet}"
            )
        return r

    def query_scalar(self, sql):
        r = self._run_psql(["-At", "-c", sql])
        return r.stdout.strip()

    def query_rows(self, sql):
        r = self._run_psql(["-At", "-F", "\t", "-c", sql])
        lines = [l for l in r.stdout.splitlines() if l.strip() != ""]
        return [l.split("\t") for l in lines]

    def exec_sql(self, sql):
        # Feed via stdin (not -c) so multi-statement scripts including VACUUM
        # run each statement in its own transaction rather than an implicit block.
        self._run_psql([], sql=sql + "\n")

    def get_wal_lsn(self):
        # Use insert LSN so small mutations are captured even with synchronous_commit = off
        return self.query_scalar("SELECT pg_current_wal_insert_lsn()::text")

    def wal_diff_bytes(self, lsn0, lsn1):
        return int(self.query_scalar(f"SELECT pg_wal_lsn_diff('{lsn1}', '{lsn0}')::bigint"))

    def get_temp_bytes(self):
        # Force stats flush via pg_stat_clear_snapshot + read
        return int(self.query_scalar(
            "SELECT pg_stat_clear_snapshot(); "
            "SELECT coalesce(temp_bytes, 0)::bigint FROM pg_stat_database WHERE datname = current_database()"
        ).splitlines()[-1])

    def get_rel_size(self, relname):
        b = int(self.query_scalar(f"SELECT pg_relation_size('{relname}')::bigint"))
        return b, b // 8192

    def get_table_size(self, relname):
        """Return heap + TOAST + FSM/VM bytes (excluding indexes)."""
        return int(self.query_scalar(f"SELECT pg_table_size('{relname}')::bigint"))

    def get_total_rel_size(self, relname):
        """Return total relation bytes (heap + TOAST + all indexes)."""
        return int(self.query_scalar(f"SELECT pg_total_relation_size('{relname}')::bigint"))

    def get_idx_blks_io(self, idxname):
        row = self.query_scalar(
            f"SELECT coalesce(idx_blks_read, 0) + coalesce(idx_blks_hit, 0) "
            f"FROM pg_statio_user_indexes WHERE indexrelname = '{idxname}'"
        )
        return int(row) if row else 0

    def run_measured(
        self,
        sql_script,
        watch_cic=False,
        rss_guard_mb=None,
        timeout_sec=300.0,
    ):
        """
        Execute sql_script in a fresh backend, returning:
          elapsed_ms, peak_hwm_mb, peak_anon_mb, wal_bytes, temp_bytes,
          cic_pass1_ms, cic_pass2_ms, status, stdout, stderr
        """
        lsn0 = self.get_wal_lsn()
        temp0 = self.get_temp_bytes()

        wrapped = "SELECT pg_backend_pid();\n" + sql_script + "\n"
        proc = subprocess.Popen(
            self.base_cmd + ["-At"],
            stdin=subprocess.PIPE,
            stdout=subprocess.PIPE,
            stderr=subprocess.PIPE,
            text=True,
        )
        proc.stdin.write(wrapped)
        proc.stdin.close()

        # First line of stdout is the backend PID
        pid_line = proc.stdout.readline().strip()
        pid = int(pid_line) if pid_line.isdigit() else None

        t0 = time.monotonic()
        peak_hwm_kb = 0.0
        peak_anon_kb = 0.0
        cic_pass2_start = None
        status = "OK"
        stop_flag = threading.Event()
        rest_out = []
        rest_err = []

        def drain_out():
            rest_out.append(proc.stdout.read())

        def drain_err():
            rest_err.append(proc.stderr.read())

        to = threading.Thread(target=drain_out, daemon=True)
        te = threading.Thread(target=drain_err, daemon=True)
        to.start()
        te.start()

        # Poll /proc/<pid>/status (and optionally pg_stat_progress_create_index)
        while proc.poll() is None:
            now = time.monotonic()
            if pid is not None:
                hwm, anon, _ = read_proc_mem_kb(pid)
                if hwm > peak_hwm_kb:
                    peak_hwm_kb = hwm
                if anon > peak_anon_kb:
                    peak_anon_kb = anon

                if rss_guard_mb is not None and (anon / 1024.0) > rss_guard_mb:
                    status = f"RSS_GUARD_{int(anon / 1024.0)}MB"
                    try:
                        self.exec_sql(f"SELECT pg_cancel_backend({pid})")
                    except Exception:
                        pass
                    time.sleep(0.2)
                    if proc.poll() is None:
                        try:
                            self.exec_sql(f"SELECT pg_terminate_backend({pid})")
                        except Exception:
                            proc.kill()
                    break

            if (now - t0) > timeout_sec:
                status = f"TIMEOUT_{int(timeout_sec)}s"
                if pid is not None:
                    try:
                        self.exec_sql(f"SELECT pg_cancel_backend({pid})")
                    except Exception:
                        pass
                time.sleep(0.2)
                if proc.poll() is None:
                    proc.kill()
                break

            if watch_cic and cic_pass2_start is None and pid is not None:
                try:
                    phase = self.query_scalar(
                        f"SELECT phase FROM pg_stat_progress_create_index WHERE pid = {pid}"
                    )
                    if phase and ("validation" in phase or "waiting" in phase):
                        cic_pass2_start = now
                except Exception:
                    pass

            time.sleep(0.025)

        t1 = time.monotonic()
        if pid is not None:
            hwm, anon, _ = read_proc_mem_kb(pid)
            peak_hwm_kb = max(peak_hwm_kb, hwm)
            peak_anon_kb = max(peak_anon_kb, anon)

        to.join(timeout=5)
        te.join(timeout=5)
        rc = proc.wait(timeout=5)

        stdout_str = "".join(rest_out)
        stderr_str = "".join(rest_err)
        if status == "OK" and rc != 0:
            status = f"ERR_RC_{rc}"

        elapsed_ms = (t1 - t0) * 1000.0
        lsn1 = self.get_wal_lsn()
        # Give stats collector a brief tick to flush temp_bytes
        time.sleep(0.05)
        temp1 = self.get_temp_bytes()

        wal_bytes = max(0, self.wal_diff_bytes(lsn0, lsn1))
        temp_bytes = max(0, temp1 - temp0)

        cic_pass1_ms = None
        cic_pass2_ms = None
        if watch_cic:
            if cic_pass2_start is not None:
                cic_pass1_ms = (cic_pass2_start - t0) * 1000.0
                cic_pass2_ms = (t1 - cic_pass2_start) * 1000.0
            else:
                cic_pass1_ms = elapsed_ms
                cic_pass2_ms = 0.0

        return {
            "elapsed_ms": elapsed_ms,
            "peak_hwm_mb": peak_hwm_kb / 1024.0,
            "peak_anon_mb": peak_anon_kb / 1024.0,
            "wal_bytes": wal_bytes,
            "temp_bytes": temp_bytes,
            "cic_pass1_ms": cic_pass1_ms,
            "cic_pass2_ms": cic_pass2_ms,
            "status": status,
            "stdout": stdout_str,
            "stderr": stderr_str,
        }

    def explain_query(self, sql, workers=0, runs=3, warmup=1, work_mem=None, analyze=True):
        """
        Run EXPLAIN (ANALYZE, BUFFERS, FORMAT JSON) — or EXPLAIN (SUMMARY, FORMAT JSON)
        when analyze=False — in a fresh backend session so we capture execution time (median),
        planning time, shared hit/read blocks, heap scan blocks, top plan node / sift usage,
        workers planned/launched, and peak backend RSS.
        """
        wm = work_mem or WORK_MEM
        setup_lines = [
            f"SET work_mem = '{wm}';",
            f"SET max_parallel_workers_per_gather = {workers};",
            "SET jit = off;",
        ]
        if workers > 0:
            setup_lines += [
                "SET parallel_setup_cost = 0;",
                "SET parallel_tuple_cost = 0;",
                "SET min_parallel_table_scan_size = 0;",
                "SET min_parallel_index_scan_size = 0;",
            ]
        setup = "\n".join(setup_lines) + "\n"
        explain_opts = "ANALYZE, BUFFERS, FORMAT JSON" if analyze else "SUMMARY, BUFFERS, FORMAT JSON"

        # Build one session script: warmup + runs
        stmts = [setup]
        for _ in range(warmup):
            stmts.append(f"EXPLAIN ({explain_opts}) {sql};")
        for _ in range(runs):
            stmts.append("SELECT '---PLAN---';")
            stmts.append(f"EXPLAIN ({explain_opts}) {sql};")

        res = self.run_measured("\n".join(stmts), timeout_sec=120.0)
        if res["status"] != "OK":
            raise RuntimeError(f"Query failed ({res['status']}): {res['stderr']}\nSQL: {sql}")

        parts = res["stdout"].split("---PLAN---")[1:]
        exec_times = []
        plan_times = []
        plan_blks_list = []
        shared_hits = []
        shared_reads = []
        heap_scan_blks_list = []
        heap_reread_list = []
        nodes_seen = set()
        workers_launched = 0
        workers_planned = 0

        def walk_plan(node, heap_acc, reread_acc):
            nonlocal workers_launched, workers_planned
            nt = node.get("Node Type", "")
            custom = node.get("Custom Plan Provider", "")
            if custom:
                nodes_seen.add(custom)
            elif nt:
                nodes_seen.add(nt)
            wl = node.get("Workers Launched", 0)
            if wl > workers_launched:
                workers_launched = wl
            wp = node.get("Workers Planned", 0)
            if wp > workers_planned:
                workers_planned = wp
            if nt in ("Seq Scan", "Bitmap Heap Scan", "Tid Scan", "Tid Range Scan"):
                heap_acc[0] += int(node.get("Shared Hit Blocks", 0)) + int(node.get("Shared Read Blocks", 0))
            if "Heap Pages" in node:
                heap_acc[0] += int(node.get("Heap Pages", 0))
            if "Rows Read Again From The Heap" in node:
                reread_acc[0] += int(node.get("Rows Read Again From The Heap", 0))
            if "Heap Fetches" in node:
                reread_acc[0] += int(node.get("Heap Fetches", 0))
            for child in node.get("Plans", []):
                walk_plan(child, heap_acc, reread_acc)

        for p in parts:
            p = p.strip()
            if not p:
                continue
            data = json.loads(p)[0]
            if analyze:
                exec_times.append(float(data.get("Execution Time", 0.0)))
            plan_times.append(float(data.get("Planning Time", 0.0)))
            planning_info = data.get("Planning") or {}
            plan_blks_list.append(
                int(planning_info.get("Shared Hit Blocks", 0))
                + int(planning_info.get("Shared Read Blocks", 0))
            )
            root = data["Plan"]
            shared_hits.append(int(root.get("Shared Hit Blocks", 0)))
            shared_reads.append(int(root.get("Shared Read Blocks", 0)))
            heap_acc = [0]
            reread_acc = [0]
            walk_plan(root, heap_acc, reread_acc)
            heap_scan_blks_list.append(heap_acc[0])
            heap_reread_list.append(reread_acc[0])

        sift_nodes = sorted([n for n in nodes_seen if "Sift" in n or "sift" in n])
        plan_tag = ",".join(sift_nodes) if sift_nodes else "PG_NATIVE"

        return {
            "exec_ms": statistics.median(exec_times) if exec_times else 0.0,
            "plan_ms": statistics.median(plan_times) if plan_times else 0.0,
            "plan_blks": int(statistics.median(plan_blks_list)) if plan_blks_list else 0,
            "shared_hit": int(statistics.median(shared_hits)) if shared_hits else 0,
            "shared_read": int(statistics.median(shared_reads)) if shared_reads else 0,
            "total_blks": int(statistics.median([h + r for h, r in zip(shared_hits, shared_reads)])) if shared_hits else 0,
            "heap_scan_blks": int(statistics.median(heap_scan_blks_list)) if heap_scan_blks_list else 0,
            "heap_reread_rows": int(statistics.median(heap_reread_list)) if heap_reread_list else 0,
            "peak_hwm_mb": res["peak_hwm_mb"],
            "peak_anon_mb": res["peak_anon_mb"],
            "workers_planned": workers_planned,
            "workers_launched": workers_launched,
            "plan_tag": plan_tag,
        }


def setup_base_schema(pg):
    pg.exec_sql("""
        CREATE EXTENSION IF NOT EXISTS pg_sift;

        DROP TABLE IF EXISTS events CASCADE;
        DROP TABLE IF EXISTS entity_parents CASCADE;

        CREATE TABLE entity_parents (
            entity_id int NOT NULL,
            parent_id int NOT NULL,
            PRIMARY KEY (entity_id, parent_id)
        ) WITH (autovacuum_enabled = false);

        -- 2,000 entities mapped to 100 parents with realistic Zipf power-law skew:
        --   - Parents 1..94 follow Zipf weight p^(-0.85) (parent 1 ~13.4%, top 10 ~45.7% of entities)
        --   - Parents 95..100 are tiny 1-entity parents (6 single-entity parents)
        --   - ~97% of entities have 1 parent; ~3% have 2-6 parents (avg multiplicity ~1.05x)
        --     spread across parents 1..80 for realistic parent overlap.
        WITH parent_weights AS (
            SELECT p, (1.0 / power(p::float8, 0.85)) AS w
            FROM generate_series(1, 94) p
        ),
        parent_cdf AS (
            SELECT
                p,
                CASE WHEN p = 94 THEN 1.01
                     ELSE sum(w) OVER (ORDER BY p) / sum(w) OVER ()
                END AS hi,
                coalesce(sum(w) OVER (ORDER BY p ROWS BETWEEN UNBOUNDED PRECEDING AND 1 PRECEDING), 0.0)
                    / sum(w) OVER () AS lo
            FROM parent_weights
        ),
        entity_primary AS (
            SELECT 1 AS entity_id, 1 AS parent_id
            UNION ALL
            SELECT e.entity_id, pc.p AS parent_id
            FROM (
                SELECT
                    g AS entity_id,
                    ((((g * 7919 + 104729) % 1000000) + 0.5) / 1000000.0) AS u
                FROM generate_series(2, 1994) g
            ) e
            JOIN parent_cdf pc ON e.u >= pc.lo AND e.u < pc.hi
            UNION ALL
            SELECT 1994 + k AS entity_id, 94 + k AS parent_id
            FROM generate_series(1, 6) k
        ),
        entity_extra AS (
            SELECT
                e,
                1 + ((e * 37 + j * 19) % 80) AS parent_id
            FROM generate_series(2, 1994) e,
                 generate_series(1, 5) j
            WHERE (e % 50 = 0 AND j = 1)
               OR (e % 166 = 17 AND j <= 2)
               OR (e IN (11, 211, 411) AND j <= 3)
               OR (e IN (611, 811, 1011) AND j <= 4)
               OR (e IN (1211, 1411) AND j <= 5)
        )
        INSERT INTO entity_parents (entity_id, parent_id)
        SELECT entity_id, parent_id FROM entity_primary
        UNION
        SELECT e, parent_id FROM entity_extra
        ON CONFLICT DO NOTHING;

        CREATE INDEX entity_parents_journal
            ON entity_parents USING sift_col (entity_id, parent_id)
            WITH (track_changes = 'ctid');

        VACUUM (ANALYZE, FREEZE) entity_parents;
    """)
    row = pg.query_rows("""
        WITH ec AS (
            SELECT entity_id, count(*) AS np FROM entity_parents GROUP BY entity_id
        ),
        pc AS (
            SELECT parent_id, count(*) AS ne FROM entity_parents GROUP BY parent_id
        ),
        ov AS (
            SELECT count(DISTINCT ep1.parent_id) AS n_ov
            FROM entity_parents ep1
            JOIN entity_parents ep2
              ON ep2.entity_id = ep1.entity_id AND ep2.parent_id <> ep1.parent_id
        )
        SELECT
            round((SELECT count(*)::numeric / count(DISTINCT entity_id) FROM entity_parents), 3),
            (SELECT ne FROM pc WHERE parent_id = 1),
            round(100.0 * (SELECT sum(ne) FROM pc WHERE parent_id <= 10) / (SELECT sum(ne) FROM pc), 1),
            round(100.0 * (SELECT count(*) FROM ec WHERE np = 1) / (SELECT count(*) FROM ec), 1),
            (SELECT max(np) FROM ec),
            round(100.0 * (SELECT n_ov FROM ov) / (SELECT count(*) FROM pc), 1)
    """)[0]
    return {
        "mult": float(row[0]),
        "p1_entities": int(row[1]),
        "top10_pct": float(row[2]),
        "single_parent_pct": float(row[3]),
        "max_mult": int(row[4]),
        "overlap_parents_pct": float(row[5]),
    }


def events_table_ddl(table_name):
    return f"""
        DROP TABLE IF EXISTS {table_name} CASCADE;
        CREATE TABLE {table_name} (
            id           bigint PRIMARY KEY,
            entity_id    int NOT NULL,
            event_date   date NOT NULL,
            deleted_at   timestamptz,
            updated_at   timestamptz NOT NULL,
            group_key    int NOT NULL,
            distinct_tag text NOT NULL,
            status       smallint NOT NULL,
            amount       numeric(12,2) NOT NULL,
            score        float8 NOT NULL,
            qty          int NOT NULL,
            event_uuid   uuid NOT NULL,
            metrics      jsonb NOT NULL,
            notes        jsonb NOT NULL,
            analysis     jsonb NOT NULL
        ) WITH (autovacuum_enabled = false);
    """


def events_insert_sql(table_name, id_lo, id_hi, date_offset=0):
    # Zipf entity skew: 5% rows on mega-entity 1, 95% power-law across entities 2..2000.
    # Soft-deletes: ~0.1% global (83% on historical event_date < 2025-01-01).
    # Sparse analysis JSONB: 25% populated with 'flag' + 2 items, 75% empty '{}'.
    return f"""
        INSERT INTO {table_name}
        SELECT
            i,
            CASE WHEN (i % 20) = 0 THEN {MEGA_ENTITY_ID}
                 ELSE LEAST(2000, 2 + floor(1999.0 * power((((i * 7919 + 104729) % 1000000) + 0.5) / 1000000.0, 2.2))::int)
            END,
            DATE '2024-01-01' + (((i + {date_offset}) % 730)::int),
            CASE WHEN (((i + {date_offset}) % 730) < 365 AND ((i * 17) % 600) = 7)
                   OR (((i + {date_offset}) % 730) >= 365 AND ((i * 17) % 3000) = 7)
                 THEN TIMESTAMPTZ '2025-11-01 00:00:00+00' + ((i % 45) * INTERVAL '1 day')
                 ELSE NULL
            END,
            TIMESTAMPTZ '2025-01-01 00:00:00+00' + (i * INTERVAL '180 seconds') + (((i * 97) % 1800) * INTERVAL '1 second'),
            1 + ((i * 131) % 50000),
            'tag-' || ((i * 97) % 50000),
            (i % 100)::smallint,
            (((i * 104729) % 100000) / 100.0)::numeric(12,2),
            (((i * 31) % 1000) / 10.0)::float8,
            1 + (i % 50),
            md5(i::text)::uuid,
            jsonb_build_object(
                'channel', (ARRAY['web','app','api','partner'])[1 + (i % 4)],
                'latency', ((i % 500) / 10.0),
                'region',  (ARRAY['na','eu','sa','ap'])[1 + ((i / 4) % 4)]
            ),
            jsonb_build_object(
                'tag',      'n-' || (i % 500),
                'priority', (i % 5),
                'summary',  repeat('x', 64) || '-' || (i % 1000)
            ),
            CASE WHEN ((i + (i / 20)) % 4) = 0 THEN
                jsonb_build_object(
                    'flag', 'ok',
                    'items', jsonb_build_array(
                        jsonb_build_object('kind', 'k' || (i % 12), 'weight', ((i % 100) / 10.0)),
                        jsonb_build_object('kind', 'k' || ((i + 5) % 12), 'weight', (((i * 3) % 100) / 10.0))
                    )
                )
            ELSE '{{}}'::jsonb
            END
        FROM generate_series({id_lo}::bigint, {id_hi}::bigint) i;
    """


def load_events_table(pg, nrows):
    pg.exec_sql(
        events_table_ddl("events")
        + events_insert_sql("events", 1, nrows)
        + "\nVACUUM (ANALYZE, FREEZE) events;\n"
    )


QUERIES = {
    "Q1_scope_whole": """
        SELECT count(*), sum(e.amount), avg(e.score)
        FROM events e
        JOIN entity_parents ep ON ep.entity_id = e.entity_id
        WHERE ep.parent_id = 42
    """,
    "Q2_scope_month": """
        SELECT count(*), sum(e.amount), avg(e.score)
        FROM events e
        JOIN entity_parents ep ON ep.entity_id = e.entity_id
        WHERE ep.parent_id = 42
          AND e.event_date >= DATE '2024-06-01'
          AND e.event_date <  DATE '2024-07-01'
    """,
    "Q3_scope_groupby_parent": """
        SELECT ep.parent_id, count(*), sum(e.amount), avg(e.score)
        FROM events e
        JOIN entity_parents ep ON ep.entity_id = e.entity_id
        GROUP BY ep.parent_id
    """,
    "Q4_highcard_groupby": """
        SELECT group_key, count(*), sum(amount), avg(amount)
        FROM events
        GROUP BY group_key
    """,
    "Q5_count_distinct": """
        SELECT count(DISTINCT distinct_tag)
        FROM events
    """,
    "Q6_jsonb_shred_array": """
        SELECT e.metrics->>'channel' AS channel,
               elem->>'kind' AS item_kind,
               count(*),
               sum((elem->>'weight')::float8),
               avg((e.metrics->>'latency')::float8)
        FROM events e,
             LATERAL jsonb_array_elements(e.analysis->'items') AS elem
        GROUP BY 1, 2
    """,
    "Q7_scope_top20_unfilt": """
        SELECT e.id, e.entity_id, e.event_date, e.amount
        FROM events e
        JOIN entity_parents ep ON ep.entity_id = e.entity_id
        WHERE ep.parent_id = 42
        ORDER BY e.event_date DESC
        LIMIT 20
    """,
    "Q8_scope_top20_1pct": """
        SELECT e.id, e.entity_id, e.event_date, e.amount
        FROM events e
        JOIN entity_parents ep ON ep.entity_id = e.entity_id
        WHERE ep.parent_id = 42 AND e.status = 0
        ORDER BY e.event_date DESC
        LIMIT 20
    """,
}

SCALING_QUERY_KEYS = [
    "Q1_scope_whole",
    "Q2_scope_month",
    "Q3_scope_groupby_parent",
    "Q7_scope_top20_unfilt",
    "Q8_scope_top20_1pct",
]

CTE6_PROBE_SQL = """
    WITH
      c1 AS (
        SELECT entity_id, count(*) AS cnt, sum(amount) AS amt
        FROM events WHERE status < 50 GROUP BY entity_id
      ),
      c2 AS (
        SELECT entity_id, avg(score) AS avg_s
        FROM events WHERE event_date >= DATE '2024-06-01' GROUP BY entity_id
      ),
      c3 AS (
        SELECT c1.entity_id, c1.cnt, c1.amt, c2.avg_s
        FROM c1 JOIN c2 ON c1.entity_id = c2.entity_id
      ),
      c4 AS (
        SELECT ep.parent_id, count(*) AS hi_cnt
        FROM events e JOIN entity_parents ep ON ep.entity_id = e.entity_id
        WHERE e.score > 50 GROUP BY ep.parent_id
      ),
      c5 AS (
        SELECT ep.parent_id, sum(c3.amt) AS p_amt, avg(c3.avg_s) AS p_avg
        FROM c3 JOIN entity_parents ep ON ep.entity_id = c3.entity_id
        GROUP BY ep.parent_id
      ),
      c6 AS (
        SELECT e.metrics->>'channel' AS ch, count(*) AS ch_cnt,
               avg((e.metrics->>'latency')::float8) AS avg_lat
        FROM events e WHERE jsonb_typeof(e.metrics->'latency') = 'number'
        GROUP BY 1
      )
    SELECT
      (SELECT count(*) FROM c5 JOIN c4 ON c4.parent_id = c5.parent_id WHERE c5.p_amt > 1000),
      (SELECT max(avg_lat) FROM c6)
"""

VARIANT_EXC_SQL = """
    SELECT metrics->>'channel' AS channel,
           count(*),
           avg((metrics->>'latency')::float8)
    FROM events
    WHERE jsonb_typeof(metrics->'latency') = 'number'
    GROUP BY 1
"""

F4_SOFT_DEL_NULL_SQL = f"""
    SELECT count(*), sum(e.amount), avg(e.score)
    FROM events e
    JOIN entity_parents ep ON ep.entity_id = e.entity_id
    WHERE ep.parent_id = {MID_PARENT_ID}
      AND e.deleted_at IS NULL
"""

F4_SOFT_DEL_NOT_NULL_SQL = f"""
    SELECT count(*), avg(e.score)
    FROM events e
    JOIN entity_parents ep ON ep.entity_id = e.entity_id
    WHERE ep.parent_id = {MEGA_PARENT_ID}
      AND e.deleted_at IS NOT NULL
"""

F5_SINGLE_ENTITY_SQL = f"""
    SELECT count(*), sum(amount), avg(score)
    FROM events
    WHERE entity_id = {MEDIAN_ENTITY_ID}
      AND deleted_at IS NULL
"""

F5_MEGA_ENTITY_SQL = f"""
    SELECT count(*), min(event_date), max(event_date), avg(score)
    FROM events
    WHERE entity_id = {MEGA_ENTITY_ID}
      AND deleted_at IS NULL
"""

F6_MATERIALIZED_CTE_SQL = f"""
    WITH ids AS MATERIALIZED (
        SELECT entity_id FROM entity_parents WHERE parent_id = {MEGA_PARENT_ID}
    )
    SELECT count(*), sum(e.amount), avg(e.score)
    FROM events e
    JOIN ids USING (entity_id)
    WHERE e.deleted_at IS NULL
"""

F7_JSONB_EXISTS_SQL = f"""
    SELECT count(*)
    FROM events e
    JOIN entity_parents ep ON ep.entity_id = e.entity_id
    WHERE ep.parent_id = {MEGA_PARENT_ID}
      AND e.deleted_at IS NULL
      AND e.analysis ? 'flag'
"""

F8_UPDATED_AT_TOP20_SQL = f"""
    SELECT e.id, e.entity_id, e.updated_at, e.amount
    FROM events e
    JOIN entity_parents ep ON ep.entity_id = e.entity_id
    WHERE ep.parent_id = {MEGA_PARENT_ID}
      AND e.deleted_at IS NULL
    ORDER BY e.updated_at DESC
    LIMIT 20
"""


F_PROBE_SQL = {
    "F4_soft_del_null": F4_SOFT_DEL_NULL_SQL,
    "F4_soft_del_not_null": F4_SOFT_DEL_NOT_NULL_SQL,
    "F5_single_entity": F5_SINGLE_ENTITY_SQL,
    "F5_mega_entity": F5_MEGA_ENTITY_SQL,
    "F6_materialized_cte": F6_MATERIALIZED_CTE_SQL,
    "F7_jsonb_exists": F7_JSONB_EXISTS_SQL,
    "F8_updated_at_top20": F8_UPDATED_AT_TOP20_SQL,
}
TOPN_ORDER_KEY = {"Q7_scope_top20_unfilt": "event_date", "Q8_scope_top20_1pct": "event_date", "F8_updated_at_top20": "updated_at"}


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


def main():
    ap = argparse.ArgumentParser(description=__doc__.split("\n")[1])
    ap.add_argument("--host", required=True, help="socket directory or host of the cluster")
    ap.add_argument("--port", type=int, required=True)
    ap.add_argument("--db", required=True)
    ap.add_argument("--user", required=True)
    ap.add_argument("--round", default="v6-core", help="name of the round: <out-dir>/<round>.tsv and .md")
    ap.add_argument("--git-sha", default="unknown")
    ap.add_argument("--git-branch", default="unknown")
    ap.add_argument("--build-sec", type=float, default=0.0)
    ap.add_argument("--make-jobs", type=int, default=4, help="parallel jobs of the build, for the record")
    ap.add_argument("--out-dir", default=".", help="where <round>.tsv and <round>.md are written")
    ap.add_argument("--scales", help="comma separated row counts of the ladder (default 10000,40000,160000)")
    ap.add_argument("--release-scale", nargs="?", const=",".join(map(str, RELEASE_SCALES)),
                    help="the sign-off ladder: 10000,100000,1000000 or the comma separated row counts given")
    ap.add_argument("--docs", action="store_true", help="also write docs/results/v6-core.tsv (read by scripts/gen-docs.py)")
    ap.add_argument("--verify-diff", action="store_true", help="check the checksum of every Q1-Q8 and F4-F8 answer of the sift plan against the heap scan; a difference fails the run")
    args = ap.parse_args()
    ladder = args.release_scale or args.scales
    if ladder:
        SCALES[:] = sorted({int(x) for x in ladder.split(",")})
        if len(SCALES) < 3:
            ap.error("the ladder needs at least 3 row counts (an OLS slope over 2 points is a line through them)")

    pg = PgClient(args.host, args.port, args.db, args.user)
    t_suite_start = time.monotonic()

    ext_ver = pg.query_scalar("CREATE EXTENSION IF NOT EXISTS pg_sift; SELECT extversion FROM pg_extension WHERE extname = 'pg_sift';")
    pg_ver = pg.query_scalar("SHOW server_version;")
    scope_info = setup_base_schema(pg)
    mult = scope_info["mult"]

    tsv_rows = []

    def rec(section, metric, scale, value, unit, notes=""):
        tsv_rows.append((section, metric, str(scale), f"{value}", unit, notes))

    rec("meta", "round", "-", args.round, "str", f"branch={args.git_branch} sha={args.git_sha}")
    rec("meta", "pg_sift_version", "-", ext_ver, "str", f"pg={pg_ver}")
    rec("meta", "build_time", "-", f"{args.build_sec:.2f}", "s", f"make -j{args.make_jobs}")
    rec(
        "meta",
        "scope_multiplicity",
        "-",
        f"{mult:.3f}",
        "x",
        f"2000 entities -> 100 parents; Zipf p1={scope_info['p1_entities']} "
        f"top10={scope_info['top10_pct']:.1f}% 1-parent={scope_info['single_parent_pct']:.1f}% "
        f"max={scope_info['max_mult']} overlap={scope_info['overlap_parents_pct']:.1f}%",
    )
    rec("meta", "budgets", "-", f"maint={MAINT_MEM},work={WORK_MEM}", "str", "tight scaling budgets")
    rec("meta", "scales", "-", ",".join(str(n) for n in SCALES), "str", "ladder of the slopes")

    scale_data = {}

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

        scale_data[nrows] = sd

    # --- Compute Explicit Scaling Slopes & Growth Ratios (10k -> 40k -> 160k) ---
    xs = [float(n) for n in SCALES]
    SLOPE_KEY = f"{klabel(SCALES[0])}-{klabel(SCALES[-1])}"
    slopes = {
        "cic_rss_anon_b_per_row": ols_slope(xs, [scale_data[n]["cic"]["peak_anon_mb"] * 1024 * 1024 for n in SCALES]),
        "cic_vm_hwm_b_per_row": ols_slope(xs, [scale_data[n]["cic"]["peak_hwm_mb"] * 1024 * 1024 for n in SCALES]),
        "cic_temp_b_per_row": ols_slope(xs, [scale_data[n]["cic"]["temp_bytes"] for n in SCALES]),
        "cic_disk_b_per_row": ols_slope(xs, [scale_data[n]["cic"]["size_bytes"] for n in SCALES]),
        "cic_wal_b_per_row": ols_slope(xs, [scale_data[n]["cic"]["wal_bytes"] for n in SCALES]),
        "vac1_rss_anon_b_per_row": ols_slope(xs, [scale_data[n]["vac1"]["peak_anon_mb"] * 1024 * 1024 for n in SCALES]),
        "vac1_vm_hwm_b_per_row": ols_slope(xs, [scale_data[n]["vac1"]["peak_hwm_mb"] * 1024 * 1024 for n in SCALES]),
    }
    for k, v in slopes.items():
        rec("S_slopes", k, SLOPE_KEY, f"{v:.2f}", "B/row", "OLS across " + ", ".join(klabel(n) for n in SCALES))

    # --- Workload D (Full Q1-Q8 + 1v4 Parallel Scaling on N=160k) ---
    N_MAX = SCALES[-1]
    print(f"[v6-core] === Workload D (Analytical Q1-Q8 & 1v4 Parallel at N={N_MAX}) ===", flush=True)
    full_q = {}
    for qk, qsql in QUERIES.items():
        if qk in scale_data[N_MAX]["queries"]:
            full_q[qk] = scale_data[N_MAX]["queries"][qk]
        else:
            full_q[qk] = pg.explain_query(qsql, workers=0, runs=3, warmup=1)
        q = full_q[qk]
        rec("D_analytical", f"{qk}_1w_ms", N_MAX, f"{q['exec_ms']:.2f}", "ms",
            f"hit={q['shared_hit']} read={q['shared_read']} plan_blks={q['plan_blks']} rss={q['peak_anon_mb']:.1f}MB plan={q['plan_tag']}")

    # 1-vs-4 worker parallel runs for Q4 and Q5 (use PAR_WORK_MEM=16MB so Q4 does not refuse parallel due to hash-spill threshold)
    par_q = {}
    for qk in ("Q4_highcard_groupby", "Q5_count_distinct"):
        q1w_par = pg.explain_query(QUERIES[qk], workers=0, runs=3, warmup=1, work_mem=PAR_WORK_MEM)
        q4w = pg.explain_query(QUERIES[qk], workers=4, runs=3, warmup=1, work_mem=PAR_WORK_MEM)
        speedup = q1w_par["exec_ms"] / q4w["exec_ms"] if q4w["exec_ms"] > 0 else 0.0
        q4w["exec_1w_par_ms"] = q1w_par["exec_ms"]
        q4w["speedup_vs_1w"] = speedup
        par_q[qk] = q4w
        rec("D_parallel", f"{qk}_1w_par_ms", N_MAX, f"{q1w_par['exec_ms']:.2f}", "ms",
            f"work_mem={PAR_WORK_MEM} plan={q1w_par['plan_tag']}")
        rec("D_parallel", f"{qk}_4w_ms", N_MAX, f"{q4w['exec_ms']:.2f}", "ms",
            f"work_mem={PAR_WORK_MEM} workers={q4w['workers_launched']} speedup={speedup:.2f}x plan={q4w['plan_tag']}")
        rec("D_parallel", f"{qk}_workers_launched", N_MAX, str(q4w["workers_launched"]), "workers",
            f"planned={q4w['workers_planned']} requested=4")
        rec("D_parallel", f"{qk}_speedup", N_MAX, f"{speedup:.2f}", "x", f"1w / 4w (work_mem={PAR_WORK_MEM})")

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
    c_ins = pg.run_measured("\n".join(ins_stmts))
    if c_ins["status"] != "OK":
        raise RuntimeError(f"OLTP 1,000 INSERTs failed: {c_ins['stderr']}")
    rec("C_oltp", "insert_1000_total_ms", N_MAX, f"{c_ins['elapsed_ms']:.1f}", "ms", f"{c_ins['elapsed_ms']/1000.0:.3f} ms/row")
    rec("C_oltp", "insert_1000_wal_mb", N_MAX, f"{c_ins['wal_bytes']/(1024*1024):.2f}", "MB", f"rss={c_ins['peak_anon_mb']:.1f}MB")

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
    c_upd = pg.run_measured(upd_sql)
    if c_upd["status"] != "OK":
        raise RuntimeError(f"OLTP 2% UPDATE failed: {c_upd['stderr']}")
    rec("C_oltp", "update_2pct_ms", N_MAX, f"{c_upd['elapsed_ms']:.1f}", "ms",
        f"3200 rows (73% JSONB, 13% soft-del, 14% scalar, 0% entity_id), rss={c_upd['peak_anon_mb']:.1f}MB")
    rec("C_oltp", "update_2pct_wal_mb", N_MAX, f"{c_upd['wal_bytes']/(1024*1024):.2f}", "MB",
        "73% JSONB+updated_at, 13% deleted_at, 14% scalar")

    # Clean baseline before 2% deletes (re-vacuum after insert+update so dirty-state isolates 2% unvacuumed deletes)
    pg.exec_sql(f"SET maintenance_work_mem = '{MAINT_MEM}'; VACUUM events;")
    dirty_probe_keys = ["Q1_scope_whole", "Q2_scope_month", "Q3_scope_groupby_parent", "Q7_scope_top20_unfilt"]
    clean_q = {}
    for qk in dirty_probe_keys:
        clean_q[qk] = pg.explain_query(QUERIES[qk], workers=0, runs=3, warmup=1)

    # C3: Bulk DELETE 2% scattered rows (every 50th row -> 3,200 rows across all entities/dates)
    c_del = pg.run_measured("DELETE FROM events WHERE id % 50 = 0;")
    rec("C_oltp", "delete_2pct_ms", N_MAX, f"{c_del['elapsed_ms']:.1f}", "ms", f"scattered 2%, rss={c_del['peak_anon_mb']:.1f}MB")
    rec("C_oltp", "delete_2pct_wal_mb", N_MAX, f"{c_del['wal_bytes']/(1024*1024):.2f}", "MB", "")

    # Dirty-state query latency AFTER 2% unvacuumed deletes
    dirty_q = {}
    dirty_ratios = {}
    for qk in dirty_probe_keys:
        dirty_q[qk] = pg.explain_query(QUERIES[qk], workers=0, runs=3, warmup=1)
        ratio = dirty_q[qk]["exec_ms"] / clean_q[qk]["exec_ms"] if clean_q[qk]["exec_ms"] > 0 else 0.0
        dirty_ratios[qk] = ratio
        rec("C_dirty", f"{qk}_dirty_ratio", N_MAX, f"{ratio:.2f}", "x",
            f"clean={clean_q[qk]['exec_ms']:.2f}ms dirty={dirty_q[qk]['exec_ms']:.2f}ms")

    # --- Workload B2: Post-Delete VACUUM (on N=160k after 2% scattered deletes) ---
    print(f"[v6-core] === Workload B2 (Post-Delete VACUUM at N={N_MAX}) ===", flush=True)
    io_b2_0 = pg.get_idx_blks_io("idx_events_scoped")
    vac2 = pg.run_measured(
        f"SET maintenance_work_mem = '{MAINT_MEM}'; VACUUM events;",
        rss_guard_mb=VACUUM_RSS_GUARD_MB,
        timeout_sec=vacuum_timeout(),
    )
    time.sleep(0.05)
    io_b2_1 = pg.get_idx_blks_io("idx_events_scoped")
    vac2_bytes, vac2_blks = pg.get_rel_size("idx_events_scoped")
    vac2["blks_touched"] = max(0, io_b2_1 - io_b2_0)
    vac2["size_after_mb"] = vac2_bytes / (1024 * 1024)
    vac2["size_after_blks"] = vac2_blks

    rec("B_vac_post_del", "time_ms", N_MAX, f"{vac2['elapsed_ms']:.1f}", "ms", f"status={vac2['status']}")
    rec("B_vac_post_del", "peak_rss_anon_mb", N_MAX, f"{vac2['peak_anon_mb']:.2f}", "MB", f"VmHWM={vac2['peak_hwm_mb']:.2f} MB")
    rec("B_vac_post_del", "wal_mb", N_MAX, f"{vac2['wal_bytes']/(1024*1024):.2f}", "MB", "")
    rec("B_vac_post_del", "blks_touched", N_MAX, str(vac2["blks_touched"]), "blks", f"size_after={vac2['size_after_mb']:.2f} MB")

    # --- Workload F: Compact v6 Feature Probes (< 3s total, on N=160k) ---
    print(f"[v6-core] === Workload F (Compact Probes at N={N_MAX}) ===", flush=True)

    # F1: 6-CTE planning latency & shared-buffer probe (EXPLAIN SUMMARY, BUFFERS; no execution)
    cte6 = pg.explain_query(CTE6_PROBE_SQL, workers=0, runs=5, warmup=1, analyze=False)
    rec("F_probes", "cte6_plan_ms", N_MAX, f"{cte6['plan_ms']:.2f}", "ms",
        f"plan_blks={cte6['plan_blks']} plan={cte6['plan_tag']}")
    rec("F_probes", "cte6_plan_blks", N_MAX, str(cte6["plan_blks"]), "blks",
        f"plan_ms={cte6['plan_ms']:.2f}ms plan={cte6['plan_tag']}")

    # F2: Pair-edit refresh probe (remove 5 + add 5 pairs in entity_parents for parent_id=42)
    pair_before = pg.explain_query(QUERIES["Q1_scope_whole"], workers=0, runs=3, warmup=1)
    pair_dml = pg.run_measured("""
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
    if pair_dml["status"] != "OK":
        raise RuntimeError(f"Pair-edit DML failed: {pair_dml['stderr']}")
    pair_first = pg.explain_query(QUERIES["Q1_scope_whole"], workers=0, runs=1, warmup=0)
    pair_med = pg.explain_query(QUERIES["Q1_scope_whole"], workers=0, runs=3, warmup=0)
    rec("F_probes", "pair_edit_dml_ms", N_MAX, f"{pair_dml['elapsed_ms']:.2f}", "ms",
        f"-5/+5 pairs, wal={pair_dml['wal_bytes']/1024.0:.1f}KB")
    rec("F_probes", "pair_edit_q1_first_ms", N_MAX, f"{pair_first['exec_ms']:.2f}", "ms",
        f"clean={pair_before['exec_ms']:.2f}ms blks={pair_first['total_blks']} plan={pair_first['plan_tag']}")
    rec("F_probes", "pair_edit_q1_med_ms", N_MAX, f"{pair_med['exec_ms']:.2f}", "ms",
        f"blks={pair_med['total_blks']} plan={pair_med['plan_tag']}")

    # F3: JSONB variant-exception probe (0.5% string values in shredded float8 path metrics.latency)
    var_upd = pg.run_measured(
        "UPDATE events SET metrics = jsonb_set(metrics, '{latency}', '\"err\"'::jsonb) WHERE id % 200 = 0;"
    )
    if var_upd["status"] != "OK":
        raise RuntimeError(f"JSONB variant update failed: {var_upd['stderr']}")
    pg.exec_sql(f"SET maintenance_work_mem = '{MAINT_MEM}'; VACUUM events;")
    var_q = pg.explain_query(VARIANT_EXC_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "jsonb_variant_exc_ms", N_MAX, f"{var_q['exec_ms']:.2f}", "ms",
        f"0.5% str in float8, blks={var_q['total_blks']} heap_blks={var_q['heap_scan_blks']} plan={var_q['plan_tag']}")
    rec("F_probes", "jsonb_variant_exc_blks", N_MAX, str(var_q["total_blks"]), "blks",
        f"hit={var_q['shared_hit']} read={var_q['shared_read']}")
    rec("F_probes", "jsonb_variant_heap_blks", N_MAX, str(var_q["heap_scan_blks"]), "blks",
        f"plan={var_q['plan_tag']}")

    # F4: Soft-delete probes (WHERE deleted_at IS NULL and WHERE deleted_at IS NOT NULL)
    f4_null = pg.explain_query(F4_SOFT_DEL_NULL_SQL, workers=0, runs=3, warmup=1)
    f4_not_null = pg.explain_query(F4_SOFT_DEL_NOT_NULL_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "f4_soft_del_null_ms", N_MAX, f"{f4_null['exec_ms']:.2f}", "ms",
        f"blks={f4_null['total_blks']} plan_blks={f4_null['plan_blks']} plan={f4_null['plan_tag']}")
    rec("F_probes", "f4_soft_del_null_blks", N_MAX, str(f4_null["total_blks"]), "blks",
        f"hit={f4_null['shared_hit']} read={f4_null['shared_read']}")
    rec("F_probes", "f4_soft_del_not_null_ms", N_MAX, f"{f4_not_null['exec_ms']:.2f}", "ms",
        f"blks={f4_not_null['total_blks']} plan_blks={f4_not_null['plan_blks']} plan={f4_not_null['plan_tag']}")
    rec("F_probes", "f4_soft_del_not_null_blks", N_MAX, str(f4_not_null["total_blks"]), "blks",
        f"hit={f4_not_null['shared_hit']} read={f4_not_null['shared_read']}")

    # F5: Single-entity (median) & mega-entity (5% rows) scoped aggregate
    f5_single = pg.explain_query(F5_SINGLE_ENTITY_SQL, workers=0, runs=3, warmup=1)
    f5_mega = pg.explain_query(F5_MEGA_ENTITY_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "f5_single_entity_ms", N_MAX, f"{f5_single['exec_ms']:.2f}", "ms",
        f"entity={MEDIAN_ENTITY_ID} blks={f5_single['total_blks']} plan_blks={f5_single['plan_blks']} plan={f5_single['plan_tag']}")
    rec("F_probes", "f5_single_entity_blks", N_MAX, str(f5_single["total_blks"]), "blks",
        f"hit={f5_single['shared_hit']} read={f5_single['shared_read']}")
    rec("F_probes", "f5_mega_entity_ms", N_MAX, f"{f5_mega['exec_ms']:.2f}", "ms",
        f"entity={MEGA_ENTITY_ID} (5% rows) blks={f5_mega['total_blks']} plan_blks={f5_mega['plan_blks']} plan={f5_mega['plan_tag']}")
    rec("F_probes", "f5_mega_entity_blks", N_MAX, str(f5_mega["total_blks"]), "blks",
        f"hit={f5_mega['shared_hit']} read={f5_mega['shared_read']}")

    # F6: MATERIALIZED CTE scope join (catches 509x Lookup Join overlapping-segment regression)
    f6_cte = pg.explain_query(F6_MATERIALIZED_CTE_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "f6_materialized_cte_ms", N_MAX, f"{f6_cte['exec_ms']:.2f}", "ms",
        f"parent={MEGA_PARENT_ID} blks={f6_cte['total_blks']} plan_blks={f6_cte['plan_blks']} plan={f6_cte['plan_tag']}")
    rec("F_probes", "f6_materialized_cte_blks", N_MAX, str(f6_cte["total_blks"]), "blks",
        f"hit={f6_cte['shared_hit']} read={f6_cte['shared_read']}")

    # F7: JSONB key-exists (`?`) probe (catches 30x jsonb_exists heap re-read)
    f7_exists = pg.explain_query(F7_JSONB_EXISTS_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "f7_jsonb_exists_ms", N_MAX, f"{f7_exists['exec_ms']:.2f}", "ms",
        f"blks={f7_exists['total_blks']} heap_blks={f7_exists['heap_scan_blks']} reread_rows={f7_exists['heap_reread_rows']} plan_blks={f7_exists['plan_blks']} plan={f7_exists['plan_tag']}")
    rec("F_probes", "f7_jsonb_exists_blks", N_MAX, str(f7_exists["total_blks"]), "blks",
        f"hit={f7_exists['shared_hit']} read={f7_exists['shared_read']}")
    rec("F_probes", "f7_jsonb_exists_heap_blks", N_MAX, str(f7_exists["heap_scan_blks"]), "blks",
        f"reread_rows={f7_exists['heap_reread_rows']} plan={f7_exists['plan_tag']}")
    rec("F_probes", "f7_jsonb_exists_reread_rows", N_MAX, str(f7_exists["heap_reread_rows"]), "rows",
        f"heap_blks={f7_exists['heap_scan_blks']} plan={f7_exists['plan_tag']}")

    # F8: Scoped secondary-sort Top-20 (ORDER BY updated_at DESC LIMIT 20; catches 60x regression)
    f8_upd_top20 = pg.explain_query(F8_UPDATED_AT_TOP20_SQL, workers=0, runs=3, warmup=1)
    rec("F_probes", "f8_updated_at_top20_ms", N_MAX, f"{f8_upd_top20['exec_ms']:.2f}", "ms",
        f"parent={MEGA_PARENT_ID} blks={f8_upd_top20['total_blks']} plan_blks={f8_upd_top20['plan_blks']} plan={f8_upd_top20['plan_tag']}")
    rec("F_probes", "f8_updated_at_top20_blks", N_MAX, str(f8_upd_top20["total_blks"]), "blks",
        f"hit={f8_upd_top20['shared_hit']} read={f8_upd_top20['shared_read']}")

    verify_bad = verify_diff(pg, rec, N_MAX) if args.verify_diff else []

    # Planning shared-buffer gate across Q1-Q8 + F4-F8 at N_MAX
    plan_probe_results = list(full_q.values()) + [
        f4_null, f4_not_null, f5_single, f5_mega, f6_cte, f7_exists, f8_upd_top20
    ]
    all_plan_blks = [r["plan_blks"] for r in plan_probe_results]
    all_plan_ms = [r["plan_ms"] for r in plan_probe_results]
    plan_buffers_median = statistics.median(all_plan_blks) if all_plan_blks else 0.0
    plan_buffers_max = max(all_plan_blks) if all_plan_blks else 0
    plan_ms_median = statistics.median(all_plan_ms) if all_plan_ms else 0.0
    rec("F_probes", "plan_buffers_median", N_MAX, f"{plan_buffers_median:.1f}", "blks",
        f"max={plan_buffers_max} plan_ms_med={plan_ms_median:.2f}ms across {len(all_plan_blks)} queries (Q1-Q8, F4-F8)")
    rec("F_probes", "plan_buffers_max", N_MAX, str(plan_buffers_max), "blks",
        f"median={plan_buffers_median:.1f} across {len(all_plan_blks)} queries")

    # --- Workload E: Idle Pooled Backend Memory ---
    print("[v6-core] === Workload E (Idle Pooled Backend Memory) ===", flush=True)
    idle = run_workload_e_idle_pooled(pg)
    rec("E_idle_backend", "rss_anon_idle_mb", idle["indexes_touched"], f"{idle['anon_idle_mb']:.2f}", "MB",
        f"cap={idle['cache_cap']} before={idle['anon_before_mb']:.2f}MB active={idle['anon_active_mb']:.2f}MB "
        f"delta={idle['anon_delta_mb']:.2f}MB hwm={idle['hwm_mb']:.2f}MB {idle['delta_kb_per_index']:.1f}KB/idx")
    rec("E_idle_backend", "rss_anon_delta_mb", idle["indexes_touched"], f"{idle['anon_delta_mb']:.2f}", "MB",
        f"cap={idle['cache_cap']} ({idle['delta_kb_per_index']:.1f}KB/idx)")

    # --- Workload G: Index Size, Page Utilization, WAL Efficiency & Compaction WA ---
    print("[v6-core] === Workload G (Size, Page Utilization, WAL & Compaction WA) ===", flush=True)
    sd_max = scale_data[N_MAX]
    heap_table_bytes = sd_max["heap_table_bytes"]
    heap_rel_bytes = sd_max["heap_rel_bytes"]
    heap_total_bytes = sd_max["heap_total_bytes"]
    ent_bytes_max = sd_max["entity"]["size_bytes"]
    cic_bytes_max = sd_max["cic"]["size_bytes"]
    cic_wal_max = sd_max["cic"]["wal_bytes"]
    clean_bd = sd_max["clean_breakdown"]

    g_size = {
        "heap_size_mb": heap_table_bytes / (1024 * 1024),
        "heap_b_per_row": heap_table_bytes / float(N_MAX),
        "entity_b_per_row": ent_bytes_max / float(N_MAX),
        "scoped_b_per_row": cic_bytes_max / float(N_MAX),
        "scoped_ex_uuid_b_per_row": (cic_bytes_max - UUID_BYTES * N_MAX) / float(N_MAX),
        "scoped_to_heap_ratio": (cic_bytes_max / float(heap_table_bytes)) if heap_table_bytes > 0 else 0.0,
        "scope_overhead_pct": sd_max["cic"]["overhead_pct"],
        "breakdown": clean_bd,
    }
    rec("G_size", "heap_size_mb", N_MAX, f"{g_size['heap_size_mb']:.2f}", "MB",
        f"pg_table_size; rel={heap_rel_bytes/(1024*1024):.2f}MB total={heap_total_bytes/(1024*1024):.2f}MB")
    rec("G_size", "heap_b_per_row", N_MAX, f"{g_size['heap_b_per_row']:.2f}", "B/row", "heap+TOAST via pg_table_size")
    rec("G_size", "entity_b_per_row", N_MAX, f"{g_size['entity_b_per_row']:.2f}", "B/row", "entity-only sift_col")
    rec("G_size", "scoped_b_per_row", N_MAX, f"{g_size['scoped_b_per_row']:.2f}", "B/row", "scoped CIC sift_col")
    rec("G_size", "scoped_ex_uuid_b_per_row", N_MAX, f"{g_size['scoped_ex_uuid_b_per_row']:.2f}", "B/row",
        f"scoped CIC minus {UUID_BYTES} B MD5 UUID per row")
    rec("G_size", "scoped_to_heap_ratio", N_MAX, f"{g_size['scoped_to_heap_ratio']:.3f}", "x", "scoped_bytes / pg_table_size(events)")
    rec("G_size", "scope_overhead_pct", N_MAX, f"{g_size['scope_overhead_pct']:.2f}", "%", "scoped vs entity size overhead")

    if clean_bd["available"]:
        rec("G_size", "page_utilization_pct", N_MAX, f"{clean_bd['page_utilization_pct']:.2f}", "%", f"via {clean_bd['fn']}")
        rec("G_size", "dead_space_pct", N_MAX, f"{clean_bd['dead_space_pct']:.2f}", "%", f"post-CIC via {clean_bd['fn']}")
        for sec in BREAKDOWN_SECTIONS:
            sb = clean_bd["sections_bytes"][sec]
            sbpr = clean_bd["sections_b_per_row"][sec]
            rec("G_size", f"section_bytes_{sec}", N_MAX, f"{sbpr:.2f}", "B/row", f"{sb} bytes")
    else:
        rec("G_size", "page_utilization_pct", N_MAX, "n/a", "%", "sift_index_size_breakdown unavailable")
        rec("G_size", "dead_space_pct", N_MAX, "n/a", "%", "sift_index_size_breakdown unavailable")
        for sec in BREAKDOWN_SECTIONS:
            rec("G_size", f"section_bytes_{sec}", N_MAX, "n/a", "B/row", "sift_index_size_breakdown unavailable")

    n_2pct = max(1.0, N_MAX * 0.02)
    g_wal = {
        "cic_wal_b_per_row": cic_wal_max / float(N_MAX),
        "oltp_ins1_wal_b_per_row": c_ins["wal_bytes"] / 1000.0,
        "update_2pct_wal_b_per_row": c_upd["wal_bytes"] / n_2pct,
        "delete_2pct_wal_b_per_row": c_del["wal_bytes"] / n_2pct,
    }
    rec("G_wal", "cic_wal_b_per_row", N_MAX, f"{g_wal['cic_wal_b_per_row']:.2f}", "B/row", f"{cic_wal_max/(1024*1024):.2f} MB total")
    rec("G_wal", "oltp_ins1_wal_b_per_row", N_MAX, f"{g_wal['oltp_ins1_wal_b_per_row']:.2f}", "B/row", "1000x 1-row INSERT")
    rec("G_wal", "update_2pct_wal_b_per_row", N_MAX, f"{g_wal['update_2pct_wal_b_per_row']:.2f}", "B/row", f"{int(n_2pct)} rows")
    rec("G_wal", "delete_2pct_wal_b_per_row", N_MAX, f"{g_wal['delete_2pct_wal_b_per_row']:.2f}", "B/row", f"{int(n_2pct)} rows")

    g_wa = run_workload_g_compaction_wa(pg, n_base=40000, waves=5, churn_pct=0.02)
    rec("G_compaction", "write_amplification", g_wa["n_base"], f"{g_wa['write_amplification']:.2f}", "x",
        f"{g_wa['waves']} waves x 2% ins/del+VAC, wal={g_wa['total_wal_bytes']/(1024*1024):.2f}MB logical={g_wa['logical_inserted_bytes']/(1024*1024):.2f}MB")
    rec("G_compaction", "index_growth_ratio", g_wa["n_base"], f"{g_wa['index_growth_ratio']:.3f}", "x",
        f"init={g_wa['init_idx_bytes']/(1024*1024):.2f}MB post={g_wa['post_idx_bytes']/(1024*1024):.2f}MB")
    if g_wa["breakdown"]["available"]:
        rec("G_compaction", "dead_space_pct", g_wa["n_base"], f"{g_wa['breakdown']['dead_space_pct']:.2f}", "%",
            f"post-churn via {g_wa['breakdown']['fn']}")
        rec("G_compaction", "tail_free_space_pct", g_wa["n_base"], f"{g_wa['breakdown']['tail_free_space_pct']:.2f}", "%",
            f"free_dead_bytes / total_bytes post-churn via {g_wa['breakdown']['fn']}")
    else:
        rec("G_compaction", "dead_space_pct", g_wa["n_base"], "n/a", "%", "sift_index_size_breakdown unavailable")
        rec("G_compaction", "tail_free_space_pct", g_wa["n_base"], "n/a", "%", "sift_index_size_breakdown unavailable")
    rec("G_compaction", "churn_total_wal_mb", g_wa["n_base"], f"{g_wa['total_wal_bytes']/(1024*1024):.2f}", "MB",
        f"statuses={','.join(g_wa['statuses'])}")
    rec("G_compaction", "churn_elapsed_ms", g_wa["n_base"], f"{g_wa['elapsed_ms']:.1f}", "ms",
        f"{g_wa['waves']} waves at N={g_wa['n_base']}")

    suite_sec = time.monotonic() - t_suite_start
    rec("meta", "bench_suite_time", "-", f"{suite_sec:.2f}", "s", "end-to-end SQL telemetry")

    # --- Write TSV ---
    today = datetime.now(timezone.utc).strftime("%Y-%m-%d")
    meta = {"version": ext_ver, "git": args.git_sha, "pg": pg_ver, "cpu": cpu_model(), "date": today}
    tsv_path = os.path.join(args.out_dir, f"{args.round}.tsv")
    write_tsv(tsv_path, tsv_rows, meta)
    if args.docs:
        write_tsv(os.path.join(REPO, "docs", "results", "v6-core.tsv"), tsv_rows, meta)

    # --- Write Markdown Report ---
    md_path = os.path.join(args.out_dir, f"{args.round}.md")
    ts = datetime.now(timezone.utc).strftime("%Y-%m-%d %H:%M:%S UTC")

    lines = [
        f"# `pg_sift` Benchmark Report — `{args.round}`",
        "",
        f"- **Timestamp:** {ts}",
        f"- **Version / Git:** `pg_sift` `{ext_ver}` (`{args.git_branch}` @ `{args.git_sha}`), PostgreSQL `{pg_ver}`",
        f"- **Runtime:** build `{args.build_sec:.2f}s`, benchmark suite `{suite_sec:.2f}s`",
        f"- **Budgets:** `maintenance_work_mem = '{MAINT_MEM}'`, `work_mem = '{WORK_MEM}'` (parallel 1v4: `{PAR_WORK_MEM}`), `sift.backend_cache_kb = '{BACKEND_CACHE_CAP}'` (Workload E), `shared_buffers = '256MB'`",
        f"- **Schema:** 15-col wide `events` (`deleted_at` 0.1% historical soft-delete, `updated_at`, `NUMERIC(12,2)`, `UUID`, 3 JSONB cols with sparse `analysis` + `flag`), `key_order = '{INDEX_KEY_ORDER}'`, Zipf-skewed M:N `scope` (`{mult:.2f}x` avg mult, `{scope_info['single_parent_pct']:.1f}%` 1-parent, max `{scope_info['max_mult']}`, top-10 parents `{scope_info['top10_pct']:.1f}%`, mega-entity `5%` rows)",
        "",
        "---",
        "",
        f"## Scaling Growth Factors ({' → '.join(klabel(n) for n in SCALES)}, {SCALES[1] // SCALES[0]}× Jumps)",
        "",
        f"| Metric | OLS Slope (`{klabel(SCALES[0])}..{klabel(SCALES[-1])}`) | " + " | ".join(klabel(n) for n in SCALES) + " |",
        "|---|---:|" + "---:|" * len(SCALES),
    ] + growth_rows(scale_data, slopes) + [
        "",
        "## Workload A — Scoped Wide+JSONB Index Build & CIC",
        "",
        "### Plain `CREATE INDEX` (Entity vs Scoped) & Serial CIC",
        "",
        "| Rows | Entity Build | Plain Scoped (`vs ent`) | Plain Temp | CIC Total (`us/row`) | CIC Pass 1 | CIC Pass 2 | Pass2/Pass1 | Entity Size | Scoped Size (`+ovh%`) | CIC WAL | CIC Temp | CIC `RssAnon`/`VmHWM` |",
        "|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|---:|",
    ]
    for n in SCALES:
        e = scale_data[n]["entity"]
        sp = scale_data[n]["scoped_plain"]
        c = scale_data[n]["cic"]
        lines.append(
            f"| {n:,} | {e['elapsed_ms']:.0f} ms | {sp['elapsed_ms']:.0f} ms ({sp['ratio_vs_entity']:.2f}×) | "
            f"{sp['temp_bytes']/(1024*1024):.2f} MB | "
            f"{c['elapsed_ms']:.0f} ms ({c['elapsed_ms']*1000/n:.1f}) | "
            f"{c['pass1_ms']:.0f} ms | {c['pass2_ms']:.0f} ms | {c['pass2_ratio']:.2f}× | "
            f"{e['size_mb']:.2f} MB ({e['size_blks']} blks) | {c['size_mb']:.2f} MB (+{c['overhead_pct']:.1f}%) | "
            f"{c['wal_bytes']/(1024*1024):.2f} MB | {c['temp_bytes']/(1024*1024):.2f} MB | "
            f"{c['peak_anon_mb']:.1f} / {c['peak_hwm_mb']:.1f} MB |"
        )

    cp = scale_data[N_MAX]["cic_par2"]
    cs = scale_data[N_MAX]["cic"]
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
        v = scale_data[n]["vac1"]
        lines.append(
            f"| First VACUUM after CIC | {n:,} | {v['status']} | {v['elapsed_ms']:.0f} ms | "
            f"{v['peak_anon_mb']:.1f} MB | {v['peak_hwm_mb']:.1f} MB | {v['wal_bytes']/(1024*1024):.2f} MB | "
            f"{v['blks_touched']:,} | {v['size_after_mb']:.2f} MB |"
        )
    lines.append(
        f"| Post-Delete VACUUM (2% del) | {N_MAX:,} | {vac2['status']} | {vac2['elapsed_ms']:.0f} ms | "
        f"{vac2['peak_anon_mb']:.1f} MB | {vac2['peak_hwm_mb']:.1f} MB | {vac2['wal_bytes']/(1024*1024):.2f} MB | "
        f"{vac2['blks_touched']:,} | {vac2['size_after_mb']:.2f} MB |"
    )

    lines += [
        "",
        f"## Workload C — OLTP Mutations & Dirty-State Resilience (`N = {N_MAX:,}`)",
        "",
        "| Operation | Latency | Per-Row | WAL | Peak `RssAnon` / `VmHWM` |",
        "|---|---:|---:|---:|---:|",
        f"| Single-row INSERT (1,000 rows) | {c_ins['elapsed_ms']:.1f} ms | {c_ins['elapsed_ms']/1000.0:.3f} ms/row | {c_ins['wal_bytes']/(1024*1024):.2f} MB | {c_ins['peak_anon_mb']:.1f} / {c_ins['peak_hwm_mb']:.1f} MB |",
        f"| Bulk UPDATE (2% = 3,200 rows: 73% JSONB, 13% soft-del, 14% scalar) | {c_upd['elapsed_ms']:.1f} ms | {c_upd['elapsed_ms']/3200.0:.3f} ms/row | {c_upd['wal_bytes']/(1024*1024):.2f} MB | {c_upd['peak_anon_mb']:.1f} / {c_upd['peak_hwm_mb']:.1f} MB |",
        f"| Bulk DELETE (2% scattered = 3,200 rows) | {c_del['elapsed_ms']:.1f} ms | {c_del['elapsed_ms']/3200.0:.3f} ms/row | {c_del['wal_bytes']/(1024*1024):.2f} MB | {c_del['peak_anon_mb']:.1f} / {c_del['peak_hwm_mb']:.1f} MB |",
        "",
        "### Dirty-State Query Latency (Before vs After 2% Unvacuumed Deletes)",
        "",
        "| Query | Clean (ms) | Dirty 2% (ms) | Dirty/Clean Ratio | Clean Blks | Dirty Blks |",
        "|---|---:|---:|---:|---:|---:|",
    ]
    for qk in dirty_probe_keys:
        lines.append(
            f"| `{qk}` | {clean_q[qk]['exec_ms']:.2f} | {dirty_q[qk]['exec_ms']:.2f} | "
            f"**{dirty_ratios[qk]:.2f}×** | {clean_q[qk]['total_blks']:,} | {dirty_q[qk]['total_blks']:,} |"
        )

    lines += [
        "",
        "## Workload D — Analytical Aggregations & Query Scaling",
        "",
        f"### Query Complexity Scaling (`{' → '.join(klabel(n) for n in SCALES)}`, 1× → {SCALES[1] // SCALES[0]}× → {SCALES[-1] // SCALES[0]}× Data Size)",
        "",
        "| Query | " + " | ".join(f"{klabel(n)} (`ms` / `blks`)" for n in SCALES) + f" | {SCALES[-1] // SCALES[0]}× Time Growth | {SCALES[-1] // SCALES[0]}× Blks Growth |",
        "|---|" + "---:|" * (len(SCALES) + 2),
    ] + query_growth_rows(scale_data)

    lines += [
        "",
        f"### Full Analytical Suite Q1–Q8 (`N = {N_MAX:,}`, `work_mem = {WORK_MEM}`; 1v4 Parallel at `work_mem = {PAR_WORK_MEM}`)",
        "",
        "| Query | 1w (`4MB`) | 1w (`16MB`) | 4w (`16MB`) | Workers Launched | 4w Speedup | Shared Hit / Read | Plan Blks | Peak `RssAnon` | Plan |",
        "|---|---:|---:|---:|---:|---:|---:|---:|---:|---|",
    ]
    for qk in QUERIES:
        q1 = full_q[qk]
        if qk in par_q:
            q4 = par_q[qk]
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

    page_util_str = f"{clean_bd['page_utilization_pct']:.2f}%" if clean_bd["available"] else "n/a"
    clean_dead_str = f"{clean_bd['dead_space_pct']:.2f}%" if clean_bd["available"] else "n/a"
    clean_bd_src = clean_bd["fn"] if clean_bd["available"] else "sift_index_size_breakdown unavailable"
    churn_dead_str = f"{g_wa['breakdown']['dead_space_pct']:.2f}%" if g_wa["breakdown"]["available"] else "n/a"
    churn_bd_src = g_wa["breakdown"]["fn"] if g_wa["breakdown"]["available"] else "sift_index_size_breakdown unavailable"
    sec_row_md = (
        "| " + " | ".join(f"{clean_bd['sections_b_per_row'][s]:.2f}" for s in BREAKDOWN_SECTIONS) + " |"
        if clean_bd["available"]
        else "| " + " | ".join("n/a" for _ in BREAKDOWN_SECTIONS) + " |"
    )

    lines += [
        "",
        f"## Workload F — Compact v6 & Real-World Probes (`N = {N_MAX:,}`)",
        "",
        "| Probe | Measurement | Details |",
        "|---|---:|---|",
        f"| **F1: 6-CTE Planning Latency** | **{cte6['plan_ms']:.2f} ms** | `EXPLAIN (SUMMARY, BUFFERS)` median of 5 runs, plan blks `{cte6['plan_blks']:,}` (`plan={cte6['plan_tag']}`) |",
        f"| **F2: Pair-Edit Refresh (`-5/+5` pairs)** | **{pair_first['exec_ms']:.2f} ms** (1st) / **{pair_med['exec_ms']:.2f} ms** (med) | DML `{pair_dml['elapsed_ms']:.2f} ms`, clean `{pair_before['exec_ms']:.2f} ms`, blks `{pair_first['total_blks']:,}` (`plan={pair_first['plan_tag']}`) |",
        f"| **F3: JSONB Variant-Exception (0.5% str in float8)** | **{var_q['exec_ms']:.2f} ms** | total blks `{var_q['total_blks']:,}`, heap scan blks `{var_q['heap_scan_blks']:,}` (`plan={var_q['plan_tag']}`) |",
        f"| **F4a: Soft-Delete `deleted_at IS NULL`** | **{f4_null['exec_ms']:.2f} ms** | parent `{MID_PARENT_ID}`, blks `{f4_null['total_blks']:,}`, plan blks `{f4_null['plan_blks']:,}` (`plan={f4_null['plan_tag']}`) |",
        f"| **F4b: Soft-Delete `deleted_at IS NOT NULL`** | **{f4_not_null['exec_ms']:.2f} ms** | parent `{MEGA_PARENT_ID}`, blks `{f4_not_null['total_blks']:,}`, plan blks `{f4_not_null['plan_blks']:,}` (`plan={f4_not_null['plan_tag']}`) |",
        f"| **F5a: Single-Entity Scoped Agg** | **{f5_single['exec_ms']:.2f} ms** | entity `{MEDIAN_ENTITY_ID}`, blks `{f5_single['total_blks']:,}`, plan blks `{f5_single['plan_blks']:,}` (`plan={f5_single['plan_tag']}`) |",
        f"| **F5b: Mega-Entity Scoped Agg (`5%` rows)** | **{f5_mega['exec_ms']:.2f} ms** | entity `{MEGA_ENTITY_ID}`, blks `{f5_mega['total_blks']:,}`, plan blks `{f5_mega['plan_blks']:,}` (`plan={f5_mega['plan_tag']}`) |",
        f"| **F6: `MATERIALIZED` CTE Scope Join** | **{f6_cte['exec_ms']:.2f} ms** | parent `{MEGA_PARENT_ID}`, blks `{f6_cte['total_blks']:,}`, plan blks `{f6_cte['plan_blks']:,}` (`plan={f6_cte['plan_tag']}`) |",
        f"| **F7: JSONB Exists (`analysis ? 'flag'`)** | **{f7_exists['exec_ms']:.2f} ms** | blks `{f7_exists['total_blks']:,}`, heap blks `{f7_exists['heap_scan_blks']:,}`, re-read rows `{f7_exists['heap_reread_rows']:,}`, plan blks `{f7_exists['plan_blks']:,}` (`plan={f7_exists['plan_tag']}`) |",
        f"| **F8: Scoped `ORDER BY updated_at DESC LIMIT 20`** | **{f8_upd_top20['exec_ms']:.2f} ms** | parent `{MEGA_PARENT_ID}`, blks `{f8_upd_top20['total_blks']:,}`, plan blks `{f8_upd_top20['plan_blks']:,}` (`plan={f8_upd_top20['plan_tag']}`) |",
        f"| **Planning Shared Buffers (Q1–Q8, F4–F8)** | **{plan_buffers_median:.1f} blks** (med) | max `{plan_buffers_max:,}` blks, median planning time `{plan_ms_median:.2f} ms` across `{len(all_plan_blks)}` queries |",
        "",
        "## Workload E — Idle Pooled Backend Memory",
        "",
        f"- **Cache cap & indexes touched:** `sift.backend_cache_kb = '{idle['cache_cap']}'`, {idle['indexes_touched']} indexes (`idx_events_scoped` + `entity_parents_journal` + {idle['extra_tables']} `sift_col` tables)",
        f"- **Resident `RssAnon` (before → active → idle):** `{idle['anon_before_mb']:.2f} MB` → `{idle['anon_active_mb']:.2f} MB` → **`{idle['anon_idle_mb']:.2f} MB`** (`+{idle['anon_delta_mb']:.2f} MB` delta, `VmRSS = {idle['rss_idle_mb']:.2f} MB`, `VmHWM = {idle['hwm_mb']:.2f} MB`, `+{idle['delta_kb_per_index']:.1f} KB/index`)",
        "",
        f"## Workload G — Index Size, Page Utilization, WAL Efficiency & Compaction WA (`N = {N_MAX:,}` & WA Probe `N = {g_wa['n_base']:,}`)",
        "",
        "### Size & Page Utilization",
        "",
        "| Metric | Value | Notes |",
        "|---|---:|---|",
        f"| **Heap + TOAST (`pg_table_size`)** | **{g_size['heap_b_per_row']:.1f} B/row** ({g_size['heap_size_mb']:.2f} MB) | `pg_relation_size={heap_rel_bytes/(1024*1024):.2f} MB`, `pg_total_relation_size={heap_total_bytes/(1024*1024):.2f} MB` |",
        f"| **Entity Index** | **{g_size['entity_b_per_row']:.1f} B/row** ({ent_bytes_max/(1024*1024):.2f} MB) | entity-only `sift_col` |",
        f"| **Scoped Index (CIC)** | **{g_size['scoped_b_per_row']:.1f} B/row** ({cic_bytes_max/(1024*1024):.2f} MB) | `+{g_size['scope_overhead_pct']:.1f}%` vs entity |",
        f"| **Scoped / Heap Ratio** | **{g_size['scoped_to_heap_ratio']:.3f}×** | `scoped_bytes / pg_table_size('events')` |",
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
        f"| **CIC Build WAL** | **{g_wal['cic_wal_b_per_row']:.1f} B/row** | {cic_wal_max/(1024*1024):.2f} MB total at `N = {N_MAX:,}` |",
        f"| **OLTP 1-row INSERT WAL** | **{g_wal['oltp_ins1_wal_b_per_row']:.1f} B/row** | 1,000 × 1-row autocommit INSERTs |",
        f"| **2% UPDATE WAL** | **{g_wal['update_2pct_wal_b_per_row']:.1f} B/row** | {int(n_2pct):,} rows |",
        f"| **2% DELETE WAL** | **{g_wal['delete_2pct_wal_b_per_row']:.1f} B/row** | {int(n_2pct):,} scattered rows |",
        f"| **Compaction Write Amplification ({g_wa['waves']}× 2% churn + VAC)** | **{g_wa['write_amplification']:.2f}×** | `WAL {g_wa['total_wal_bytes']/(1024*1024):.2f} MB` / `logical {g_wa['logical_inserted_bytes']/(1024*1024):.2f} MB` (`{g_wa['elapsed_ms']:.0f} ms` at `N = {g_wa['n_base']:,}`) |",
        f"| **Post-Churn Index Growth Ratio** | **{g_wa['index_growth_ratio']:.3f}×** | `{g_wa['init_idx_bytes']/(1024*1024):.2f} MB` → `{g_wa['post_idx_bytes']/(1024*1024):.2f} MB` |",
        f"| **Post-Churn Dead Space** | **{churn_dead_str}** | {churn_bd_src} |",
        "",
    ]

    with open(md_path, "w", encoding="utf-8") as f:
        f.write("\n".join(lines) + "\n")

    print(f"[v6-core] wrote {tsv_path} and {md_path}" + (" and docs/results/v6-core.tsv" if args.docs else ""), flush=True)
    if verify_bad:
        sys.exit(f"verify-diff: the sift plan and the heap scan disagree on {len(verify_bad)} probes: {', '.join(verify_bad)}")


if __name__ == "__main__":
    main()
