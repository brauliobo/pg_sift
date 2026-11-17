#!/usr/bin/env python3
"""The legacy memory gate (the slope gate of scripts/mem-slope.py measures the kinds `slope` and `leak`; this one keeps the kinds it does not measure yet: leakx, flat, flatcut, cap, evict, pinned, and the idle series). The memory slope gate: what an operation holds against the rows of its table, measured on one backend at 3 sizes (test/mem-slope-legacy.tsv).

PostgreSQL's sorts, hashes and aggregates honour work_mem and its maintenance honours maintenance_work_mem, so the memory of a statement does not follow the table.
Each case runs in a new backend with work_mem and maintenance_work_mem at 1 MB, on tables of N, 2N and 4N rows; the peak of the anonymous resident memory of the
backend (sampled every 2 ms from /proc, the minimum of two runs) above what it held before the operation gives the slope in bytes a row between the first and the last size.
A case of kind `flat` (`flatcut`: the index cut by a key; `flat`: key_cut off, which also leaves out the sample of the heap that decides the key at every rewrite: a phase of its own, up to 400,000 rows) runs at 8, 16 and 32 times the rows (400,000, 800,000 and 1,600,000 by default) on a table of its own (one sift_col index, no search index), one run each, and
passes when the memory between the two largest sizes grows at most the ceiling in bytes a row and by at most 10%: the memory of an operation that is bounded by maintenance_work_mem is flat once the rows pass its budget.
A case passes when the slope is at most its ceiling. A clean case has the ceiling of the noise (CLEAN); a case that grows with the data today has the slope it was measured at
(status `open`: a defect to fix, and its ceiling goes to CLEAN with the fix; status `noise`: a case whose slope moves from run to run with where the allocator
places its blocks, with the largest of three runs plus 15 percent as its ceiling and no floor). A case whose slope is far under its open ceiling fails too, so that a fix is not left unrecorded.

  scripts/mem-slope-legacy.py run [--rows N] [--only RE]   one SLOPE line a case: SLOPE name kind bytes/row ceiling status PASS|FAIL
  scripts/mem-slope-legacy.py idle [--rows N] [--statements M] [--mode sift|off|plain] [--db NAME]   one backend runs the statements of test/idle-series.tsv in turn M times in all (12000): in the last 3000 its anonymous memory
                                                          sets no new high-water mark above IDLE_RSS_KB (2 MB) and the memory its contexts hold grows by under IDLE_CTX_KB in the last 1000 (a pooled backend is steady)
  scripts/mem-slope-legacy.py selftest

Reads PGHOST, PGPORT, PGUSER, PGDATABASE like psql; needs Linux (/proc) and an installed pg_sift. kind `leak` repeats a statement in one backend and asserts that the memory
its contexts hold (pg_backend_memory_contexts) does not follow the number of statements. Kind `cap` runs its statements in one backend with sift.backend_cache_kb at
the ceiling (kB) and asserts that the caches hold at most that; the same statements with the budget off must hold more, or the case measures nothing.
"""
import os, re, subprocess, sys, threading, time

