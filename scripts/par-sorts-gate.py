#!/usr/bin/env python3
"""The bounded-memory gate of the sorts, the parallel aggregate and the scans that spill (test/par-sorts.tsv), at 1x, 2x and 4x rows.

Every size is measured in a backend of its own (the way scripts/mem-slope.py does: the table is built by another session, the measured backend samples its own anonymous memory with
sift._test_rss_peak_start, and sift._test_mem_highwater() gives the bytes the memory scopes of sift counted). A case is the statement, its settings and the rows of its 1x table; the
columns of its verdict are

  peak_1x peak_2x peak_4x   the peak anonymous memory of the backend (MB) at 1x, 2x and 4x rows (the least of RUNS runs)
  a_mem                      peak_4x / peak_1x: what a bounded operation keeps near 1 (at most 1.15)
  b_exact                    bytes a row of the growth of the bytes the memory scopes of sift counted (at most 2)
  b_rss                      bytes a row of the growth of the peak (at most 8)
  temp                       the most bytes pg_ls_tmpdir() held at once at 4x, and tmp_x the multiple of the bytes the statement spills (at most 2.5)
  a_time                     time(4x) / time(1x)
  cap                        peak_4x above the start of the backend at most 2.5 x the hash memory + FIXED_BASE_MB: nothing of the statement is allowed to follow the rows or its own settings
  files_at_once              the most files pg_ls_tmpdir() held at once (at most FILES_MAX)
  files                      the files pg_ls_tmpdir() holds after the statement (none), and the leak warnings it raised (none)

  scripts/par-sorts-gate.py run [--only RE] [--runs N] [--table-only]   one CASE line a case (cases of kind `mem`: the columns above; kind `files`: the leftovers and warnings)
  scripts/par-sorts-gate.py list [--only RE]

Reads PGHOST, PGPORT, PGUSER, PGDATABASE like psql. TSV columns: NAME KIND ROWS FIXTURE HASH_KB TMP_BYTES_PER_ROW SETTINGS OPERATION.
"""
import importlib.util, os, re, subprocess, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("memslope", os.path.join(HERE, "mem-slope.py"))
ms = importlib.util.module_from_spec(spec)
spec.loader.exec_module(ms)

TSV = os.environ.get("PAR_SORTS_TSV") or os.path.join(HERE, "..", "test", "par-sorts.tsv")
SCALES = (1, 2, 4)
ALPHA_MAX, B_EXACT_MAX, B_RSS_MAX, TMP_X_MAX = 1.15, 2.0, 8.0, 2.5
FIXED_BASE_MB = 4.0
FILES_MAX = 192					# the files at once of a spilling parallel aggregate: a file for each of its 64 partitions and each of its 3 participants
CAP_X = 2.5
RUNS = int(os.environ.get("PAR_SORTS_RUNS", "2"))
LEAK = re.compile(r"leak|still pinned|reference leak|resource", re.I)

FIXTURES = {
    "none": "",
    "p": """
drop table if exists p{n} cascade;
create table p{n} as select g id, g k_hi, g % 5000 k_mid, g % 10 k_lo, 'name' || (g::bigint * 7919) % 1000000 txt, (g % 1000)::numeric amount from generate_series(1, {n}) g;
alter table p{n} add primary key (id);
create index p{n}_col on p{n} using sift_col (k_hi, k_mid, k_lo, txt, amount);
vacuum analyze p{n};
""",
    "facts": """
drop table if exists facts{n} cascade;
create table facts{n} (id int primary key, dim_a_id int, bucket int, doc jsonb) with (autovacuum_enabled = false);
insert into facts{n}
select i, case when i % 97 = 0 then null else i % 50 end, i % 2,
       jsonb_build_object('items', (select jsonb_agg(jsonb_build_object('k', (array['a', 'b', 'c'])[1 + (i + j) % 3], 'n', (i * j) % 10)) from generate_series(1, 1 + i % 4) j))
from generate_series(1, {n}) i;
create index facts{n}_col on facts{n} using sift_col (id, dim_a_id, bucket, doc) with (paths = 'doc.items[]{{k text, n float8}}');
vacuum (freeze, analyze) facts{n};
""",
    "docs": """
drop table if exists docs{n}, args{n} cascade;
create table docs{n} (id int primary key, tenant int not null, amount int not null, body text not null) with (autovacuum_enabled = false);
insert into docs{n} select i, i % 300, i % 100, concat_ws(' ', case when i % 2 = 0 then 'common' end, case when i % 20 = 0 then 'mid' end, 'tok' || i % 13) from generate_series(1, 30000) i;
create index docs{n}_text on docs{n} using sift (body);
create index docs{n}_col on docs{n} using sift_col (id, tenant, amount);
create table args{n} as select g % 300 n, case when g % 2 = 0 then 'common' else 'mid' end w from generate_series(1, {n}) g;
vacuum analyze docs{n};
analyze args{n};
""",
    "star": """
drop table if exists f{n}, d{n} cascade;
create table f{n} as select g id, g % {n} + 1 foreign_id, g % 10 k_lo from generate_series(1, 100000) g;
create table d{n} as select g id, g % 7 cls from generate_series(1, {n}) g;
alter table d{n} add primary key (id);
create index d{n}_col on d{n} using sift_col (id, cls);
create index f{n}_col on f{n} using sift_col (id, foreign_id, k_lo);
vacuum analyze f{n}; vacuum analyze d{n};
""",
}


