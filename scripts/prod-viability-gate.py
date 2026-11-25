#!/usr/bin/env python3
"""The production viability gate: what an operation holds, writes to pgsql_tmp and costs against the rows of its table, at 1x, 2x and 4x (test/t/agg/viability_gate.sh).

27 cases across the four lifecycle classes (A creations, B writes, C maintenance, D reads, E error and cancel cleanup), each measured in fresh backends on tables of N, 2N and 4N rows
(N = 32,768: whole granules of 8,192 rows, so no size is cut by a granule boundary). The tables are built with maintenance_work_mem 64 MB and the operation runs at 1 to 4 MB: a part
built at the large budget holds several times the memory the operation may use, which is what a VACUUM of a 50 GB table finds. Reads are measured twice and the lower peak is kept.
With P the peak anonymous memory the backend sampled itself (sift._test_rss_peak_*), X the bytes the memory scopes of sift counted (sift._test_mem_highwater, 0 when a path has no scope),
B the budget of the case and dN = N(4x) - N(1x):
  alpha_mem = max(P4, F) / max(P1, F) with F = max(B, 2 MiB) (X with max(B, 1 MiB), the larger of both when X4 > 0)  at most 1.15
  beta_rss  = (P4 - P1) / dN and beta_exact = (X4 - X1) / dN, in bytes a row                                          at most 8 and 2
  P4 at most 3 B + base (8 MiB, 12 for a maintenance, 16 for a parallel build)
  temp_bytes (pg_stat_database) and the files left in pg_ls_tmpdir(): `zero` cases write none under temp_file_limit 1 MB, `cap` cases stay under their cap with alpha_tmp at most 5, every case leaves none
  alpha_time = T4 / max(T1, 0.05 s) at most 5.5 (a case over it measures the smallest and the largest size once more and keeps the lower times); alpha_disk = size after a churn and VACUUM over the size of a clean build, at most 2 and no growth over 3 cycles
  maint_autovacuum_isolation: a real autovacuum worker with autovacuum_work_mem 2 MB and maintenance_work_mem 64 MB stays under 20 MiB and its scopes report that budget.

  scripts/prod-viability-gate.py run [--scale-base N] [--only RE] [--summary-tsv PATH] [--dir D] [--av-host H --av-port P --av-log FILE] [--no-rss]
  scripts/prod-viability-gate.py list [--only RE]
  scripts/prod-viability-gate.py --selftest
Reads PGHOST, PGPORT, PGUSER and PGDATABASE like psql, PROD_GATE_ROWS (the 1x size), PROD_GATE_LANES (cases side by side, each in a database of its own: 2) and PROD_GATE_CACHE (the
sift.backend_cache_kb of the operations: 1MB); needs Linux (/proc) and an installed pg_sift and pg_sift_test. Prints GATE_HEADER and one GATE_ROW a case, GATE_TIME lines on stderr.
An operation that fails keeps what it measured up to the failure, a crash of a backend restarts the server and the trial runs again (twice at most).
"""
from __future__ import annotations

import argparse, collections, concurrent.futures, os, re, shutil, subprocess, sys, tempfile, threading, time
from typing import NamedTuple

MIB = 1048576
ALPHA_MEM, BETA_EXACT, BETA_RSS, CAP_X, ALPHA_TIME, ALPHA_TMP, ALPHA_DISK, DISK_FLAT, TIME_FLOOR = 1.15, 2.0, 8.0, 3.0, 5.5, 5.0, 2.0, 1.15, 0.05
AV_RSS_CAP, AV_BUDGET = 20 * MIB, 2 * MIB
SCALES = (("1x", 1), ("2x", 2), ("4x", 4))
HEADER = ["case", "peak_1x", "peak_2x", "peak_4x", "α_mem", "β_exact", "β_rss", "temp_bytes", "α_disk", "α_time", "verdict"]
os.environ["LC_ALL"] = "C"
os.environ["PGOPTIONS"] = "-c client_min_messages=warning"
BUILD = "set sift.async_maintenance='off'; set sift.test_create_format1=off; set max_parallel_maintenance_workers=0; set maintenance_work_mem='64MB';\n"
COLS = "k_hi, k_mid, k_lo, cat, txt, payload, doc, amount, ts"
CACHE = os.environ.get("PROD_GATE_CACHE", "1MB")
LANES = int(os.environ.get("PROD_GATE_LANES", "2"))
ZSTD = "with (compression='zstd')"
ROWS = "id, k_hi, k_mid, k_lo, cat, txt, payload, doc, amount, ts, title"
SHIFTED = "id + 10000000, k_hi + 10000000, k_mid, k_lo, cat, txt, payload, doc, amount, ts, title"
GEN = """select g::bigint as id, g::bigint as k_hi, (g % 2048)::int as k_mid, (g % 8)::int as k_lo,
 (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8] as cat,
 'k' || ((g::bigint * 2654435761) & 1048575) || '_' || repeat('abcdefgh', 7) as txt,
 repeat('seg=' || lpad((g % 16)::text, 2, '0') || '|grp=' || lpad((g % 128)::text, 3, '0') || '|pad=0123456789abcdef|', 8) as payload,
 jsonb_build_object('k', g % 64, 'tag', 't' || (g % 32), 'msg', repeat('json_payload_', 8) || (g % 1024), 'items', jsonb_build_array(
   jsonb_build_object('c', 'c' || (g % 8), 'q', (g % 50)::int), jsonb_build_object('c', 'c' || ((g + 1) % 8), 'q', ((g + 3) % 50)::int))) as doc,
 (((g::bigint * 7919) % 29500)::numeric / 100.0)::numeric(10,2) as amount, timestamp '2025-01-01' + ((g % 86400) * interval '1 second') as ts,
 (array['wireless','portable','compact','premium','rugged','slim','smart','classic'])[1 + g % 8] || ' ' ||
 (array['charger','speaker','headphones','keyboard','lamp','monitor','camera'])[1 + (g / 8) % 7] || ' term' || (g % 4096) || ' model' || (g % 512) as title
 from generate_series(1, {n}) g"""
