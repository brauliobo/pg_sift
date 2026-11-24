#!/usr/bin/env python3
"""The scale-growth gate of the creation, auxiliary maintenance, verification and storage-read paths of format major 2.

Every case is measured in a fresh backend at 1x, 2x and 4x rows of one synthetic table (8 to 10 times compressible text, jsonb and numerics), with a tight budget
(maintenance_work_mem 2 MB, work_mem 1 MB) over parts that were built with 64 MB. What the operation holds must not follow the table:

  alpha_mem    peak(4x) / peak(1x) of the resident memory of the backend, and of the memory scopes of sift when they count, each over a floor of its budget  <= 1.15
  beta_exact   bytes a row between 1x and 4x of the memory scopes of sift (when they count)                                                                  <= 2
  beta_rss     bytes a row between 1x and 4x of the resident memory                                                                                          <= 8
  alpha_time   time(4x) / max(time(1x), 0.05 s)                                                                                                              <= 5.5
  temp_bytes   pg_stat_database.temp_bytes the statement added, with temp_file_limit 1 MB: 0 and no file left in pgsql_tmp (heapallindexed: sorts that spill, bounded)
  alpha_disk   bytes a row of the relation a creation wrote at 4x over 1x                                                                                    <= 1.15

  test/t/aux_reads_scale.py [--rows N] [--only RE] [--selftest]       reads PGHOST, PGPORT, PGUSER, PGDATABASE like psql; needs Linux (/proc), pg_sift and pg_sift_test installed
"""
import os, re, shutil, subprocess, sys, tempfile, time
from typing import NamedTuple

ROWS = 32768                     # 4, 8 and 16 granules of 8192 rows: no step of a granule between the sizes
RUNS = int(os.environ.get("AUX_SCALE_RUNS", "2"))
MWM, WM = 2 * 1048576, 1048576
FLOOR_RSS, FLOOR_EX = 2 * 1048576, 1048576
ALPHA_MEM, BETA_EXACT, BETA_RSS, ALPHA_TIME, ALPHA_TMP, ALPHA_DISK = 1.15, 2.0, 8.0, 5.5, 5.0, 1.15
PEAK_STOP = "select 'PEAK', sift._test_rss_peak_stop();"
SETTINGS = f"""set sift.async_maintenance='off'; set work_mem='{WM // 1024}kB'; set maintenance_work_mem='{MWM // 1024}kB'; set max_parallel_workers_per_gather=0;
set max_parallel_maintenance_workers=0; set jit=off; set sift.shared_cache_kb=0; set sift.backend_cache_kb='1MB'; set sift.test_create_format1=off;"""
BUILD = "set sift.async_maintenance='off'; set maintenance_work_mem='64MB'; set max_parallel_maintenance_workers=0; set sift.test_create_format1=off;\n"
os.environ["LC_ALL"] = "C"


class Case(NamedTuple):
    name: str
    setup: str       # in a session of its own with 64 MB, {n} rows, {d} the directory of the files
    op: str          # the measured statement, in a fresh backend with the tight settings
    temp: str        # zero: no temporary file at all; bounded: sorts that spill, the bytes bounded by the rows
    budget: int      # the memory the statement may hold
    disk: str = ""   # the relation whose size a row is compared


BASE = """drop table if exists base_{n} cascade;
create table base_{n} (id bigint not null, k_hi bigint not null, k_mid int not null, k_lo int not null, cat text not null, txt text not null, label text not null, label_b text not null, label_c text not null, payload text not null,
  doc jsonb not null, amount numeric(10,2) not null, ts timestamp not null) with (autovacuum_enabled = off);
insert into base_{n} select g, g, g % 2048, g % 8, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8],
  'k' || ((g::bigint * 2654435761) & 1048575) || '_' || repeat('abcdefgh', 7), 'name' || (g::bigint * 7919) % 1000000, 'user' || (g::bigint * 104729) % 1000003, 'item' || (g::bigint * 1299709) % 999983,
  repeat('seg=' || lpad((g % 16)::text, 2, '0') || '|grp=' || lpad((g % 128)::text, 3, '0') || '|pad=0123456789abcdef|', 8),
  jsonb_build_object('k', g % 64, 'tag', 't' || (g % 32), 'msg', repeat('json_payload_', 8) || (g % 1024),
    'items', jsonb_build_array(jsonb_build_object('c', 'c' || (g % 8), 'q', (g % 50)::int), jsonb_build_object('c', 'c' || ((g + 1) % 8), 'q', ((g + 3) % 50)::int))),
  ((g::bigint * 7919) % 29500)::numeric / 100, timestamp '2025-01-01' + ((g % 86400) * interval '1 second')
from generate_series(1, {n}) g;
"""
HEAP = """drop table if exists h{n} cascade;
create table h{n} as select * from base_{n};
create index h{n}_col on h{n} using sift_col (id, k_hi, k_mid, k_lo, cat, txt, payload, amount, ts) with (key_order = 'id');
vacuum h{n};
"""
STORE = "set work_mem='128kB'; drop table if exists s{n}; create table s{n} using sift_table as select * from base_{n};\n"
PROJ = "select sift.create_projection('h{n}_col', 'p', '%s');"