class Case:
    def __init__(self, line):
        p = (line.rstrip("\n").split("\t") + [""] * 8)[:8]
        self.name, self.kind, self.rows, self.fixture, self.hash_kb, self.tmp_row, self.settings, self.op = p[0], p[1], int(p[2]), p[3], int(p[4]), int(p[5] or 0), p[6], p[7]


def read_cases(only):
    cases = []
    for l in open(TSV, encoding="utf-8").read().split("\n"):
        if l.strip() and not l.startswith("#"):
            c = Case(l)
            if only is None or re.search(only, c.name):
                cases.append(c)
    return cases


def build(c, n):
    ms.script(ms.BUILD_SETTINGS + FIXTURES[c.fixture].format(n=n))


def stmt(c, n):
    return c.settings.replace("\\n", "\n") + "\n" + c.op.replace("\\n", "\n").replace("{n}", str(n))


class TmpSampler(threading.Thread):
    """the most bytes and the most files pg_ls_tmpdir() holds while the statement runs (a second session asks every 20 ms)"""

    def __init__(self):
        super().__init__(daemon=True)
        self.stop_flag = False
        self.bytes = self.files = 0

    def run(self):
        p = ms.psql()
        while not self.stop_flag:
            out, _ = ms.send(p, "select 'TMP', coalesce(sum(size), 0), count(*) from pg_ls_tmpdir();\nselect pg_sleep(0.02);", "TMPSAMPLEDONE")
            for l in out:
                if l.startswith("TMP|"):
                    _, b, f = l.strip().split("|")
                    self.bytes, self.files = max(self.bytes, int(b)), max(self.files, int(f))
        p.stdin.write("\\q\n")
        p.wait()


def measure(c, n):
    """peak MB (absolute anonymous memory), bytes the scopes counted, seconds, the most tmp bytes and files at once, files left after, leak warnings"""
    sql = stmt(c, n)
    ms.prepare(sql)
    best = None
    for _ in range(RUNS):
        p = ms.psql()
        out, err = ms.send(p, ms.SETTINGS + "select sift._test_thp_disable();\nselect sift._test_malloc_stats(true);\n" + ms.PEAK_STOP + "\nselect 'BASE', sift._test_rss_peak_start();", "WARMDONE")
        if err:
            raise RuntimeError("warm: " + err)
        base = ms.num_of(out, "BASE")
        samp = TmpSampler()
        samp.start()
        t = time.time()
        out, err = ms.send(p, sql, "OPDONE")
        dt = time.time() - t
        samp.stop_flag = True
        samp.join()
        leaks = sum(1 for l in out if LEAK.search(l) and ("WARNING" in l or "ERROR" in l))
        if err and "temporary file size exceeds" not in err:
            raise RuntimeError(f"{c.name} {n}: {err}")
        post, _ = ms.send(p, ms.PEAK_STOP + "\nselect 'SCOPE', scope, peak from sift._test_mem_highwater() where parent is null;\n"
                          "select 'LEFT', count(*) from pg_ls_tmpdir();", "HWDONE")
        p.stdin.write("\\q\n")
        p.wait()
        scopes = sum(int(l.strip().split("|")[2]) for l in post if l.startswith("SCOPE|"))
        row = (ms.num_of(post, "PEAK") / 1024.0, scopes / 1048576.0, dt, samp.bytes, samp.files, ms.num_of(post, "LEFT"), leaks, base / 1024.0, err)
        best = row if best is None or row[0] < best[0] else best
    return best


