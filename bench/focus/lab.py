"""Shared parts of the focused write and read benches (wamp.py, reads.py): one scratch cluster on tmpfs, a churn workload of
insert / update / soft-delete batches with VACUUM every sixth batch, and the helpers that wait for the maintenance workers.

Every run copies a base heap (`lab.py base CASE WORK`: plain PostgreSQL, no index, so one heap serves every build under test), creates the
index and settles it, then churns. The data of a batch is seeded by its number, so two builds see the same rows and the same updates.
Environment: FOCUS_ROWS (base rows, 200000), FOCUS_N (rows inserted by a batch, 2000), FOCUS_PORT (56350), FOCUS_SCRATCH (/dev/shm/sift-focus), FOCUS_GRACE (0.3).
Linux only (perf_event_open, pg_waldump); PostgreSQL 18 (pg_buffercache_evict_relation, extension_control_path).
"""
import bisect, ctypes, os, platform, re, shutil, struct, subprocess, sys, threading, time

SCRATCH = os.environ.get("FOCUS_SCRATCH", "/dev/shm/sift-focus")
PORT = int(os.environ.get("FOCUS_PORT", "56350"))
ROWS = int(os.environ.get("FOCUS_ROWS", "200000"))
N = int(os.environ.get("FOCUS_N", "2000"))
VACUUM_EVERY = 6
READS = 5   # read classes run by the reader session after every batch
VACUUM_GRACE = 1.0   # a VACUUM asks the workers for more than a statement does, and they start after their naptime
GRACE = float(os.environ.get("FOCUS_GRACE", "0.3"))   # seconds the WAL must stand still after the last worker step (more than sift.maintenance_naptime)
# 200,000 rows: a partition of 8 MB (a third of maintenance_work_mem) cuts the 8-9 MB index of the std case in about one piece, as the default 64 MB does for 500,000 rows
CONF = ["fsync = off", "shared_buffers = 64MB", "max_wal_size = 2GB", "min_wal_size = 128MB", "wal_keep_size = 64MB", "wal_level = replica",
        "full_page_writes = on", "wal_compression = off", "autovacuum = off", "jit = off", "checkpoint_timeout = 1d", "max_worker_processes = 16",
        "max_locks_per_transaction = 256", "maintenance_work_mem = 26MB", "log_timezone = 'UTC'", "sift.log_min_duration = 0",
        "sift.maintenance_naptime = 200"]
COLS = "id, tenant_id, event_date, foreign_id, parent_id, status, points, label, created_at, updated_at, deleted_at, doc"
EXTRA = [("m1", "int", "(random()*100)::int"), ("m2", "int", "(random()*1000000)::int"), ("m3", "bigint", "(random()*1e9)::bigint"),
         ("m4", "double precision", "random()"), ("m5", "numeric(12,2)", "round((random()*10000)::numeric, 2)"),
         ("m6", "smallint", "(random()*30)::int"), ("m7", "int", "(random()*50000)::int"), ("m8", "double precision", "random()*1000"),
         ("m9", "int", "(random()*5)::int"), ("m10", "bigint", "(random()*1e12)::bigint"),
         ("s1", "text", "(array['red','green','blue','grey'])[1 + (random()*3)::int]"), ("s2", "text", "'c' || (random()*200)::int"),
         ("s3", "text", "'region-' || (random()*40)::int"), ("s4", "text", "md5((random()*3000)::int::text)"),
         ("ts1", "timestamptz", "now() - (random()*1e7) * interval '1 second'"), ("flag", "boolean", "random() < 0.3")]