CASES = [
    Case("create_copy", "drop table if exists t{n}; create table t{n} (like base_{n}) using sift_table; copy base_{n} to '{d}/b{n}.bin' (format binary);",
         "copy t{n} from '{d}/b{n}.bin' (format binary);", "zero", MWM, "t{n}"),
    Case("create_insert_select", "drop table if exists t{n}; create table t{n} (like base_{n}) using sift_table;", "insert into t{n} select * from base_{n};", "zero", MWM, "t{n}"),
    Case("create_ctas", "drop table if exists t{n};", "create table t{n} using sift_table as select * from base_{n};", "zero", MWM, "t{n}"),
    Case("tail_relocate", "drop table if exists h{n} cascade;\n"
         "create table h{n} as select id, k_hi, k_mid, k_lo, cat, txt, label, label_b, label_c, payload || payload || payload || payload as payload, amount, ts from base_{n};\n"
         "set maintenance_work_mem='1MB'; create index h{n}_col on h{n} using sift_col (id, k_hi, k_mid, k_lo, cat, txt, payload, amount, ts) with (key_order = 'id');",
         "select sift._test_f2_tail_relocate('h{n}_col');", "zero", MWM),
    Case("proj_order", HEAP, PROJ % "ORDER BY k_mid, id INCLUDE (cat, amount)", "zero", MWM),
    Case("proj_agg", HEAP, PROJ % "GROUP BY k_hi MEASURES count(*), sum(amount)", "zero", MWM),
    Case("check_keys", HEAP, "select count(*) from sift.check('h{n}_col', false);", "zero", MWM),
    Case("check_heapallindexed", HEAP, "set work_mem='64kB'; set maintenance_work_mem='128kB'; select count(*) from sift.check('h{n}_col', true);", "bounded", MWM),
    Case("scan_numeric", STORE, "select count(*), sum(k_lo), sum(amount) from s{n};", "zero", WM),
    Case("scan_wide", STORE, "select count(*), sum(length(label)), sum(length(label_b)), sum(length(label_c)), sum(length(txt)), sum(length(payload)), sum(length(doc->>'msg')) from s{n};", "zero", WM),
]