FIXTURES = f"""drop table if exists base_{{s}}, dim_{{s}}, read_col_{{s}}, read_tbl_{{s}}, read_bm_{{s}} cascade;
create table base_{{s}} with (autovacuum_enabled = off) as {GEN};
create table dim_{{s}} with (autovacuum_enabled = off) as select g::bigint as id, (g % 7)::int as cls from generate_series(1, {{n}}) g;
create table read_col_{{s}} with (autovacuum_enabled = off) as select * from base_{{s}};
create index read_col_{{s}}_col on read_col_{{s}} using sift_col ({COLS}) {ZSTD};
create index dim_{{s}}_col on dim_{{s}} using sift_col (id, cls);
create table read_tbl_{{s}} using sift_table as select * from base_{{s}};
create table read_bm_{{s}} with (autovacuum_enabled = off) as select * from base_{{s}};
create index read_bm_{{s}}_bm on read_bm_{{s}} using sift (title);
vacuum analyze base_{{s}}; vacuum analyze dim_{{s}}; vacuum analyze read_col_{{s}}; vacuum analyze read_bm_{{s}};"""


class Case(NamedTuple):
    name: str
    cls: str                    # A creations, B writes, C maintenance, D reads, E cleanup
    budget_mb: int
    temp: str                   # zero: no temp file at all, cap: bounded (cap_mb, or 2.5 x the bytes of cols)
    base_mib: int
    ops: tuple
    setup: str = ""
    drop: str = ""
    settings: str = ""
    cols: str = ""
    cap_mb: int = 0
    post: str = ""
    runs: int = 1
    workers: bool = False
    kind: str = "scale"         # scale, av, disk, cleanup
    weight: int = 3             # seconds the case takes, to cut the lanes


def tbl(name, extra=""):
    return f"create table {name}_{{s}} using sift_table as select * from base_{{s}}; {extra}"


def heap(name, delete=""):
    return (f"create table {name}_{{s}} as select * from base_{{s}}; create index {name}_{{s}}_col on {name}_{{s}} using sift_col ({COLS}) {ZSTD}; {delete}")


def drops(*names):
    return "".join(f"drop table if exists {n}_{{s}} cascade; " for n in names)


def segments(name, k=6, expr="title"):
    ins = "".join(f"insert into {name}_{{s}} select * from base_{{s}} where id % {k} = {i}; select sift.promote('{name}_{{s}}_bm'); " for i in range(k))
    return (f"create table {name}_{{s}} (like base_{{s}}) with (autovacuum_enabled = off); create index {name}_{{s}}_bm on {name}_{{s}} using sift (({expr})); {ins}"
            f"delete from {name}_{{s}} where id % 10 < 3;")


def updates(n):
    step = n // 100
    return "\n".join(f"update b4_{{s}} set amount = amount + 1 where id between {1 + i * step} and {64 + i * step};" for i in range(100))


def read(name, q, cols, tmp="cap", mb=1):
    return Case(name, "D", mb, tmp, 8, tuple(q) if not isinstance(q, str) else (q,), cols=cols, runs=2)