CASES = {"std": [], "wide": EXTRA}
INDEX_OPTS = "key_cut='tenant_id', paths='auto'"
# the ten read classes; the key window of a measured batch moves by rotate()
Q = {"point": "select count(*), sum(points) from facts where id = 123457",
     "pointx": "select count(*), sum(points) from facts where id = 123457",
     "key": "select count(*), sum(points) from facts where tenant_id = 123",
     "range": "select count(*), sum(points), max(event_date) from facts where tenant_id between 100 and 130 and event_date >= date '2025-06-01'",
     "top20": "select id, event_date from facts where tenant_id = 123 order by event_date desc limit 20",
     "top_off": "select id, event_date from facts where tenant_id = 123 order by event_date desc limit 20 offset 1000",
     "top_sub": "select id, event_date from facts where tenant_id in (%s) order by event_date desc limit 20" % ",".join(str(i) for i in range(100, 160)),
     "top_flt": "select id, event_date from facts where tenant_id = 123 and status = 1 and deleted_at is null order by event_date desc limit 20",
     "latest": "select t.id, x.event_date from generate_series(100, 199) t(id) cross join lateral "
               "(select event_date from facts where tenant_id = t.id order by event_date desc limit 1) x",
     "group": "select foreign_id, count(*), sum(points) from facts where tenant_id between 100 and 160 group by 1 order by 1"}
CLASSES = list(Q)
KEYS = [(re.compile(r"tenant_id = (\d+)"), lambda m, d: f"tenant_id = {int(m.group(1)) + d}"),
        (re.compile(r"between (\d+) and (\d+)"), lambda m, d: f"between {int(m.group(1)) + d} and {int(m.group(2)) + d}"),
        (re.compile(r"generate_series\((\d+), (\d+)\)"), lambda m, d: f"generate_series({int(m.group(1)) + d}, {int(m.group(2)) + d})"),
        (re.compile(r"in \(([\d,]+)\)"), lambda m, d: "in (" + ",".join(str(int(x) + d) for x in m.group(1).split(",")) + ")")]


def rotate(sql, delta):
    for rx, f in KEYS:
        sql = rx.sub(lambda m: f(m, delta), sql)
    return sql


def delta(b):
    return b * 53 % 400


def sh(*a, **k):
    return subprocess.run(a, check=True, text=True, capture_output=True, **k).stdout


def insert_sql(case, n, seed=None):
    ex = CASES[case]
    cols = "tenant_id, event_date, foreign_id, parent_id, status, points, label, body, created_at, updated_at, doc" + "".join(", " + e[0] for e in ex)
    body = "md5(g::text) || md5((g+1)::text)" if ex else "md5(g::text)"
    sel = ("1 + (random()*599)::int, date '2026-01-01' + (random()*300)::int, 1 + (random()*9)::int, (random()*50000)::int, (random()*3)::int,"
           " (random()*10)::int, 'l' || (random()*2000)::int, " + body + ", now(), now(),"
           " jsonb_build_object('score', round((random()*10)::numeric, 1), 'lang', (array['en','es','pt','fr'])[1 + (random()*3)::int],"
           " 'parts', jsonb_build_array(jsonb_build_object('k', (random()*5)::int, 'v', (random()*100)::int)))") + "".join(", " + e[2] for e in ex)
    seed = f"select setseed({seed}); " if seed is not None else ""
    return f"begin; {seed}insert into facts ({cols}) select {sel} from generate_series(1, {n}) g; commit;"


def update_sql(n, seed):
    return (f"select setseed({seed}); update facts set updated_at = now(), points = (random()*10)::int where id in "
            f"(select 1 + (random()*(select max(id) from facts))::bigint from generate_series(1, {n // 10}))")


def delete_sql(n, seed):
    return (f"select setseed({seed}); update facts set deleted_at = now(), updated_at = now() where id in "
            f"(select 1 + (random()*(select max(id) from facts))::bigint from generate_series(1, {n // 30})) and deleted_at is null")


def index_cols(case):
    return COLS + "".join(", " + e[0] for e in CASES[case])


