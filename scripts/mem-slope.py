#!/usr/bin/env python3
"""The memory slope gate: what an operation holds against the rows of its table, measured on one backend at 3 sizes (test/mem-slope.tsv).

PostgreSQL's sorts, hashes and aggregates honour work_mem and its maintenance honours maintenance_work_mem, so the memory of a statement does not follow the table.
Each case runs in a new backend with work_mem and maintenance_work_mem at 1 MB, on tables of N, 2N and 4N rows; the peak of the anonymous resident memory of the
backend (sampled every 2 ms from /proc, the minimum of two runs) above what it held before the operation gives the slope in bytes a row between the first and the last size.
A case passes when the slope is at most its ceiling. A clean case has the ceiling of the noise (CLEAN); a case that grows with the data today has the slope it was measured at
(status `open`: a defect to fix, and its ceiling goes to CLEAN with the fix). A case whose slope is far under its open ceiling fails too, so that a fix is not left unrecorded.

  scripts/mem-slope.py run [--rows N] [--only RE]   one SLOPE line a case: SLOPE name kind bytes/row ceiling status PASS|FAIL
  scripts/mem-slope.py selftest

Reads PGHOST, PGPORT, PGUSER, PGDATABASE like psql; needs Linux (/proc) and an installed pg_sift. kind `leak` repeats a statement in one backend and asserts that the memory
its contexts hold (pg_backend_memory_contexts) does not follow the number of statements. Kind `cap` runs its statements in one backend with sift.backend_cache_kb at
the ceiling (kB) and asserts that the caches hold at most that; the same statements with the budget off must hold more, or the case measures nothing.
"""
import os, re, subprocess, sys, threading, time

CLEAN = 12                 # bytes a row: the noise of a 150,000-row difference is 1.8 MB
LEAK_KB = 512              # kB the contexts of a backend may hold more after 60 repeats than after 10
SETTINGS = "set work_mem='1MB'; set maintenance_work_mem='1MB'; set max_parallel_workers_per_gather=0; set max_parallel_maintenance_workers=0;\n"
TSV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test", "mem-slope.tsv")
os.environ["LC_ALL"] = "C"


def psql():
    return subprocess.Popen(["psql", "-X", "-q", "-A", "-t", "-v", "ON_ERROR_STOP=0"], stdin=subprocess.PIPE, stdout=subprocess.PIPE,
                            stderr=subprocess.STDOUT, text=True, bufsize=1)


def script(sql):
    p = psql()
    out, _ = p.communicate(sql)
    return out


def anon_kb(pid):
    try:
        for l in open(f"/proc/{pid}/status"):
            if l.startswith("RssAnon"):
                return int(l.split()[1])
    except FileNotFoundError:
        pass
    return 0


def peak_mb(setup, op):
    """a new backend: SETTINGS and setup, then op; the peak anonymous memory above what it held before op, in MB"""
    p = psql()
    p.stdin.write("select 'PID', pg_backend_pid();\n")
    line = p.stdout.readline()
    while "PID" not in line:
        line = p.stdout.readline()
    pid = int(line.strip().split("|")[1])
    p.stdin.write(SETTINGS + setup + "\n\\echo SETUPDONE\n")
    for l in iter(p.stdout.readline, ""):
        if "SETUPDONE" in l:
            break
        if "ERROR" in l:
            raise RuntimeError("setup: " + l.strip())
    base = anon_kb(pid)
    peak, stop = [base], threading.Event()

    def sample():
        while not stop.is_set():
            peak[0] = max(peak[0], anon_kb(pid))
            time.sleep(0.002)

    t = threading.Thread(target=sample)
    t.start()
    p.stdin.write(op + "\n\\echo OPDONE\n")
    err = None
    for l in iter(p.stdout.readline, ""):
        if "OPDONE" in l:
            break
        if "ERROR" in l:
            err = l.strip()
    stop.set()
    t.join()
    p.stdin.write("\\q\n")
    p.wait()
    if err:
        raise RuntimeError("op: " + err)
    return (peak[0] - base) / 1024.0