CLEAN = 12                 # bytes a row: the noise of a 150,000-row difference is 1.8 MB
BOUNDED = "'sift cache unit buffers'"   # a fixed count of spare buffers (UNIT_KEEP of UNIT_KEEP_BYTES, col/seg/seg.c): it grows to its 16 MB while repeats find larger units, and no further
DEFAULT_BUDGET_KB = 256 * 1024   # sift.backend_cache_kb at its default: what a normal workload holds is what -1 holds
IDLE_RSS_KB = 2048        # kB over the highest anonymous memory of the first statements that an idle-series backend may reach in its last 3000: while caches fill and malloc touches the free space of its arena (a staircase of 1 to 13 MB, about 8000 statements) it climbs, then it moves inside a band
IDLE_CTX_KB = 1024        # kB its contexts may grow over the same statements
LEAK_KB = 512              # kB the contexts of a backend may hold more after 60 repeats than after 10
SETTINGS = f"set work_mem='1MB'; set maintenance_work_mem='{os.environ.get('MEM_SLOPE_MWM', '1MB')}'; set max_parallel_workers_per_gather=0; set max_parallel_maintenance_workers=0;\n"
TSV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test", "mem-slope-legacy.tsv")
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
    """a new backend: SETTINGS and setup (run in a backend of its own when it starts with "@other ": what the operation reads is then a table another session made), then op; the peak anonymous memory above what it held before op, in MB"""
    p = psql()
    p.stdin.write("select 'PID', pg_backend_pid();\n")
    line = p.stdout.readline()
    while "PID" not in line:
        line = p.stdout.readline()
    pid = int(line.strip().split("|")[1])
    if setup.startswith("@other "):
        script(setup[7:])
        setup = ""
    p.stdin.write(SETTINGS + "select sift._test_thp_disable();\n" + setup + "\n\\echo SETUPDONE\n")
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
       'name' || (g::bigint * 7919) % 1000000 as txt, ((g::bigint * 7919) % 29500)::numeric / 100 as amount, (g::bigint * 7919 % 29500)::float8 / 100 as fl,
       timestamp '2025-01-01' + (g::bigint * 311 % 31536000) * interval '1 second' as ts,
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


def flat_table(n):
    script(f"""
set client_min_messages=warning;
drop table if exists g{n} cascade;
create table g{n} as
select g as id, g as k_hi, g % 5000 as k_mid, g % 10 as k_lo, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8] as cat,
       'name' || (g::bigint * 7919) % 1000000 as txt, ((g::bigint * 7919) % 29500)::numeric / 100 as amount, (g::bigint * 7919 % 29500)::float8 / 100 as fl,
       timestamp '2025-01-01' + (g::bigint * 311 % 31536000) * interval '1 second' as ts from generate_series(1, {n}) g;
alter table g{n} add primary key (id);
create index g{n}_col on g{n} using sift_col (k_hi, k_mid, k_lo, cat, txt, amount, fl, ts);
vacuum analyze g{n};
""")


def flat_copy(n, cut):
    script(f"""
set client_min_messages=warning;
drop table if exists m{n} cascade;
create table m{n} as select * from g{n};
alter table m{n} add primary key (id);
create index m{n}_col on m{n} using sift_col (k_hi, k_mid, k_lo, cat, txt, amount, fl, ts) with (key_cut = '{cut}');
vacuum analyze m{n};
""")


def flat_case(sizes, setup, op, cut):
    peaks = []
    for n in sizes:
        flat_copy(n, cut)
        fmt = dict(m=f"m{n}", n=n)
        peaks.append(peak_mb(setup.format(**fmt), op.format(**fmt)))
    return (peaks[-1] - peaks[-2]) * 1048576.0 / (sizes[-1] - sizes[-2]), peaks


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
    if status == "noise":
        return "PASS" if slope <= ceiling else "FAIL"
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
  where name in ('sift cache dictionaries', 'sift cache lookup tables', 'sift cache segments', 'sift cache partition facts', 'sift cache value store view'))
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
    """one statement a transaction of its own (VACUUM, a merge) repeated 25 times in a backend: the memory of its contexts after the 25th over the 10th"""
    copy_table(n)
    fmt = dict(f=f"f{n}", d=f"d{n}", m=f"m{n}", s=f"s{n}", n=n)
    probe = lambda i: f"insert into leakres select {i}, sum(total_bytes)/1024 from pg_backend_memory_contexts where name not in ({BOUNDED});\ninsert into leakctx select {i}, name, sum(total_bytes)/1024 from pg_backend_memory_contexts where name not in ({BOUNDED}) group by 2;\n"
    steps = "".join(op.format(**fmt) + "\n" + (probe(i) if i in (10, 25) else "") for i in range(1, 26))
    out = script(SETTINGS + "create temp table leakres(i int, kb bigint); create temp table leakctx(i int, name text, kb bigint);\n"
                 + setup.format(**fmt) + "\n" + steps + "select string_agg(i || ':' || kb, ' ' order by i) from leakres;\n"
                 + "select 'GROWER ' || a.name || ' ' || (b.kb - a.kb) from leakctx a join leakctx b using (name) where a.i = 10 and b.i = 25 and b.kb > a.kb order by b.kb - a.kb desc limit 3;")
    print("   ", " ".join(re.findall(r"GROWER .*", out)).replace("GROWER ", "| "), flush=True)
    m = re.search(r"10:(\d+) 25:(\d+)", out)
    if not m:
        raise RuntimeError("leakx: " + out.strip()[-200:])
    return int(m.group(2)) - int(m.group(1))


