import json
import statistics
import subprocess
import threading
import time
from .config import WORK_MEM, read_proc_mem_kb


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