def build_tables(n):
    script(f"""
drop table if exists f{n}, d{n} cascade;
create table f{n} as
select g as id, g as k_hi, g % 5000 as k_mid, g % 10 as k_lo, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8] as cat,
       'name' || (g * 7919) % 1000000 as txt, ((g * 7919) % 29500)::numeric / 100 as amount, (g * 7919 % 29500)::float8 / 100 as fl,
       timestamp '2025-01-01' + (g * 311 % 31536000) * interval '1 second' as ts,
       (array['wireless','portable','compact','premium','rugged','slim','smart','classic'])[1 + g % 8] || ' ' ||
       (array['charger','speaker','headphones','keyboard','lamp','monitor','camera'])[1 + (g / 8) % 7] || ' w' || (g % 20000) as title
from generate_series(1, {n}) g;
alter table f{n} add primary key (id);
create index f{n}_col on f{n} using sift_col (k_hi, k_mid, k_lo, cat, txt, amount, fl, ts);
create index f{n}_bm on f{n} using sift (title);
vacuum analyze f{n};
create table d{n} as select g as id, g % 7 as cls, 'dim' || g as label from generate_series(1, {n}) g;
alter table d{n} add primary key (id);
create index d{n}_col on d{n} using sift_col (id, cls);
vacuum analyze d{n};
""")


def copy_table(n):
    script(f"""
drop table if exists m{n}, s{n} cascade;
create table m{n} as select * from f{n};
alter table m{n} add primary key (id);
create index m{n}_col on m{n} using sift_col (k_hi, k_mid, k_lo, cat, txt, amount, fl, ts);
create index m{n}_bm on m{n} using sift (title);
vacuum analyze m{n};
""")


def read_cases(only):
    cases = []
    for l in open(TSV, encoding="utf-8").read().split("\n"):
        if not l.strip() or l.startswith("#"):
            continue
        p = l.split("\t")
        name, kind, ceiling, status, setup, op = p[0], p[1], int(p[2]), p[3], p[4].replace("\\n", "\n"), p[5].replace("\\n", "\n")
        if only is None or re.search(only, name):
            cases.append((name, kind, ceiling, status, setup, op))
    return cases


def verdict(slope, ceiling, status):
    if status == "open":
        return "PASS" if CLEAN < ceiling and slope <= ceiling and slope > ceiling * 0.4 else "FAIL"
    return "PASS" if slope <= ceiling else "FAIL"


def slope_case(sizes, name, setup, op):
    peaks = []
    for n in sizes:
        fmt = dict(f=f"f{n}", d=f"d{n}", m=f"m{n}", s=f"s{n}", n=n)
        runs = []
        for _ in range(2):
            if "{m}" in setup + op or "{s}" in setup + op:
                copy_table(n)
            runs.append(peak_mb(setup.format(**fmt), op.format(**fmt)))
        peaks.append(min(runs))
    return (peaks[-1] - peaks[0]) * 1048576.0 / (sizes[-1] - sizes[0]), peaks


def leak_case(n, name, op):
    fmt = dict(f=f"f{n}", d=f"d{n}", n=n)
    sql = op.format(**fmt).rstrip(";")
    out = script(SETTINGS + f"""create temp table leakres(i int, kb bigint);
do $$ declare i int; begin for i in 1..60 loop execute $q${sql}$q$; if i in (10, 60) then insert into leakres select i, sum(total_bytes)/1024 from pg_backend_memory_contexts; end if; end loop; end $$;
select string_agg(i || ':' || kb, ' ' order by i) from leakres;""")
    m = re.search(r"10:(\d+) 60:(\d+)", out)
    if not m:
        raise RuntimeError("leak: " + out.strip()[-200:])
    return int(m.group(2)) - int(m.group(1))


CACHE_HELD = """with c as (select path[cardinality(path)] as id from pg_backend_memory_contexts
  where name in ('sift cache dictionaries', 'sift cache lookup tables', 'sift cache segments'))
select 'HELD', coalesce(sum(total_bytes), 0) / 1024 from pg_backend_memory_contexts m
where m.path[cardinality(m.path)] in (select id from c) or m.path && (select coalesce(array_agg(id), '{}') from c);"""


def cache_held_kb(n, op, budget_kb):
    fmt = dict(f=f"f{n}", d=f"d{n}", n=n)
    out = script(SETTINGS + f"set sift.backend_cache_kb = {budget_kb};\n" + op.format(**fmt) + "\n" + CACHE_HELD)
    m = re.search(r"HELD\|(\d+)", out)
    if not m:
        raise RuntimeError("cap: " + out.strip()[-200:])
    return int(m.group(1))