HELD_BY = """select 'HELD ' || :'tag' || ' ' || coalesce(string_agg(n || '=' || kb, ' ' order by n), '') from (select c.name n,
  (select coalesce(sum(total_bytes), 0) / 1024 from pg_backend_memory_contexts m where m.path && array[c.id] or m.path[cardinality(m.path)] = c.id) kb
  from (select name, path[cardinality(path)] id from pg_backend_memory_contexts
        where name in ('sift cache dictionaries', 'sift cache lookup tables', 'sift cache segments')) c) x;"""
EVICT_ROWS = "select d.label, count(distinct f.txt), sum(f.amount) from {f} f join {d} d on d.id = f.k_hi group by 1 order by 1 limit 20;"


def held_by(n, a, b, budget_kb):
    fmt = dict(f=f"f{n}", d=f"d{n}", n=n)
    out = script(SETTINGS + f"set sift.backend_cache_kb = {budget_kb};\n" + a.format(**fmt) + "\n\\set tag A\n" + HELD_BY + "\n"
                 + b.format(**fmt) + "\n\\set tag B\n" + HELD_BY)
    got = {}
    for tag, rest in re.findall(r"HELD (\w) (.*)", out):
        got[tag] = {k.split()[-1]: int(v.split(".")[0]) for k, v in (kv.split("=") for kv in re.findall(r"sift cache \w+ ?\w*=\d+", rest))}
    if "A" not in got or "B" not in got:
        raise RuntimeError("evict: " + out.strip()[-200:])
    return got


def rows_at(n, budget_kb):
    fmt = dict(f=f"f{n}", d=f"d{n}", n=n)
    return script(SETTINGS + f"set sift.backend_cache_kb = {budget_kb};\n" + EVICT_ROWS.format(**fmt))