Z1 = "set temp_file_limit='1MB';"
COPY_FROM = "\\copy a5_{s} from '{dump}' (format binary)"
CASES = [
    Case("create_f2_col_serial", "A", 2, "zero", 8, (f"create index a1_{{s}}_col on base_{{s}} using sift_col ({COLS}) {ZSTD}",), drop="drop index if exists a1_{s}_col;", settings=Z1),
    Case("create_f2_col_parallel", "A", 4, "zero", 16, (f"create index a2_{{s}}_col on base_{{s}} using sift_col ({COLS}) {ZSTD}",), drop="drop index if exists a2_{s}_col;", workers=True,
         settings=Z1 + "set max_parallel_maintenance_workers=2; set min_parallel_table_scan_size='64kB';"),
    Case("create_f2_col_cic", "A", 4, "zero", 8, ("create index concurrently a3_{s}_col on base_{s} using sift_col (k_hi, k_mid, k_lo, cat, txt, payload, amount, ts) " + ZSTD,),
         drop="drop index if exists a3_{s}_col;", settings=Z1),
    Case("create_f2_tbl_ctas", "A", 2, "zero", 8, ("create table a4_{s} using sift_table as select * from base_{s}",), drop=drops("a4"), settings=Z1),
    Case("create_f2_tbl_copy", "A", 2, "zero", 8, (COPY_FROM,), setup="create table a5_{s} (like base_{s}) using sift_table;", drop=drops("a5"), settings=Z1),
    Case("create_bm25_idx", "A", 2, "cap", 8, ("create index a6_{s}_bm on base_{s} using sift (title)",), drop="drop index if exists a6_{s}_bm;", cols="title"),
    Case("write_f2_insert_select", "B", 2, "zero", 8, (f"insert into b1t_{{s}} select {SHIFTED} from base_{{s}}", f"insert into b1h_{{s}} select {SHIFTED} from base_{{s}}"),
         setup=tbl("b1t") + heap("b1h"), drop=drops("b1t", "b1h"), settings=Z1),
    Case("write_f2_update_all", "B", 2, "zero", 8, ("update b2_{s} set amount = amount + 1, payload = payload || 'u', doc = jsonb_set(doc, '{k}', to_jsonb((id % 64)::int))",),
         setup=tbl("b2"), drop=drops("b2"), settings=Z1),
    Case("write_f2_delete_bulk", "B", 2, "zero", 8, ("delete from b3t_{s} where k_lo < 4", "delete from b3h_{s} where k_lo < 4"), setup=tbl("b3t") + heap("b3h"),
         drop=drops("b3t", "b3h"), settings=Z1),
    Case("write_f2_col_delta_churn", "B", 2, "zero", 8, ("@updates",), setup=tbl("b4", "alter table b4_{s} add primary key (id);"), drop=drops("b4"), settings=Z1),
    Case("maint_f2_col_vacuum_dead", "C", 2, "zero", 12, ("vacuum c1_{s}",), setup=heap("c1", "delete from c1_{s} where k_lo < 2;"), drop=drops("c1"),
         settings=Z1),
    Case("maint_f2_tbl_vacuum_dead", "C", 2, "zero", 12, ("vacuum c2_{s}",), setup=tbl("c2", "delete from c2_{s} where k_lo < 2;"), drop=drops("c2"),
         settings=Z1),
    Case("maint_f2_tbl_vacuum_freeze", "C", 2, "zero", 12, ("vacuum (freeze) c3_{s}",), setup=tbl("c3"), drop=drops("c3"),
         settings=Z1, post="select not coalesce(bool_or(unfrozen), false) from sift.parts('c3_{s}')"),
    Case("maint_f2_maintain_api", "C", 2, "zero", 12, ("select * from sift.maintain('c4h_{s}_col')", "select * from sift.maintain('c4t_{s}')"), settings=Z1,
         setup=heap("c4h", "delete from c4h_{s} where k_lo < 2;") + tbl("c4t", "delete from c4t_{s} where k_lo < 2;"), drop=drops("c4h", "c4t")),
    Case("maint_f2_check_verify", "C", 2, "zero", 12, ("select count(*) from sift.check('c5_{s}_col', true)",), setup=heap("c5"), drop=drops("c5"), settings=Z1),
    Case("maint_bm25_vacuum_bound", "C", 2, "cap", 12, ("vacuum c6_{s}",), setup=segments("c6", expr="title || ' ' || txt"), drop=drops("c6"), cap_mb=2.5,
         settings="set sift.vacuum_rewrite_limit=1;"),
    Case("maint_autovacuum_isolation", "C", 2, "zero", 12, (), kind="av"),
    Case("maint_disk_amp_3cycles", "C", 2, "zero", 12, (), kind="disk", settings=Z1),
    read("read_f2_tbl_scan_wide", "select count(*), sum(length(txt)), sum(length(payload)), sum(length(doc->>'msg')) from read_tbl_{s}", "txt", "zero"),
    read("read_v6_group_unique_int", "select k_hi, count(*), sum(amount) from read_col_{s} group by 1", "k_hi, amount"),
    read("read_v6_group_unique_text", "select txt, count(*), sum(amount) from read_col_{s} group by 1", "txt, amount"),
    read("read_v6_gen_agg_highcard", "select k_hi, string_agg(cat, ','), percentile_cont(0.5) within group (order by amount) from read_col_{s} group by 1", "k_hi, cat, amount"),
    read("read_v6_count_distinct", "select k_lo, count(distinct k_hi), count(distinct txt) from read_col_{s} group by 1", "k_hi, txt"),
    read("read_v6_order_limit_and_full", ("select id from read_col_{s} order by amount desc, k_hi limit 1000", "select k_hi, payload from read_col_{s} order by payload desc, k_hi"),
         "k_hi, payload, amount"),
    read("read_v6_grace_join_window_unnest", ("select d.cls, count(*), sum(f.amount) from read_col_{s} f join dim_{s} d on d.id = f.k_hi group by d.cls",
                                               "select id, sum(amount) over (partition by k_mid order by id) from read_col_{s}",
                                               "select e.c, count(*), sum(e.q) from read_tbl_{s} t cross join lateral jsonb_to_recordset(t.doc->'items') e(c text, q int) group by 1"),
         "k_hi, k_mid, amount, doc"),
    read("read_bm25_search", ("select id from read_bm_{s} where title @@> 'wireless charger' order by sift.score(ctid) desc, id limit 10",
                              "select cat, count(*) from read_bm_{s} where title @@> 'wireless' group by 1"), "title", "zero"),
    Case("cleanup_tmp_error_limit_cancel", "E", 2, "zero", 8, (), kind="cleanup"),
]
WEIGHTS = {"write_f2_insert_select": 8, "maint_f2_maintain_api": 7, "maint_f2_col_vacuum_dead": 4, "maint_f2_tbl_vacuum_dead": 4, "maint_f2_tbl_vacuum_freeze": 4,
           "maint_disk_amp_3cycles": 6, "cleanup_tmp_error_limit_cancel": 6, "maint_autovacuum_isolation": 10, "create_bm25_idx": 2, "read_bm25_search": 1}
CASES = [c._replace(settings=c.settings + ("set temp_file_limit='1MB';" if c.cls == "D" and c.temp == "zero" else ""), weight=WEIGHTS.get(c.name, c.weight if c.cls != "D" else 2))
         for c in CASES]


class Trial(NamedTuple):
    peak: float                 # bytes of anonymous memory above the baseline (workers added)
    exact: float
    tmp: int                    # temp_bytes delta
    left: int                   # files left in pg_ls_tmpdir()
    secs: float
    err: str = ""


class Result(NamedTuple):
    case: Case
    trials: list                # one Trial a scale (empty for av, disk and cleanup)
    fails: list
    cols: dict                  # printed cells that are not per scale: alpha_disk, details


def env_of(host=None, port=None):
    e = dict(os.environ)
    if host:
        e["PGHOST"], e["PGPORT"] = host, str(port)
    return e