class Cluster:
    def __init__(s, data, port, root=None):
        s.data, s.port, s.root = data, port, root
        s.sock, s.log = f"{SCRATCH}/sock-{port}", data + ".log"
        s.env = dict(os.environ, LC_ALL="C", PGHOST=s.sock, PGPORT=str(port), PGUSER="postgres", PGDATABASE="w")

    def conf(s):
        sharedir, pkglib = sh("pg_config", "--sharedir").strip(), sh("pg_config", "--pkglibdir").strip()
        lines = [f"port = {s.port}", "listen_addresses = ''", f"unix_socket_directories = '{s.sock}'"] + CONF
        lines += [x.strip() for x in os.environ.get("FOCUS_CONF", "").split(";") if x.strip()]
        if s.root:
            lines += [f"extension_control_path = '{s.root}{sharedir}:$system'", f"dynamic_library_path = '{s.root}{pkglib}:$libdir'"]
        base = open(s.data + "/postgresql.conf").read().split("\n# focus\n")[0]
        open(s.data + "/postgresql.conf", "w").write(base + "\n# focus\n" + "\n".join(lines) + "\n")

    def start(s):
        os.makedirs(s.sock, exist_ok=True)
        sh("pg_ctl", "-D", s.data, "-l", s.log, "-w", "start")

    def stop(s):
        sh("pg_ctl", "-D", s.data, "-m", "fast", "-w", "stop")

    def psql(s, sql):
        return subprocess.run(["psql", "-X", "-Atq", "-v", "ON_ERROR_STOP=1", "-c", sql], env=s.env, check=True, text=True, capture_output=True).stdout.strip()

    def psql_file(s, text):
        subprocess.run(["psql", "-X", "-q", "-v", "ON_ERROR_STOP=1"], input=text, env=s.env, check=True, text=True, capture_output=True)

    def status(s):
        return s.psql("select 'index_bytes=' || index_bytes || ' segs=' || segments || ' units=' || units || ' dirty_seg=' || dirty_segments"
                      " || ' buf_rows=' || buffer_rows || ' free=' || free_bytes from sift.col_index_status")

    def settings(s):
        """line 1 of every result file: the settings that are not the default, and the effective sift ones"""
        skip = "'port','unix_socket_directories','listen_addresses','extension_control_path','dynamic_library_path','DateStyle','TimeZone','application_name'"
        a = s.psql(f"select name || '=' || current_setting(name) from pg_settings where source not in ('default', 'override') "
                   f"and name not in ({skip}) and name not like 'lc\\_%' and name not in ('log_timezone', 'default_text_search_config') order by 1").splitlines()
        b = s.psql("load 'pg_sift'; select name || '=' || current_setting(name) from pg_settings where name like 'sift.%' and "
                   "(source <> 'default' or name in ('sift.async_maintenance', 'sift.col_auto_merge', 'sift.col_auto_merge_wal_pct', "
                   "'sift.col_auto_merge_partition_kb')) order by 1").splitlines()
        return "# settings: " + "; ".join(a + [x for x in b if x.startswith("sift.")])


def base(case, work):
    d = f"{work}/{case}"
    shutil.rmtree(d, ignore_errors=True)
    os.makedirs(d)
    sh("initdb", "--no-sync", "-D", d + "/data", "-A", "trust", "-U", "postgres", "-E", "UTF8", "--no-locale")
    cl = Cluster(d + "/data", PORT)
    cl.conf(); cl.start()
    sh("createdb", "-h", cl.sock, "-p", str(PORT), "-U", "postgres", "w")
    cl.psql_file("create table facts (id bigserial primary key, tenant_id int not null, event_date date not null, foreign_id int not null, parent_id int,"
                 " status smallint not null default 0, points int, label text, body text, created_at timestamptz not null default now(),"
                 " updated_at timestamptz not null default now(), deleted_at timestamptz, doc jsonb" + "".join(f", {e[0]} {e[1]}" for e in CASES[case]) + ");")
    ins = insert_sql(case, ROWS, 0.5).replace("now(), now(),", "now() - interval '30 days' + g * interval '1 second' / 20, now() - interval '30 days' + g * interval '1 second' / 20,")
    cl.psql_file(ins.replace("date '2026-01-01' + (random()*300)", "date '2024-01-01' + (random()*1000)") + "\nvacuum analyze facts;")
    cl.psql("checkpoint")
    cl.stop()
    keep = min(int(m.group(1), 16) << 32 | int(m.group(2), 16) for m in re.finditer(r"checkpoint(?:'s REDO)? location:\s+([0-9A-F]+)/([0-9A-F]+)", sh("pg_controldata", d + "/data"))) >> 24
    for f in os.listdir(d + "/data/pg_wal"):   # a clean shutdown needs the segments from its checkpoint on only; every run copies this directory
        if re.fullmatch(r"[0-9A-F]{24}", f) and int(f[8:16], 16) * 256 + int(f[16:], 16) < keep:
            os.remove(f"{d}/data/pg_wal/{f}")
    print("base ready", case, sh("du", "-sh", d + "/data").split()[0])