def verdict_mem(c, rows, res):
    n1, n4 = rows[0], rows[-1]
    peak = [r[0] for r in res]
    exact = [r[1] for r in res]
    a_mem = peak[-1] / peak[0]
    b_exact = (exact[-1] - exact[0]) * 1048576.0 / (n4 - n1)
    b_rss = (peak[-1] - peak[0]) * 1048576.0 / (n4 - n1)
    tmp = res[-1][3]
    tmp_x = tmp / (n4 * c.tmp_row) if c.tmp_row else 0.0
    a_time = res[-1][2] / max(res[0][2], 1e-3)
    cap_mb = CAP_X * c.hash_kb / 1024.0 + FIXED_BASE_MB
    over = peak[-1] - res[-1][7]
    left = sum(r[5] for r in res)
    leaks = sum(r[6] for r in res)
    open_files = max(r[4] for r in res)
    fails = [k for k, bad in (("a_mem", a_mem > ALPHA_MAX), ("b_exact", b_exact > B_EXACT_MAX), ("b_rss", b_rss > B_RSS_MAX), ("tmp_x", tmp_x > TMP_X_MAX),
                              ("cap", over > cap_mb), ("open_files", open_files > FILES_MAX), ("files", left > 0), ("leaks", leaks > 0)) if bad]
    return (f"CASE {c.name} | {peak[0]:.1f} | {peak[1]:.1f} | {peak[2]:.1f} | a_mem {a_mem:.2f} | b_exact {b_exact:.1f} | b_rss {b_rss:.1f} | temp {tmp} tmp_x {tmp_x:.2f} | "
            f"a_time {a_time:.2f} | cap {over:.1f}/{cap_mb:.1f} | files_at_once {open_files} left {left} leaks {leaks} | " + ("PASS" if not fails else "FAIL " + ",".join(fails))), not fails


def verdict_files(c, rows, res):
    most_files = max(r[4] for r in res)
    left = sum(r[5] for r in res)
    leaks = sum(r[6] for r in res)
    fails = [k for k, bad in (("open_files", most_files > c.tmp_row), ("files", left > 0), ("leaks", leaks > 0)) if bad]
    return (f"CASE {c.name} | files_at_once {most_files} (at most {c.tmp_row}) | files {left} leaks {leaks} | " + ("PASS" if not fails else "FAIL " + ",".join(fails))), not fails


def wait_ready():
    """the server restarts after a backend died: the next case waits for it"""
    for _ in range(120):
        if "1" in ms.script("select 1;"):
            return
        time.sleep(0.5)
    raise RuntimeError("the server did not come back")


def run(only):
    ok = True
    for c in read_cases(only):
        rows = [c.rows * s for s in SCALES] if c.kind == "mem" else [c.rows]
        res = []
        failed = None
        for n in rows:
            build(c, n)
            try:
                res.append(measure(c, n))
            except (RuntimeError, StopIteration) as e:
                failed = f"{n} rows: {e if isinstance(e, RuntimeError) else 'the backend died'}"
                wait_ready()
                break
        if failed:
            print(f"CASE {c.name} | FAIL crash_or_error | {failed}", flush=True)
            ok = False
            continue
        line, good = (verdict_mem if c.kind == "mem" else verdict_files)(c, rows, res)
        print(line, flush=True)
        ok &= good
    return ok


def main():
    argv = sys.argv[1:]
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    if "--runs" in argv:
        global RUNS
        RUNS = int(argv[argv.index("--runs") + 1])
    if not argv or argv[0] not in ("run", "list"):
        sys.exit(__doc__)
    if argv[0] == "list":
        print("\n".join(c.name for c in read_cases(only)))
        return
    sys.exit(0 if run(only) else 1)


if __name__ == "__main__":
    main()