def psql():
    return subprocess.Popen(["psql", "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=True, bufsize=1)


def script(sql):
    out, _ = psql().communicate(sql)
    err = next((l for l in out.splitlines() if "ERROR" in l), None)
    if err:
        raise RuntimeError(err.strip())
    return out


def send(p, sql, marker):
    p.stdin.write(sql + f"\n\\echo {marker}\n")
    out = []
    for l in iter(p.stdout.readline, ""):
        if marker in l:
            break
        out.append(l)
    return out, next((l.strip() for l in out if "ERROR" in l), None)


def num(out, tag):
    return int(next(l for l in out if l.startswith(tag + "|")).strip().split("|")[1])


def temp_state():
    """pg_stat_database.temp_bytes (a backend reports it when it exits) and the files in pgsql_tmp"""
    out = script("select pg_stat_clear_snapshot();\nselect temp_bytes from pg_stat_database where datname = current_database();\nselect count(*) from pg_ls_tmpdir();")
    lines = [l for l in out.split("\n") if l.strip().lstrip("-").isdigit()]
    return int(lines[0]), int(lines[1])


class Run(NamedTuple):
    rss: int
    exact: int
    seconds: float
    temp: int
    files: int
    disk: int


def measure(case, n, d, temp_limit):
    fmt = dict(n=n, d=d)
    script(BUILD + case.setup.format(**fmt))
    t0, _ = temp_state()
    p = psql()
    out, err = send(p, SETTINGS + f"set temp_file_limit='{temp_limit}';\nselect sift._test_thp_disable();\nselect sift._test_malloc_stats(true);\n{PEAK_STOP}\nselect 'BASE', sift._test_rss_peak_start();", "READY")
    if err:
        raise RuntimeError("settings: " + err)
    base = num(out, "BASE")
    start = time.monotonic()
    _, err = send(p, case.op.format(**fmt), "OPDONE")
    seconds = time.monotonic() - start
    out, hw_err = send(p, PEAK_STOP + "\nselect 'SCOPE', scope, peak from sift._test_mem_highwater() where parent is null;", "HWDONE")
    p.stdin.write("\\q\n")
    p.wait()
    if err or hw_err:
        raise RuntimeError(f"{case.name} at {n} rows: {err or hw_err}")
    exact = sum(int(l.strip().split("|")[2]) for l in out if l.startswith("SCOPE|"))
    t1, files = temp_state()
    disk = int(script(f"select pg_relation_size('{case.disk.format(**fmt)}');").strip()) if case.disk else 0
    return Run((num(out, "PEAK") - base) * 1024, exact, seconds, t1 - t0, files, disk)


class Verdict(NamedTuple):
    alpha_mem: float
    beta_exact: float
    beta_rss: float
    alpha_time: float
    alpha_disk: float
    temp: int
    files: int
    failed: list


def judge(case, sizes, runs):
    """runs: the Run of each size, the least of the repeats for the peaks and the time"""
    rss, ex = [r.rss for r in runs], [r.exact for r in runs]
    floor_rss, floor_ex = max(case.budget, FLOOR_RSS), max(case.budget, FLOOR_EX)
    a_rss = max(rss[-1], floor_rss) / max(rss[0], floor_rss)
    a_ex = max(ex[-1], floor_ex) / max(ex[0], floor_ex)
    alpha = max(a_rss, a_ex) if ex[-1] > 0 else a_rss
    rows = sizes[-1] - sizes[0]
    beta_exact, beta_rss = (ex[-1] - ex[0]) / rows, (rss[-1] - rss[0]) / rows
    alpha_time = runs[-1].seconds / max(runs[0].seconds, 0.05)
    alpha_disk = (runs[-1].disk / sizes[-1]) / (runs[0].disk / sizes[0]) if case.disk and runs[0].disk else 0.0
    temp = max(r.temp for r in runs)
    files = max(r.files for r in runs)
    failed = []
    if alpha > ALPHA_MEM:
        failed.append(f"alpha_mem {alpha:.2f} > {ALPHA_MEM}")
    if ex[-1] > 0 and beta_exact > BETA_EXACT:
        failed.append(f"beta_exact {beta_exact:.1f} > {BETA_EXACT} B/row")
    if beta_rss > BETA_RSS:
        failed.append(f"beta_rss {beta_rss:.1f} > {BETA_RSS} B/row")
    if alpha_time > ALPHA_TIME:
        failed.append(f"alpha_time {alpha_time:.2f} > {ALPHA_TIME}")
    if alpha_disk > ALPHA_DISK:
        failed.append(f"alpha_disk {alpha_disk:.2f} > {ALPHA_DISK}")
    if case.temp == "zero" and temp != 0:
        failed.append(f"temp_bytes {temp} != 0")
    if case.temp == "bounded":
        a_tmp = max(runs[-1].temp, 1048576) / max(runs[0].temp, 1048576)
        if a_tmp > ALPHA_TMP or runs[-1].temp > 64 * sizes[-1]:
            failed.append(f"temp_bytes {runs[-1].temp} from {runs[0].temp} (alpha {a_tmp:.1f})")
    if files != 0:
        failed.append(f"{files} files left in pgsql_tmp")
    return Verdict(alpha, beta_exact, beta_rss, alpha_time, alpha_disk, temp, files, failed)


def mb(v):
    return f"{v / 1048576:.1f}"


def run_case(case, sizes, d):
    runs = []
    for n in sizes:
        r = [measure(case, n, d, "1MB" if case.temp == "zero" else "-1") for _ in range(RUNS)]
        runs.append(Run(min(x.rss for x in r), min(x.exact for x in r), min(x.seconds for x in r), max(x.temp for x in r), max(x.files for x in r), r[-1].disk))
    return runs, judge(case, sizes, runs)


def row(case, runs, v):
    peaks = " | ".join(f"{mb(r.rss)}/{mb(r.exact)}" for r in runs)
    return (f"{case.name} | {peaks} | {v.alpha_mem:.2f} | {v.beta_exact:.1f} | {v.beta_rss:.1f} | {v.temp} | {v.alpha_disk:.2f} | {v.alpha_time:.2f} | "
            f"{'PASS' if not v.failed else 'FAIL'}")


def main(rows, only):
    sizes = [rows, rows * 2, rows * 4]
    d = os.environ.get("AUX_SCALE_DIR") or tempfile.mkdtemp(prefix="aux-scale-")
    os.makedirs(d, exist_ok=True)
    os.chmod(d, 0o777)
    script(f"create extension if not exists pg_sift_test cascade;\n" + "".join(BASE.format(n=n) for n in sizes))
    print("GATE_HEADER case | peak_1x | peak_2x | peak_4x (rss/exact MB) | alpha_mem | beta_exact | beta_rss | temp_bytes | alpha_disk | alpha_time | verdict", flush=True)
    bad = 0
    try:
        for case in CASES:
            if only and not re.search(only, case.name):
                continue
            try:
                runs, v = run_case(case, sizes, d)
            except RuntimeError as e:
                print(f"GATE_ROW {case.name} | ERROR {e} | FAIL", flush=True)
                bad += 1
                continue
            print("GATE_ROW " + row(case, runs, v) + ("" if not v.failed else "   # " + "; ".join(v.failed)), flush=True)
            bad += bool(v.failed)
    finally:
        for f in os.listdir(d):
            if f.endswith(".bin"):
                os.unlink(os.path.join(d, f))
        if not os.environ.get("AUX_SCALE_DIR"):
            os.rmdir(d)
    return 1 if bad else 0


def selftest():
    c = Case("x", "", "", "zero", MWM)
    flat = [Run(5 << 20, 0, 0.1, 0, 0, 0), Run(5 << 20, 0, 0.2, 0, 0, 0), Run(5 << 20, 0, 0.4, 0, 0, 0)]
    assert not judge(c, [100, 200, 400], flat).failed
    grow = [Run(5 << 20, 1 << 20, 0.1, 0, 0, 0), Run(9 << 20, 3 << 20, 0.2, 0, 0, 0), Run(17 << 20, 7 << 20, 0.4, 0, 0, 0)]
    assert len(judge(c, [32768, 65536, 131072], grow).failed) >= 3
    quad = [Run(5 << 20, 0, 0.1, 0, 0, 0), Run(5 << 20, 0, 0.6, 0, 0, 0), Run(5 << 20, 0, 2.4, 0, 0, 0)]
    assert judge(c, [100, 200, 400], quad).failed == ["alpha_time 24.00 > 5.5"]
    tmp = [Run(5 << 20, 0, 0.1, 0, 0, 0), Run(5 << 20, 0, 0.2, 4096, 0, 0), Run(5 << 20, 0, 0.4, 0, 1, 0)]
    assert judge(c, [100, 200, 400], tmp).failed == ["temp_bytes 4096 != 0", "1 files left in pgsql_tmp"]
    assert judge(c._replace(temp="bounded"), [32768, 65536, 131072], [Run(5 << 20, 0, 0.1, 1 << 20, 0, 0), Run(5 << 20, 0, 0.2, 2 << 20, 0, 0), Run(5 << 20, 0, 0.4, 4 << 20, 0, 0)]).failed == []
    print("aux_reads_scale selftest: ok")
    return 0


if __name__ == "__main__":
    args = sys.argv[1:]
    if "--selftest" in args:
        sys.exit(selftest())
    arg = lambda name, default=None: args[args.index(name) + 1] if name in args else default
    sys.exit(main(int(arg("--rows", ROWS)), arg("--only")))