def attr_syscall():
    return {"x86_64": 298, "aarch64": 241}[platform.machine()]


class Sess:
    """a psql backend that stays open; an SQL error ends the run. counted() returns the user-space instructions the backend ran for a statement"""
    def __init__(s, cl, init=(), err=None):
        s.p = subprocess.Popen(["psql", "-AtX", "-q", "-v", "ON_ERROR_STOP=1"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=err, text=True, bufsize=1, env=cl.env)
        s.pid = int(s.ask("select pg_backend_pid()")[0])
        for q in init:
            s.ask(q)
        s.fd = None

    def ask(s, sql):
        s.p.stdin.write(sql + ";\nselect 'END';\n"); s.p.stdin.flush()
        out = []
        while True:
            l = s.p.stdout.readline()
            if l == "":
                raise RuntimeError(f"psql ended on: {sql[:120]}")
            if l.strip() == "END":
                return out
            out.append(l.rstrip())

    def instr(s):
        if s.fd is None:   # PERF_TYPE_HARDWARE / PERF_COUNT_HW_INSTRUCTIONS, user space only (exclude_kernel | exclude_hv), enabled at once
            attr = struct.pack("IIQQQQQ", 0, 128, 1, 0, 0, 0, (1 << 5) | (1 << 6)).ljust(128, b"\0")
            s.fd = ctypes.CDLL(None, use_errno=True).syscall(attr_syscall(), ctypes.c_char_p(attr), s.pid, -1, -1, 0)
            if s.fd < 0:
                raise OSError(ctypes.get_errno(), "perf_event_open (kernel.perf_event_paranoid > 2?)")
        return struct.unpack("Q", os.read(s.fd, 8))[0]

    def counted(s, sql):
        a = s.instr()
        out = s.ask(sql)
        return out, s.instr() - a

    def close(s):
        if s.fd is not None:
            os.close(s.fd)
        s.p.stdin.write("\\q\n"); s.p.stdin.close(); s.p.wait()


class Sampler:
    """the insert LSN every 5 ms: time -> LSN for the intervals of the workers' log lines, and the instant the WAL last moved"""
    def __init__(s, cl):
        s.T, s.L, s.last = [], [], time.time()
        s.p = subprocess.Popen(["psql", "-AtXq", "-F", "|"], stdin=subprocess.PIPE, stdout=subprocess.PIPE, text=True, bufsize=1, env=cl.env)
        s.p.stdin.write("select extract(epoch from clock_timestamp()), pg_current_wal_insert_lsn() - '0/0'::pg_lsn;\n\\watch 0.005\n"); s.p.stdin.flush()
        threading.Thread(target=s.read, daemon=True).start()

    def read(s):
        for l in s.p.stdout:
            m = re.match(r"^(\d+\.\d+)\|(\d+)$", l.strip())
            if not m:
                continue
            t, v = float(m.group(1)), int(m.group(2))
            if s.L and v != s.L[-1]:
                s.last = t
            s.T.append(t); s.L.append(v)

    # A worker writes its pages in bursts just before it logs, so a record is placed by the samples around the op, not by interpolation:
    # the LSN of the last sample before its start and of the first sample from its log line on
    def before(s, t):
        return s.L[max(bisect.bisect_right(s.T, t) - 1, 0)]

    def after(s, t):
        return s.L[min(bisect.bisect_left(s.T, t), len(s.L) - 1)]

    def stop(s):
        s.p.terminate(); s.p.wait()


# no worker step running and no VACUUM in progress
IDLE_SQL = ("select (select count(*) from pg_stat_progress_vacuum) || '|' || "
            "coalesce((select bool_or(running) from sift.worker_state('facts_sift'::regclass)), false)")


class Lab:
    """a scratch cluster with the churned table: setup() builds and settles the index, idle() waits for the workers, close() removes everything"""
    def __init__(s, case, work, root, out, port=PORT):
        s.case, s.out = case, out
        tag = os.path.basename(out)   # a run of its own directory: the cases of run.sh may run at once
        s.run, s.tbs = f"{work}/{case}/run-{tag}", f"{work}/{case}/ts-{tag}"
        for d in (s.run, s.tbs):
            shutil.rmtree(d, ignore_errors=True)
        sh("cp", "-a", f"{work}/{case}/data", s.run)
        os.makedirs(s.tbs)
        s.cl = Cluster(s.run, port, root)
        s.cl.conf(); s.cl.start()
        s.tf = open(out + ".txt", "w")
        s.err = open(out + ".stderr", "w")

    def rep(s, line):
        print(line, flush=True)
        s.tf.write(line + "\n"); s.tf.flush()

    def setup(s):
        cl = s.cl
        cl.psql("create extension if not exists pg_sift")
        s.settings = cl.settings()
        s.rep(s.settings)
        cl.psql(f"create tablespace sift_ts location '{s.tbs}'")
        s.sam, s.ctl = Sampler(cl), Sess(cl, err=s.err)
        cl.psql(f"create index facts_sift on facts using sift_col ({index_cols(s.case)}) with ({INDEX_OPTS}) tablespace sift_ts")
        prev = None
        for _ in range(10):
            cl.psql("vacuum facts"); s.idle(grace=2.0)
            cur = cl.status()
            if cur == prev:
                break
            prev = cur
        s.rep(f"# settled: {cl.status()}")
        s.size0 = int(cl.psql("select pg_relation_size('facts_sift'::regclass)"))
        cl.psql("checkpoint")

    def idle(s, kind="", grace=None, tmax=900):
        grace = grace or (VACUUM_GRACE if kind == "vacuum" else GRACE)
        t0 = time.time()
        while time.time() - t0 < tmax:
            time.sleep(0.05)
            if time.time() - s.sam.last >= grace and s.ctl.ask(IDLE_SQL)[0] == "0|false":
                return
        raise RuntimeError("workers did not settle")

    def batch(s, w, rd, b, ops):
        """one batch through the writer session w and the reader session rd; ops(kind, fn) runs each part (and waits) so that a bench can measure around it"""
        ops("insert", lambda: w.ask(insert_sql(s.case, N, (b * 4) / 1000)))
        ops("update", lambda: w.ask(update_sql(N, (b * 4 + 1) / 1000)))
        ops("delete", lambda: w.ask(delete_sql(N, (b * 4 + 2) / 1000)))

        def readloop():   # the scans that the read-priced maintenance rules see
            for q in [CLASSES[(READS * (b - 1) + i) % len(CLASSES)] for i in range(READS)]:
                if q == "pointx":
                    rd.ask("set enable_indexscan = off; set enable_bitmapscan = off")
                rd.ask(Q[q])
                if q == "pointx":
                    rd.ask("reset enable_indexscan; reset enable_bitmapscan")
        ops("read", readloop)
        if b % VACUUM_EVERY == 0:
            ops("vacuum", lambda: w.ask("vacuum facts"))

    def sessions(s):
        return (Sess(s.cl, err=s.err), Sess(s.cl, ["set max_parallel_workers_per_gather = 0", "set sift.enable_agg = on"], err=s.err))

    def index_size(s):
        return int(s.cl.psql("select pg_relation_size('facts_sift'::regclass)"))

    def close(s):
        s.sam.stop(); s.ctl.close()
        shutil.copy(s.cl.log, s.out + ".cluster.log")
        s.cl.stop(); s.tf.close(); s.err.close()
        shutil.rmtree(s.run, ignore_errors=True); shutil.rmtree(s.tbs, ignore_errors=True)

    def kill(s):
        subprocess.run(["pg_ctl", "-D", s.run, "-m", "immediate", "stop"], capture_output=True)


if __name__ == "__main__":
    if len(sys.argv) != 4 or sys.argv[1] != "base" or sys.argv[2] not in CASES:
        sys.exit("usage: lab.py base std|wide WORK")
    base(sys.argv[2], sys.argv[3])