def evict_case(n, a, b):
    """statement a fills one cache, b another: with the budget off both stay; for some budget b makes a's cache give way to its own, and the rows a budget gives are those of none"""
    free = held_by(n, a, b, -1)
    kept = free["A"].get("tables", 0)
    if not (kept > 0 and free["B"].get("tables") == kept and free["B"].get("segments", 0) > 0):
        return False, "the budget off did not keep both caches"
    for pct in range(98, 80, -1):
        got = held_by(n, a, b, kept * pct // 100)
        if got["A"].get("tables", 0) > 0 and got["B"].get("tables", 0) < got["A"]["tables"] and got["B"].get("segments", 0) > 0:
            same = rows_at(n, 1024) == rows_at(n, -1) == rows_at(n, kept * pct // 100)
            return same, f"budget {kept * pct // 100} kB: lookup {got['A']['tables']} -> {got['B']['tables']} kB for segments {got['B']['segments']} kB; rows {'equal' if same else 'DIFFER'}"
    return False, "no budget made the segment cache take room from the lookup cache"


PIN_PRESSURE = ["select d.label, count(*) from {f} f join {d} d on d.id = f.k_hi group by 1 order by 2 desc, 1 limit 1;",
                "select count(distinct txt) from {f};", "select count(*) from {f} where title @@> 'portable speaker';"]


def pinned_case(n, budget_kb):
    """a cursor on a search holds the segment bytes it reads; with the budget far under what the caches would hold, statements that fill them do not take those bytes: the cursor returns all its rows"""
    fmt = dict(f=f"f{n}", d=f"d{n}")
    cur = "select id from {f} where title @@> 'wireless charger' order by id;".format(**fmt)
    count = script(SETTINGS + "select count(*) from {f} where title @@> 'wireless charger';".format(**fmt)).strip().splitlines()[-1]
    out = script(SETTINGS + f"set sift.backend_cache_kb = {budget_kb};\nbegin;\ndeclare c cursor for {cur[:-1]};\n\\echo FIRST\nfetch 10 from c;\n\\echo PRESSURE\n"
                 + "\n".join(q.format(**fmt) for q in PIN_PRESSURE) + "\n\\set tag P\n" + HELD_BY + "\n\\echo REST\nfetch all from c;\n\\echo END\ncommit;\n")
    first, rest = out.split("FIRST\n", 1)[1].split("PRESSURE")[0], out.split("REST\n", 1)[1].split("END")[0]
    nrows = sum(1 for r in (first + rest).splitlines() if r.strip().isdigit())
    held = re.search(r"HELD P .*sift cache segments=(\d+)", out)
    return nrows if held and int(held.group(1)) > 0 else -1, int(count)


def run(rows, only):
    sizes = [rows, rows * 2, rows * 4]
    flat = [rows * 8, rows * 16, rows * 32]
    cases = read_cases(only)
    if not os.path.exists("/proc/self/status"):
        print("SKIP needs /proc")
        return 0
    if any(not c[1].startswith("flat") for c in cases):
        for n in sizes:
            build_tables(n)
    if any(c[1].startswith("flat") for c in cases):
        for n in flat:
            flat_table(n)
    bad = 0
    for name, kind, ceiling, status, setup, op in cases:
        try:
            if kind.startswith("flat"):
                slope, peaks = flat_case(flat, setup, op, "k_mid" if kind == "flatcut" else "off")
                ok = verdict(slope, ceiling, status) == "PASS" and (status == "noise" or peaks[-1] <= peaks[-2] * 1.1)
                print(f"SLOPE {name} flat {slope:.0f} B/row between the two largest sizes (peaks {'/'.join(f'{p:.1f}' for p in peaks)} MB at {'/'.join(str(s) for s in flat)} rows), ceiling {ceiling} {'PASS' if ok else 'FAIL'}", flush=True)
            elif kind == "pinned":
                got, want = pinned_case(sizes[1], ceiling)
                ok = got == want > 0
                print(f"SLOPE {name} pinned a cursor kept its segments in the cache at a budget of {ceiling} kB while other statements filled it: {got} of {want} rows {'PASS' if ok else 'FAIL'}", flush=True)
            elif kind == "evict":
                ok, what = evict_case(sizes[1], setup, op)
                print(f"SLOPE {name} evict {what} {'PASS' if ok else 'FAIL'}", flush=True)
            elif kind == "cap":
                script(setup.format(f=f"f{sizes[1]}", d=f"d{sizes[1]}", n=sizes[1]))
                held, free, default = (cache_held_kb(sizes[1], op, b) for b in (ceiling, -1, DEFAULT_BUDGET_KB))
                ok = held <= ceiling < free and default == free
                print(f"SLOPE {name} cap {held} kB held at a budget of {ceiling} kB, {free} kB with -1 and {default} kB at the default {'PASS' if ok else 'FAIL'}", flush=True)
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


IDLE_TSV = os.path.join(os.path.dirname(os.path.abspath(__file__)), "..", "test", "idle-series.tsv")
PTC = """drop table if exists ptc cascade; create table ptc (id int, k int, cat text, amount numeric) partition by range (id);
select format('create table ptc_%s partition of ptc for values from (%s) to (%s)', g, g*2, (g+1)*2) from generate_series(0, 999) g \\gexec
insert into ptc select g, g % 100, 'c' || g % 7, g % 1000 from generate_series(0, 1999) g;
create index ptc_col on ptc using sift_col (k, cat, amount); vacuum analyze ptc;"""


PLAIN_TABLES = """create table f{n} as select g as id, g as k_hi, g % 5000 as k_mid, g % 10 as k_lo, (array['DE','FR','US','BR','JP','AR','CL','PE'])[1 + g % 8] as cat,
 'name' || (g::bigint * 7919) % 1000000 as txt, ((g::bigint * 7919) % 29500)::numeric / 100 as amount, (g::bigint * 7919 % 29500)::float8 / 100 as fl,
 timestamp '2025-01-01' + (g * 311 % 31536000) * interval '1 second' as ts,
 (array['wireless','portable','compact','premium','rugged','slim','smart','classic'])[1 + g % 8] || ' ' ||
 (array['charger','speaker','headphones','keyboard','lamp','monitor','camera'])[1 + (g / 8) % 7] || ' w' || (g % 20000) as title from generate_series(1, {n}) g;
alter table f{n} add primary key (id); create index f{n}_k on f{n} (k_mid, k_lo); create index f{n}_t on f{n} using gin (to_tsvector('simple', title)); vacuum analyze f{n};
create table d{n} as select g as id, g % 7 as cls, 'dim' || g as label from generate_series(1, {n}) g; alter table d{n} add primary key (id); vacuum analyze d{n};
create table m{n} as select * from f{n}; alter table m{n} add primary key (id); vacuum analyze m{n};"""
PLAIN_PTC = PTC.replace(" using sift_col (k, cat, amount)", " (k, cat)")


def idle_series(rows, total, mode, db):
    """one backend, the statements of test/idle-series.tsv in turn (mode sift; off: the sift statements with sift.enable_scan and sift.enable_agg off, minus those that call sift functions; plain: in a database
    without the extension, the forms of the PLAIN column): its anonymous memory, the memory of its contexts and (with the extension) what malloc has given out, every 50 statements"""
    n = rows * 2
    os.environ["PGDATABASE"] = "postgres"
    script(f"drop database if exists {db}; create database {db};")
    os.environ["PGDATABASE"] = db
    if mode == "plain":
        script("set client_min_messages=warning;\n" + PLAIN_TABLES.format(n=n) + "\n" + PLAIN_PTC)
    else:
        script("create extension pg_sift; create extension pg_sift_test;")
        build_tables(n)
        copy_table(n)
        script("set client_min_messages=warning;\n" + PTC)
    fmt = dict(f=f"f{n}", d=f"d{n}", m=f"m{n}", p="ptc")
    lines = [(l.rstrip("\n").split("\t") + ["", ""])[:3] for l in open(IDLE_TSV, encoding="utf-8") if l.strip() and not l.startswith("#")]
    pick = lambda l: (l[2] or l[1]) if mode == "plain" else l[1]
    skip = lambda l: l[2] == "-" and mode != "sift"
    setup = [pick(l).format(**fmt) for l in lines if l[0] == "setup"]
    stmts = [pick(l).format(**fmt) for l in lines if l[0] != "setup" and not skip(l)]
    p = psql()
    errors = []

    def q(sql):
        p.stdin.write(sql.rstrip().rstrip(";") + ";\n\\echo DONE\n")
        out = []
        for l in iter(p.stdout.readline, ""):
            if "DONE" in l:
                break
            out.append(l)
        r = "".join(out)
        if "ERROR" in r:
            errors.append(sql[:60] + ": " + r.strip()[:100])
        return r

    pid = int(q("select pg_backend_pid()").strip().splitlines()[-1])
    q("set client_min_messages=warning; set max_parallel_workers_per_gather=0; set work_mem='4MB'")
    if mode == "off":
        q("set sift.enable_scan = off; set sift.enable_agg = off")
    for st in setup:
        q(st)
    series = {}
    for i in range(1, total + 1):
        q(stmts[(i - 1) % len(stmts)])
        if i % 50 == 0:
            ctx = int(float(q("select sum(total_bytes) / 1024 from pg_backend_memory_contexts").strip().splitlines()[-1]))
            held = int(re.search(r"inuse=(\d+)", q("select sift._test_malloc_stats()")).group(1)) // 1024 if mode != "plain" else 0
            series[i] = (anon_kb(pid), ctx, held)
            q(f"insert into idle_snap select {i}, name, sum(total_bytes) / 1024 from pg_backend_memory_contexts group by 2")
            print(f"SERIES {mode} {i} {series[i][0]} {series[i][1]} {series[i][2]}", flush=True)
    a, b = total - 1000, total
    grow = lambda lo, hi, k: q(f"select string_agg(n || ' ' || d, '; ') from (select coalesce(y.name, x.name) n, coalesce(y.kb, 0) - coalesce(x.kb, 0) d from (select * from idle_snap where i = {lo}) x "
                               f"full join (select * from idle_snap where i = {hi}) y on x.name = y.name where coalesce(y.kb, 0) <> coalesce(x.kb, 0) order by 2 desc limit {k}) z").strip()
    growers = grow(a, b, 5)
    print(f"GROWERS 1000 -> {total - 1000} statements: {grow(1000, total - 1000, 8)}", flush=True)
    p.stdin.write("\\q\n")
    p.wait()
    band = lambda lo, hi: max(v[0] for i, v in series.items() if lo < i <= hi)
    early, late = band(0, total - 3000), band(total - 3000, total)
    rss, ctx = late - early, series[b][1] - series[a][1]
    ok = rss < IDLE_RSS_KB and ctx < IDLE_CTX_KB and not errors
    print(f"SLOPE idle_series idle {mode} {total} statements of {len(stmts)} kinds: RSS band maximum of the last 3000 is {rss:+d} kB over the earlier maximum (under {IDLE_RSS_KB}), contexts {ctx:+d} kB over the last 1000 "
          f"(under {IDLE_CTX_KB}); growers {growers}; {len(errors)} errors {errors[:2]} {'PASS' if ok else 'FAIL'}", flush=True)
    return 0 if ok else 1


def selftest():
    assert os.path.exists(IDLE_TSV)
    assert verdict(5, CLEAN, "clean") == "PASS"
    assert verdict(CLEAN + 1, CLEAN, "clean") == "FAIL"
    assert verdict(90, 140, "open") == "PASS"
    assert verdict(160, 140, "open") == "FAIL"
    assert verdict(10, 140, "open") == "FAIL"
    assert verdict(-80, 64, "noise") == "PASS" and verdict(65, 64, "noise") == "FAIL"
    for c in read_cases(None):
        assert c[1] in ("slope", "leak", "leakx", "flat", "flatcut", "cap", "evict", "pinned") and c[3] in ("clean", "open", "noise"), c[0]
        assert c[1] in ("cap", "evict", "pinned") or c[1].startswith("flat") or (c[3] == "clean") == (c[2] <= CLEAN), c[0]
    print("mem-slope selftest: ok")
    return 0


if __name__ == "__main__":
    if sys.argv[1:2] == ["selftest"]:
        sys.exit(selftest())
    rows = int(sys.argv[sys.argv.index("--rows") + 1]) if "--rows" in sys.argv else 50000
    only = sys.argv[sys.argv.index("--only") + 1] if "--only" in sys.argv else None
    if sys.argv[1:2] == ["idle"]:
        arg = lambda name, dflt: sys.argv[sys.argv.index(name) + 1] if name in sys.argv else dflt
        sys.exit(idle_series(rows, int(arg("--statements", 12000)), arg("--mode", "sift"), arg("--db", "idle_" + arg("--mode", "sift"))))
    sys.exit(run(rows, only))