def leakx_case(n, name, setup, op):
    """one statement a transaction of its own (VACUUM, a merge) repeated 20 times in a backend: the memory of its contexts after the 20th over the 5th"""
    copy_table(n)
    fmt = dict(f=f"f{n}", d=f"d{n}", m=f"m{n}", s=f"s{n}", n=n)
    probe = lambda i: f"insert into leakres select {i}, sum(total_bytes)/1024 from pg_backend_memory_contexts;\ninsert into leakctx select {i}, name, sum(total_bytes)/1024 from pg_backend_memory_contexts group by 2;\n"
    steps = "".join(op.format(**fmt) + "\n" + (probe(i) if i in (5, 20) else "") for i in range(1, 21))
    out = script(SETTINGS + "create temp table leakres(i int, kb bigint); create temp table leakctx(i int, name text, kb bigint);\n"
                 + setup.format(**fmt) + "\n" + steps + "select string_agg(i || ':' || kb, ' ' order by i) from leakres;\n"
                 + "select 'GROWER ' || a.name || ' ' || (b.kb - a.kb) from leakctx a join leakctx b using (name) where a.i = 5 and b.i = 20 and b.kb > a.kb order by b.kb - a.kb desc limit 3;")
    print("   ", " ".join(re.findall(r"GROWER .*", out)).replace("GROWER ", "| "), flush=True)
    m = re.search(r"5:(\d+) 20:(\d+)", out)
    if not m:
        raise RuntimeError("leakx: " + out.strip()[-200:])
    return int(m.group(2)) - int(m.group(1))


def run(rows, only):
    sizes = [rows, rows * 2, rows * 4]
    cases = read_cases(only)
    if not os.path.exists("/proc/self/status"):
        print("SKIP needs /proc")
        return 0
    for n in sizes:
        build_tables(n)
    bad = 0
    for name, kind, ceiling, status, setup, op in cases:
        try:
            if kind == "cap":
                held, free = cache_held_kb(sizes[1], op, ceiling), cache_held_kb(sizes[1], op, -1)
                ok = held <= ceiling < free
                print(f"SLOPE {name} cap {held} kB held at a budget of {ceiling} kB, {free} kB without one {'PASS' if ok else 'FAIL'}", flush=True)
            elif kind == "leakx":
                grown = leakx_case(sizes[0], name, setup, op)
                limit = ceiling if status == "open" else LEAK_KB
                ok = grown <= limit and (status != "open" or grown > limit * 0.4)
                print(f"SLOPE {name} leak {grown} kB over 15 repeats, at most {limit} kB [{status}] {'PASS' if ok else 'FAIL'}", flush=True)
            elif kind == "leak":
                grown = leak_case(sizes[1], name, op)
                ok = grown <= LEAK_KB
                print(f"SLOPE {name} leak {grown} kB over 50 repeats, at most {LEAK_KB} kB {'PASS' if ok else 'FAIL'}", flush=True)
            else:
                slope, peaks = slope_case(sizes, name, setup, op)
                v = verdict(slope, ceiling, status)
                ok = v == "PASS"
                print(f"SLOPE {name} slope {slope:.0f} B/row (peaks {'/'.join(f'{p:.1f}' for p in peaks)} MB at {'/'.join(str(s) for s in sizes)} rows), ceiling {ceiling} [{status}] {v}", flush=True)
        except RuntimeError as e:
            ok = False
            print(f"SLOPE {name} {kind} ERROR {e}", flush=True)
        bad += not ok
    return 1 if bad else 0


def selftest():
    assert verdict(5, CLEAN, "clean") == "PASS"
    assert verdict(CLEAN + 1, CLEAN, "clean") == "FAIL"
    assert verdict(90, 140, "open") == "PASS"
    assert verdict(160, 140, "open") == "FAIL"
    assert verdict(10, 140, "open") == "FAIL"
    for c in read_cases(None):
        assert c[1] in ("slope", "leak", "leakx", "cap") and c[3] in ("clean", "open") and (c[1] == "cap" or (c[3] == "open") == (c[2] > CLEAN)), c[0]
    print("mem-slope selftest: ok")
    return 0


if __name__ == "__main__":
    if sys.argv[1:2] == ["selftest"]:
        sys.exit(selftest())
    rows = int(sys.argv[sys.argv.index("--rows") + 1]) if "--rows" in sys.argv else 50000
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    sys.exit(run(rows, only))