class Psql:
    def __init__(self, env):
        self.p = subprocess.Popen(["psql", "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                                  text=True, bufsize=1, env=env)

    def write(self, sql, marker):
        try:
            self.p.stdin.write(sql + f"\n\\echo {marker}\n")
        except BrokenPipeError:
            pass

    def read(self, marker):
        out, seen = [], False
        for line in iter(self.p.stdout.readline, ""):
            if marker in line:
                seen = True
                break
            out.append(line)
        err = next((x.strip() for x in out if "ERROR" in x or "FATAL" in x), None)
        return out, err or (None if seen else "backend lost: " + " ".join(x.strip() for x in out[-2:]))

    def send(self, sql, marker="DONE"):
        self.write(sql, marker)
        return self.read(marker)

    def value(self, sql, tag="V"):
        out, err = self.send(sql.rstrip(";") + ";")
        if err or any("terminating connection" in x for x in out):
            raise RuntimeError(err or "backend lost: another backend crashed")
        return next(x.strip() for x in out if x.strip() and not x.startswith(("WARNING", "HINT", "DETAIL", "CONTEXT"))).split("|")[-1]

    def close(self):
        try:
            self.p.stdin.write("\\q\n")
            self.p.stdin.close()
        except BrokenPipeError:
            pass
        self.p.wait()


def server_ready(env):
    return subprocess.run(["psql", "-X", "-Atc", "select 1"], env=env, capture_output=True).returncode == 0


class Ctl(Psql):
    """the session that reads the statistics and the temp files: it connects again when a crash of a backend restarted the server"""
    def __init__(self, env):
        self.env = env
        super().__init__(env)

    def value(self, sql, tag="V"):
        try:
            return super().value(sql)
        except RuntimeError as e:
            if "lost" not in str(e):
                raise
            wait_until(lambda: server_ready(self.env), 120)
            Psql.__init__(self, self.env)
            return super().value(sql)


def script(env, sql, strict=True):
    """sql in a session of its own; a server that a crashed backend of another lane is restarting is waited for and the script runs again (every script drops what it creates first)"""
    for _ in range(3):
        p = Psql(env)
        out, err = p.send(sql, "SCRIPTDONE")
        p.close()
        if not err or not any(k in err or k in "".join(out) for k in ("recovery mode", "starting up", "backend lost", "terminating connection")):
            break
        wait_until(lambda: server_ready(env), 120)
    if err and strict:
        raise RuntimeError("setup: " + err)


def fill(sql, s, n, dump=""):
    return sql.replace("{dump}", dump).replace("{s}", s).replace("{n}", str(n))


def settings_of(c):
    wm = 1 if c.cls in "DE" else c.budget_mb
    mwm = c.budget_mb
    return (f"set sift.async_maintenance='off'; set sift.test_mem_check=off; set sift.test_create_format1=off; set jit=off; set max_parallel_workers_per_gather=0; "
            f"set max_parallel_maintenance_workers=0; set work_mem='{wm}MB'; set maintenance_work_mem='{mwm}MB'; set sift.backend_cache_kb='{CACHE}'; set sift.shared_cache_kb=0; {c.settings}\n")


def rss_children(pm, needle):
    """the sum and the largest anonymous memory (kB) of the children of the postmaster whose command line holds needle"""
    tot = best = 0
    try:
        kids = open(f"/proc/{pm}/task/{pm}/children").read().split()
    except OSError:
        return 0, 0
    for pid in kids:
        try:
            if needle in open(f"/proc/{pid}/cmdline").read().replace("\0", " "):
                kb = int(re.search(r"^RssAnon:\s+(\d+)", open(f"/proc/{pid}/status").read(), re.M).group(1))
                tot += kb
                best = max(best, kb)
        except (OSError, AttributeError):
            pass
    return tot, best


class Sampler(threading.Thread):
    """the peak of the summed anonymous memory of the processes named needle, polled every 20 ms until stop()"""
    def __init__(self, pm, needle):
        super().__init__(daemon=True)
        self.pm, self.needle, self.done, self.peak, self.single = pm, needle, threading.Event(), 0, 0

    def run(self):
        while not self.done.is_set():
            tot, best = rss_children(self.pm, self.needle)
            self.peak, self.single = max(self.peak, tot), max(self.single, best)
            self.done.wait(0.02)

    def stop(self):
        self.done.set()
        self.join()


def postmaster(ctl):
    return int(open(os.path.join(ctl.value("show data_directory"), "postmaster.pid")).readline())


def wait_until(cond, seconds):
    """true as soon as cond() holds: polled at 5 ms to 100 ms, so that a loaded host only makes it longer"""
    end, d = time.monotonic() + seconds, 0.005
    while not cond():
        if time.monotonic() > end:
            return False
        time.sleep(d)
        d = min(d * 2, 0.1)
    return True


def backend_gone(ctl, pid):
    return ctl.value(f"select count(*) from pg_stat_activity where pid = {pid}") == "0"


def temp_bytes(ctl):
    return int(ctl.value("select pg_stat_clear_snapshot(); select temp_bytes from pg_stat_database where datname = current_database()"))


def leftovers(ctl, pid=None):
    return int(ctl.value("select count(*) from pg_ls_tmpdir()" + (f" where name like 'pgsql_tmp{pid}.%'" if pid else "")))


def number(out, tag):
    return int(next(x for x in out if x.startswith(tag + "|")).strip().split("|")[1])


def trial(env, ctl, c, s, n, dump, pm):
    """setup, then one fresh backend runs the ops one by one (peak and scopes of each, the largest kept); the temp files are read once it has exited, which is when its statistics are flushed.
    An operation that fails keeps what was measured up to there (a crash restarts the server first)"""
    dump = dump.replace("{s}", s)
    script(env, BUILD + fill(c.drop + c.setup, s, n, dump))
    me = Psql(env)
    sampler = Sampler(pm, "parallel worker") if c.workers else None
    failure, peak, exact, secs, pid = "", 0.0, 0.0, 0.0, 0
    try:
        out, failure = me.send(settings_of(c) + "select 'PID', pg_backend_pid(); select sift._test_thp_disable(); select sift._test_malloc_stats(true); select count(*) from sift._test_mem_highwater(true);")
        if failure:
            raise RuntimeError("settings: " + failure)
        pid = number(out, "PID")
        tb0 = temp_bytes(ctl)
        if sampler:
            sampler.start()
        for op in c.ops:
            op = fill(updates(n) if op == "@updates" else op, s, n, dump)
            op = op if op.startswith("\\") or op.endswith(";") else op + ";"
            me.send("select 'STOP', sift._test_rss_peak_stop();")
            base = number(me.send("select 'BASE', sift._test_rss_peak_start();")[0], "BASE")
            quiet = c.cls == "D"
            t0 = time.monotonic()
            _, failure = me.send(("\\o /dev/null\n" if quiet else "") + op + ("\n\\o" if quiet else ""), "OPDONE")
            secs += time.monotonic() - t0
            out, hw_err = me.send("select 'PEAK', sift._test_rss_peak_stop(); select 'SCOPE', scope, peak from sift._test_mem_highwater(true) where parent is null;", "HWDONE")
            failure = failure or hw_err or ""
            if not hw_err:
                peak = max(peak, (number(out, "PEAK") - base) * 1024.0)
                exact = max(exact, float(sum(int(x.strip().split("|")[2]) for x in out if x.startswith("SCOPE|"))))
            if failure:
                break
        if sampler:
            sampler.stop()
            peak += sampler.peak * 1024.0
    finally:
        if sampler and sampler.is_alive():
            sampler.stop()
        me.close()
    if "lost" in failure:
        wait_until(lambda: server_ready(env), 120)
    wait_until(lambda: backend_gone(ctl, pid), 30)
    tmp, left = temp_bytes(ctl) - tb0, leftovers(ctl, pid)
    post = ctl.value(fill(c.post, s, n, dump)) if c.post and s == "4x" and not failure else "t"
    script(env, fill(c.drop, s, n, dump), strict=False)
    return Trial(peak, exact, tmp, left, secs, failure[:110]), post == "t"


def mb(b):
    return f"{b / MIB:.1f}MB"


def judge(c, trials, ns, caps, skip_rss=False):
    """the violated invariants of a scale case and its cells: alpha_mem, beta_exact, beta_rss, temp, alpha_time"""
    fails, dn = [], ns[2] - ns[0]
    errs = [t.err for t in trials if t.err]
    if any(t.err == "not run" for t in trials):
        return [f"error: {errs[0]}"], 0.0, None, 0.0, 0.0
    fails += [f"error: {errs[0]}"] if errs else []
    p, x = [t.peak for t in trials], [t.exact for t in trials]
    b = c.budget_mb * MIB
    f_rss, f_ex = max(b, 2 * MIB), max(b, MIB)
    a_rss = max(p[2], f_rss) / max(p[0], f_rss)
    a_ex = max(x[2], f_ex) / max(x[0], f_ex) if x[2] > 0 else 0.0
    alpha = max(a_ex, a_rss if not skip_rss else 0.0)
    beta_ex = (x[2] - x[0]) / dn if x[2] > 0 else None
    beta_rss = (p[2] - p[0]) / dn
    cap = CAP_X * b + c.base_mib * MIB
    if alpha > ALPHA_MEM:
        fails.append(f"α_mem={alpha:.2f}>{ALPHA_MEM}")
    if beta_ex is not None and beta_ex > BETA_EXACT:
        fails.append(f"β_exact={beta_ex:.1f}>{BETA_EXACT}")
    if not skip_rss and beta_rss > BETA_RSS:
        fails.append(f"β_rss={beta_rss:.1f}>{BETA_RSS}")
    if not skip_rss and p[2] > cap:
        fails.append(f"cap={mb(p[2])}>{mb(cap)}")
    tmp = [t.tmp for t in trials]
    if c.temp == "zero" and max(tmp) > 0:
        fails.append(f"temp_bytes={max(tmp)}>0")
    if c.temp == "cap":
        over = [t for t, k in zip(tmp, caps) if t > k]
        a_tmp = max(tmp[2], MIB) / max(tmp[0], MIB)
        if over:
            fails.append(f"temp_bytes={mb(max(tmp))}>{mb(max(caps))}")
        if a_tmp > ALPHA_TMP:
            fails.append(f"α_tmp={a_tmp:.1f}>{ALPHA_TMP}")
    if any(t.left for t in trials):
        fails.append(f"left={max(t.left for t in trials)} temp files")
    a_time = trials[2].secs / max(trials[0].secs, TIME_FLOOR)
    if a_time > ALPHA_TIME:
        fails.append(f"α_time={a_time:.2f}>{ALPHA_TIME}")
    return fails, alpha, beta_ex, beta_rss, a_time


def cells_of(r):
    c, t = r.case, r.trials
    verdict = "PASS" if not r.fails else "FAIL [" + ", ".join(r.fails) + "]"
    if c.kind == "scale" and t:
        alpha, beta_ex, beta_rss, a_time, skip = r.cols["alpha"], r.cols["beta_ex"], r.cols["beta_rss"], r.cols["a_time"], r.cols["skip"]
        peaks = ["-" if x.err == "not run" else (mb(x.peak) if not skip else "-") + (f" (ex:{mb(x.exact)})" if x.exact > 0 else " (ex:untrk)") for x in t]
        if any(x.err == "not run" for x in t):
            return [c.name, *peaks, "-", "-", "-", "-", "-", "-", verdict]
        return [c.name, *peaks, f"{alpha:.2f}", "-" if beta_ex is None else f"{beta_ex:.1f}", "-" if skip else f"{beta_rss:.1f}", f"{max(x.tmp for x in t)} B",
                "-", f"{a_time:.2f}", verdict]
    d = r.cols
    return [c.name, d.get("p1", "-"), d.get("p2", "-"), d.get("p4", "-"), "-", "-", "-", d.get("tmp", "-"), d.get("disk", "-"), "-", verdict]


def row_text(cells):
    return " | ".join(cells)


def attempt(env, ctl, c, s, n, dump, pm):
    """a crash of a backend of a lane terminates the sessions of the others: a trial that lost its backend runs again (twice at most), so that only the operation that crashes reports it"""
    for _ in range(3):
        try:
            t, ok = trial(env, ctl, c, s, n, dump, pm)
        except RuntimeError as e:
            t, ok = Trial(0, 0, 0, 0, 0, str(e)[:110]), True
        if "lost" not in t.err:
            break
    return t, ok


def run_scale(env, ctl, c, ns, dump, pm, skip_rss):
    trials, post_ok = [], True
    for (s, _), n in zip(SCALES, ns):
        best = [attempt(env, ctl, c, s, n, dump, pm) for _ in range(c.runs)]
        post_ok &= all(ok for _, ok in best)
        ts = [b[0] for b in best]
        trials.append(Trial(min(t.peak for t in ts), min(t.exact for t in ts), max(t.tmp for t in ts), max(t.left for t in ts), min(t.secs for t in ts), next((t.err for t in ts if t.err), "")))
        if "lost" in trials[-1].err:
            trials += [Trial(0, 0, 0, 0, 0, "not run")] * (len(SCALES) - len(trials))
            break
    caps = [c.cap_mb * MIB if c.cap_mb else 2.5 * input_bytes(ctl, c.cols, s) for (s, _) in SCALES] if c.temp == "cap" else [0, 0, 0]
    fails, alpha, beta_ex, beta_rss, a_time = judge(c, trials, ns, caps, skip_rss)
    if a_time > ALPHA_TIME and not any(t.err for t in trials):
        for k in (0, 2):
            again = attempt(env, ctl, c, SCALES[k][0], ns[k], dump, pm)[0]
            trials[k] = trials[k]._replace(secs=min(trials[k].secs, again.secs), peak=min(trials[k].peak, again.peak))
        fails, alpha, beta_ex, beta_rss, a_time = judge(c, trials, ns, caps, skip_rss)
    if not post_ok:
        fails.append("the operation did not leave the relation in the expected state")
    return Result(c, trials, fails, dict(alpha=alpha, beta_ex=beta_ex, beta_rss=beta_rss, a_time=a_time, skip=skip_rss))


def input_bytes(ctl, cols, s):
    return float(ctl.value(f"select sum({'+'.join(f'pg_column_size({x.strip()})' for x in cols.split(','))}) from base_{s}"))


def run_disk(env, ctl, c, ns):
    s = "2x"
    script(env, BUILD + f"drop table if exists dt, dh, dtb, dhb cascade; create table dt using sift_table as select * from base_{s}; create table dh as select * from base_{s}; "
           f"create index dh_col on dh using sift_col ({COLS}) {ZSTD}; create table dtb using sift_table as select * from base_{s}; create table dhb as select * from base_{s}; "
           f"create index dhb_col on dhb using sift_col ({COLS}) {ZSTD};")
    base_t, base_h = int(ctl.value("select pg_relation_size('dtb')")), int(ctl.value("select pg_relation_size('dhb_col')"))
    sizes, tmp0, fails = [], temp_bytes(ctl), []
    for _ in range(3):
        me = Psql(env)
        _, err = me.send(settings_of(c) + "".join(f"delete from {t} where id % 10 < 3; insert into {t} select * from base_{s} where id % 10 < 3; vacuum {t};" for t in ("dt", "dh")), "CYCLEDONE")
        me.close()
        if err:
            fails.append("churn: " + err[:100])
            break
        sizes.append((int(ctl.value("select pg_relation_size('dt')")), int(ctl.value("select pg_relation_size('dh_col')"))))
    tmp = temp_bytes(ctl) - tmp0
    ratio = max((max(a / base_t, b / base_h) for a, b in sizes), default=0.0)
    flat = max((sizes[-1][i] / sizes[0][i] for i in (0, 1)), default=0.0) if sizes else 0.0
    if ratio > ALPHA_DISK:
        fails.append(f"α_disk={ratio:.2f}>{ALPHA_DISK}")
    if flat > DISK_FLAT:
        fails.append(f"size(c3)/size(c1)={flat:.2f}>{DISK_FLAT}")
    if tmp:
        fails.append(f"temp_bytes={tmp}>0")
    script(env, "drop table if exists dt, dh, dtb, dhb cascade;", strict=False)
    return Result(c, [], fails, dict(disk=f"{ratio:.2f}" if sizes else "-", tmp=f"{tmp} B", p2=f"c3/c1={flat:.2f}" if sizes else "-"))


def activity(ctl, pid, needle):
    """running, finished (idle, its last statement holds needle) or not started"""
    state = ctl.value(f"select coalesce(max(state), 'none') from pg_stat_activity where pid = {pid} and query like '%{needle}%'")
    return "running" if state == "active" else "finished" if state == "idle" else "waiting"


BM25_BUILD = "set maintenance_work_mem='1MB'; create index e1_2x_build_bm on e1_2x using sift (title)"


def run_cleanup(env, ctl, c, ns):
    """each way a spilling statement ends (error of temp_file_limit, early LIMIT, cancel) leaves no file in pg_ls_tmpdir(); the error and LIMIT cases must have written temp files (an
    unexercised case proves nothing), a cancel that came after the statement finished is not counted against it"""
    s, big = "2x", "4x"
    script(env, BUILD + fill(drops("e1") + segments("e1"), s, ns[1]))
    group = f"select k_hi, count(*), sum(amount) from read_col_{big} group by 1"
    cases = [("group_error", f"set temp_file_limit='512kB'; {group}", "group by 1", None, True), ("group_limit", f"{group} limit 10", "limit 10", None, False),
             ("group_cancel", group, "group by 1", "cancel", False), ("bm25_build_error", f"set temp_file_limit='256kB'; {BM25_BUILD}", "e1_2x_build_bm", None, True),
             ("bm25_build_cancel", BM25_BUILD, "e1_2x_build_bm", "cancel", False)]
    fails, notes = [], []
    for name, sql, needle, cancel, must in cases:
        me = Psql(env)
        out, _ = me.send(settings_of(c) + "select 'PID', pg_backend_pid();")
        pid, tb0 = number(out, "PID"), temp_bytes(ctl)
        me.write(("\\o /dev/null\n" if "group" in name else "") + sql + ";", "QDONE")
        if cancel:
            wait_until(lambda: activity(ctl, pid, needle) != "waiting", 30)
            if wait_until(lambda: leftovers(ctl, pid) > 0 or activity(ctl, pid, needle) == "finished", 60) and leftovers(ctl, pid):
                ctl.value(f"select pg_cancel_backend({pid})")
        out, err = me.read("QDONE")
        me.close()
        wait_until(lambda: backend_gone(ctl, pid), 30)
        tmp, left = temp_bytes(ctl) - tb0, leftovers(ctl, pid)
        notes.append(f"{name}:{'error' if err else 'done'}")
        if left:
            fails.append(f"{name}: {left} temp files left")
        if must and not err and not tmp:
            fails.append(f"{name}: wrote no temp file")
    script(env, fill(drops("e1"), s, ns[1]), strict=False)
    return Result(c, [], fails, dict(tmp="0 left" if not fails else f"{len(fails)} subtests fail", p2=" ".join(notes)))


AV_XIDS = 120000


def run_av(av_env, c, ns, log):
    """the node runs with autovacuum_freeze_max_age 100000, autovacuum_work_mem 2 MB and maintenance_work_mem 64 MB: the tables are built at 64 MB, a quarter of their rows is deleted, and the
    transactions that age them past the limit make a worker vacuum them (the dead rows of a sift_table do not reach the statistics autovacuum reads, its age does)"""
    ctl = Ctl(av_env)
    n, s = ns[2], "4x"
    script(av_env, BUILD + "set temp_file_limit=-1;\n" + fill(f"drop table if exists av_base, av_tbl, av_heap, av_burn cascade; create table av_base as {GEN}; create table av_tbl using sift_table as select * from av_base; "
                                                          f"create table av_heap as select * from av_base; create index av_col on av_heap using sift_col ({COLS}) {ZSTD}; create table av_burn (i int); "
                                                          "delete from av_tbl where k_lo < 2; delete from av_heap where k_lo < 2; analyze av_base;", s, n))
    tb0, offset, pm = temp_bytes(ctl), os.path.getsize(log), postmaster(ctl)
    sampler = Sampler(pm, "autovacuum worker")
    sampler.start()
    script(av_env, f"do $$ begin for i in 1..{AV_XIDS} loop begin insert into av_burn values (i); exception when others then null; end; end loop; end $$;")
    dead = "select coalesce(sum(dead_rows), 0) from (select * from sift.parts('av_tbl') union all select * from sift.parts('av_col')) p"
    done = wait_until(lambda: ctl.value(dead) == "0" and not rss_children(pm, "autovacuum worker")[0], 60)
    sampler.stop()
    fails = [] if done else [f"dead_rows={ctl.value(dead)} 60 s after the autovacuum was due"]
    rss, tmp = sampler.single * 1024, temp_bytes(ctl) - tb0
    with open(log, "rb") as f:
        f.seek(offset)
        scopes = [int(m.group(1)) for m in re.finditer(r"SIFTMEM_SCOPE name=\S+ peak=\d+ budget=(\d+)", f.read().decode(errors="replace"))]
    over = [b for b in scopes if b > AV_BUDGET]
    if rss > AV_RSS_CAP:
        fails.append(f"av_rss={mb(rss)}>{mb(AV_RSS_CAP)}")
    if over:
        fails.append(f"scope_budget={mb(max(over))}!={mb(AV_BUDGET)}")
    if tmp:
        fails.append(f"temp_bytes={tmp}>0")
    if leftovers(ctl):
        fails.append("temp files left")
    if ctl.value("select coalesce(bool_or(unfrozen), false) from sift.parts('av_tbl')") == "t":
        fails.append("unfrozen parts after the anti-wraparound autovacuum")
    script(av_env, "drop table if exists av_base, av_tbl, av_heap, av_burn cascade;", strict=False)
    ctl.close()
    return Result(c, [], fails, dict(p4=f"{mb(rss)} (av:{mb(max(scopes))})" if scopes else f"{mb(rss)} (av:untrk)", tmp=f"{tmp} B"))


def rows_of(only):
    return [c for c in CASES if only is None or re.search(only, c.name)]


def lanes_of(cases, k):
    """the cases cut in k lanes by their weight, the heaviest first onto the lightest lane"""
    lanes = [[] for _ in range(k)]
    for c in sorted(cases, key=lambda c: -c.weight):
        min(lanes, key=lambda lane: sum(x.weight for x in lane)).append(c)
    return lanes


def run_case(c, lane, args, ns, dump_dir, pm):
    env, ctl = lane
    t0 = time.monotonic()
    try:
        if c.kind == "av":
            if not args.av_port:
                raise RuntimeError("needs --av-host, --av-port and --av-log of the node with autovacuum_work_mem 2MB")
            r = run_av(env_of(args.av_host, args.av_port), c, ns, args.av_log)
        elif c.kind == "disk":
            r = run_disk(env, ctl, c, ns)
        elif c.kind == "cleanup":
            r = run_cleanup(env, ctl, c, ns)
        else:
            r = run_scale(env, ctl, c, ns, dump_dir + "/copy_{s}.bin", pm, args.no_rss)
    except (RuntimeError, ValueError, StopIteration) as e:
        r = Result(c, [], [f"error: {str(e)[:160] or type(e).__name__}"], {})
    print(f"GATE_TIME {c.name} {time.monotonic() - t0:.1f}", file=sys.stderr, flush=True)
    return r


def run(args):
    env, ns = env_of(), [args.scale_base * m for _, m in SCALES]
    if not os.path.exists("/proc/self/status"):
        print("SKIP needs /proc")
        return 0
    cases, dump_dir = rows_of(args.only), args.dir or tempfile.mkdtemp(prefix="prod-gate-", dir=os.environ.get("SIFT_TMP"))
    admin = Ctl(env)
    pm = postmaster(admin)
    lanes = [x for x in lanes_of(cases, max(1, min(LANES, len(cases)))) if x]
    dbs = [f"pvg{i}" for i in range(len(lanes))]
    for db in dbs:
        script(env, f"drop database if exists {db} with (force); create database {db};")
    envs = [dict(env, PGDATABASE=db) for db in dbs]
    for e in envs:
        script(e, "create extension pg_sift; create extension pg_sift_test;")
    with concurrent.futures.ThreadPoolExecutor(3 * len(lanes)) as ex:
        list(ex.map(lambda x: script(x[0], BUILD + fill(FIXTURES, x[1][0], x[1][1])), [(e, (s, n)) for e in envs for (s, _), n in zip(SCALES, ns)]))
    for e in envs:
        for (s, _), n in zip(SCALES, ns):
            if any(c.name == "create_f2_tbl_copy" for c in cases):
                script(e, f"copy (select {ROWS} from base_{s}) to '{dump_dir}/copy_{s}.bin' (format binary);")
    results = {}

    def run_lane(lane, e):
        ctl = Ctl(e)
        for c in lane:
            results[c.name] = run_case(c, (e, ctl), args, ns, dump_dir, pm)
        ctl.close()

    with concurrent.futures.ThreadPoolExecutor(len(lanes)) as ex:
        for f in [ex.submit(run_lane, lane, e) for lane, e in zip(lanes, envs)]:
            f.result()
    rows = [cells_of(results[c.name]) for c in cases]
    print("GATE_HEADER " + row_text(HEADER))
    for r in rows:
        print("GATE_ROW " + row_text(r))
    for db in dbs:
        script(env, f"drop database if exists {db} with (force);", strict=False)
    admin.close()
    for (s, _) in SCALES:
        if os.path.exists(f"{dump_dir}/copy_{s}.bin"):
            os.unlink(f"{dump_dir}/copy_{s}.bin")
    if not args.dir:
        shutil.rmtree(dump_dir, ignore_errors=True)
    if args.summary_tsv:
        with open(args.summary_tsv, "w") as f:
            f.write("\n".join("\t".join(r) for r in [HEADER, *rows]) + "\n")
    return 1 if any(results[c.name].fails for c in cases) else 0


def selftest():
    c = Case("x", "C", 2, "zero", 12, ())
    ns = [32768, 65536, 131072]
    flat = [Trial(5 * MIB, 2 * MIB, 0, 0, 1.0), Trial(5 * MIB, 2 * MIB, 0, 0, 2.0), Trial(5.2 * MIB, 2 * MIB, 0, 0, 4.2)]
    fails, alpha, bex, brss, atime = judge(c, flat, ns, [0, 0, 0])
    assert fails == [] and alpha < 1.15 and bex < 1 and brss < 8 and atime < 5.5, (fails, alpha)
    linear = [Trial(26 * MIB, 0, 0, 0, 1.0), Trial(52 * MIB, 0, 0, 0, 2.0), Trial(104 * MIB, 0, 0, 0, 4.0)]
    fails, alpha, bex, brss, _ = judge(c, linear, ns, [0, 0, 0])
    assert bex is None and round(alpha, 2) == 4.0 and brss > 780 and len(fails) == 3 and fails[0].startswith("α_mem=4.00"), fails
    constant_over = [Trial(40 * MIB, 0, 0, 0, 1.0)] * 3
    assert [f.split("=")[0] for f in judge(c, constant_over, ns, [0, 0, 0])[0]] == ["cap"]
    tracked = [Trial(6 * MIB, 1 * MIB, 0, 0, 1.0), Trial(6 * MIB, 2 * MIB, 0, 0, 1.0), Trial(6 * MIB, 4 * MIB, 0, 0, 1.0)]
    assert any(f.startswith("β_exact") for f in judge(c, tracked, ns, [0, 0, 0])[0])
    assert judge(c, tracked, ns, [0, 0, 0], skip_rss=True)[0][0].startswith("α_mem=")
    below = [Trial(0.5 * MIB, 0, 0, 0, 0.01), Trial(0.6 * MIB, 0, 0, 0, 0.02), Trial(0.9 * MIB, 0, 0, 0, 0.04)]
    assert judge(c, below, ns, [0, 0, 0])[0] == [], "under the noise floors a flat path does not fail by ratio"
    spilled = [Trial(5 * MIB, 0, 3, 0, 1.0), Trial(5 * MIB, 0, 0, 2, 1.0), Trial(5 * MIB, 0, 0, 0, 1.0)]
    assert [f.split("=")[0] for f in judge(c, spilled, ns, [0, 0, 0])[0]] == ["temp_bytes", "left"]
    cap = c._replace(temp="cap")
    grown = [Trial(5 * MIB, 0, 1 * MIB, 0, 1.0), Trial(5 * MIB, 0, 5 * MIB, 0, 1.0), Trial(5 * MIB, 0, 12 * MIB, 0, 1.0)]
    assert [f.split("=")[0] for f in judge(cap, grown, ns, [10 * MIB] * 3)[0]] == ["temp_bytes", "α_tmp"]
    assert judge(cap, grown[:1] * 3, ns, [10 * MIB] * 3)[0] == []
    slow = [Trial(5 * MIB, 0, 0, 0, 0.01), Trial(5 * MIB, 0, 0, 0, 0.1), Trial(5 * MIB, 0, 0, 0, 0.3)]
    assert judge(c, slow, ns, [0, 0, 0])[0] == ["α_time=6.00>5.5"], judge(c, slow, ns, [0, 0, 0])[0]
    crash = [Trial(5 * MIB, 0, 0, 0, 1.0, "backend lost: crash"), Trial(0, 0, 0, 0, 0, "not run"), Trial(0, 0, 0, 0, 0, "not run")]
    assert judge(c, crash, ns, [0, 0, 0])[0] == ["error: backend lost: crash"]
    assert cells_of(Result(c, crash, ["error: backend lost: crash"], dict(alpha=0.0, beta_ex=None, beta_rss=0.0, a_time=0.0, skip=False)))[1:7] == ["5.0MB (ex:untrk)", "-", "-", "-", "-", "-"]
    errored = [Trial(5 * MIB, 0, 0, 0, 1.0, "ERROR: x")] * 3
    assert judge(c, errored, ns, [0, 0, 0])[0] == ["error: ERROR: x"]
    lanes = lanes_of([c._replace(name=str(i), weight=w) for i, w in enumerate((8, 7, 4, 4, 3, 2))], 2)
    assert sorted(x.name for lane in lanes for x in lane) == ["0", "1", "2", "3", "4", "5"] and abs(sum(x.weight for x in lanes[0]) - sum(x.weight for x in lanes[1])) <= 2
    r = Result(c, flat, [], dict(alpha=1.03, beta_ex=0.0, beta_rss=1.0, a_time=4.2, skip=False))
    assert cells_of(r) == ["x", "5.0MB (ex:2.0MB)", "5.0MB (ex:2.0MB)", "5.2MB (ex:2.0MB)", "1.03", "0.0", "1.0", "0 B", "-", "4.20", "PASS"], cells_of(r)
    assert cells_of(r._replace(fails=["cap=9.0MB>8.0MB"]))[-1] == "FAIL [cap=9.0MB>8.0MB]"
    names = [x.name for x in CASES]
    assert len(names) == len(set(names)) == 27, len(names)
    assert collections.Counter(x.cls for x in CASES) == {"A": 6, "B": 4, "C": 8, "D": 8, "E": 1}
    assert all(x.temp in ("zero", "cap") and x.kind in ("scale", "av", "disk", "cleanup") for x in CASES)
    assert all(bool(x.ops) == (x.kind == "scale") for x in CASES), "a case measured at three sizes has operations, the others run their own"
    assert fill("{s} {n} {dump}", "2x", 5, "/d") == "2x 5 /d" and fill("'{k}'", "1x", 1) == "'{k}'"
    assert updates(32768).count("\n") == 99
    print("prod-viability-gate selftest: ok")
    return 0


def main():
    if "--selftest" in sys.argv:
        sys.exit(selftest())
    ap = argparse.ArgumentParser()
    ap.add_argument("cmd", choices=("run", "list"))
    ap.add_argument("--scale-base", type=int, default=int(os.environ.get("PROD_GATE_ROWS", "32768")))
    ap.add_argument("--only")
    ap.add_argument("--summary-tsv")
    ap.add_argument("--dir")
    ap.add_argument("--av-host")
    ap.add_argument("--av-port", type=int)
    ap.add_argument("--av-log")
    ap.add_argument("--no-rss", action="store_true")
    args = ap.parse_args()
    if args.cmd == "list":
        print("\n".join(c.name for c in rows_of(args.only)))
        return 0
    return run(args)


if __name__ == "__main__":
    sys.exit(main())
