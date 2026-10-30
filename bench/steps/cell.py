#!/usr/bin/env python3
"""One measured cell of steps/articles.sh and steps/places.sh: pgbench runs in chunks until the cell is precise enough.

    python3 -I cell.py qps|latency [--rse X] [--min-time S] [--max-time S] [--min-n N] [--seed N] [--hint FILE] [--tmp DIR] [--xbench] -- PGBENCH_ARGS...

PGBENCH_ARGS are the arguments of pgbench without -t, -T, -l, --log-prefix and --random-seed (the clients are its -c); with --xbench
they are a whole command line of the load generator of the engines outside Postgres (`xbench http URL QUERIES --clients N ...`, steps/xlib.sh),
to which the chunk adds --repeat / --duration, --warmup 0, --seed and --txlog (bench/xbench), so Elasticsearch cells are measured and
stopped exactly like the pgbench ones. Every
transaction is logged (-l). A chunk is one pgbench run: a number of transactions per client (-t) when it is short and its clients
run many of them, whole seconds (-T, which keeps every client busy to the end) when it is long or slow. A chunk with many
transactions per client drops the first of each: a backend that has just started pays for its caches in its first query. The cell
stops once it has MIN_N transactions and MIN_TIME seconds and the relative standard error of its value is at most RSE, or once
MAX_TIME seconds are measured (a slow cell is limited by time, not by precision), whichever comes first after the minimums; the next
chunk is sized from the error so far (the error shrinks with the square root of the time). A cell that cannot reach RSE in MAX_TIME
(fewer than 1 / RSE^2 queries fit) is measured for MAX_TIME in one chunk.

  qps      value = transactions per second of all clients. The standard error is that of batch means: the transactions of every client
           are cut into consecutive batches, a batch's rate is the sum over the clients of their transactions per second in it, and the
           error is the standard deviation of the batch rates over the square root of their number (at least 4 batches), or, when
           there are fewer, that of the mean latency: sd / (mean * sqrt(n)), whichever is larger. Prints: qps, stderr, n, seconds, CPU ms per transaction, MB per transaction.
  latency  value = p50 and p95 of the transaction latency in ms. The standard error of the p50 is distribution free: half the
           distance between the order statistics n/2 -+ sqrt(n)/2 (the 68% interval of the median). Prints: p50, p95, stderr of p50, n,
           seconds, CPU ms per transaction, MB per transaction. n counts the transactions kept.

    python3 -I cell.py batch PLAN OUT [--rse X] [--min-time S] [--max-time S] [--min-n N] [--seed N] [--timeout S] [--retries N]

batch is the single-client latency cell of a Postgres engine without pgbench: one process opens a connection per (port, database) once and runs every cell of PLAN (one
`id<TAB>port<TAB>database<TAB>script` per line, in that order; script is a pgbench file with \\set NAME random(A, B), statements and \\gset) in a tight loop through libpq (ctypes),
the transaction latency is the same as pgbench's (from the first statement sent to the last result), seeded alike. A cell stops by the rule above, checked every ~20 ms of
work, so there is no process start, connection or log file per cell. Its OUT line is `id<TAB>p50<TAB>p95<TAB>se of p50<TAB>n<TAB>seconds<TAB>cpu<TAB>mb<TAB>noisy`; noisy is 1 when the host's
CPU or IO pressure stall time (/proc/pressure) over the window was above SIFT_QUIET_MAX percent (default 10): such cells are measured again after the batch, up to --retries
times, and stay 1 only when the last attempt was noisy too. Outliers are not removed: p50 and p95 are order statistics.

--server PID|PGDATA (with --db for Postgres) meters the server around the whole cell, the chunks the cell drops too (steps/meter.py): `NA` for both figures without it, and for the MB of a server with no buffer counter.
--hint FILE remembers the rate of the cell; the next round of the same cell needs no first chunk to size the others. A failed pgbench
prints 0 (qps) or nan (latency) like before, its error on stderr.
"""
import argparse, ctypes, ctypes.util, math, os, random, re, shutil, subprocess, sys, tempfile, time

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from stats import mean, sem  # noqa: E402  (steps/stats.py)
import benchlib  # noqa: E402  (steps/benchlib.py: the exit status of an interrupted run)
import meter  # noqa: E402  (steps/meter.py: the CPU and buffers of the server)

FEW = 10          # transactions per client that make a chunk worth a dropped first transaction and a count instead of seconds
MAX_CHUNKS = 12
BATCH = 5         # transactions of a client in a batch, at least


def figure(v):
    return 'NA' if v is None else f'{v:.4g}'


def pgbench_rows(log):
    """(client, transaction number, end time s, latency s) of the logs of pgbench -l --log-prefix=LOG/l"""
    for name in os.listdir(log):
        with open(os.path.join(log, name)) as f:
            for line in f:
                c, i, lat, _, sec, us = line.split()[:6]
                yield int(c), int(i), int(sec) + int(us) / 1e6, int(lat) / 1e6


def xbench_rows(log):
    """the same from the --txlog of xbench: `client seq latency_us end_us`"""
    with open(os.path.join(log, 'tx')) as f:
        for line in f:
            c, i, lat, end = line.split()
            yield int(c), int(i), int(end) / 1e6, int(lat) / 1e6


def run_chunk(argv, tmp, flag, amount, seed, xbench=False):
    """pgbench -t or -T (xbench --repeat or --duration): {client: [(end time s, latency s)]} in transaction order"""
    log = tempfile.mkdtemp(dir=tmp)
    try:
        if xbench:
            cmd = [*argv, '--repeat' if flag == '-t' else '--duration', str(amount), '--warmup', '0', '--seed', str(seed), '--txlog', os.path.join(log, 'tx')]
        else:
            cmd = ['pgbench', *argv, flag, str(amount), '-l', f'--log-prefix={log}/l', f'--random-seed={seed}']
        run = subprocess.run(cmd, capture_output=True, text=True)
        if run.returncode:
            raise RuntimeError(next((l for l in run.stderr.splitlines() if 'error' in l), f'{cmd[0]} exited with {run.returncode}'))
        clients = {}
        for c, i, end, lat in (xbench_rows if xbench else pgbench_rows)(log):
            clients.setdefault(c, []).append((i, end, lat))
        return {c: [(t, lat) for _, t, lat in sorted(rows)] for c, rows in clients.items()}
    finally:
        shutil.rmtree(log, ignore_errors=True)


def batches(clients):
    """one chunk: [(transactions, client seconds)] of its batches: the first transactions of every client, then the next, and so on;
    the seconds of a client in a batch run from the end of its transaction before to the end of its last transaction in it"""
    k = max(1, min(10, min(len(rows) for rows in clients.values()) // BATCH))
    out = [[0, 0.0] for _ in range(k)]
    for rows in clients.values():
        bounds = [len(rows) * j // k for j in range(k + 1)]
        for j in range(k):
            lo, hi = bounds[j], bounds[j + 1]
            if hi > lo:
                out[j][0] += hi - lo
                out[j][1] += rows[hi - 1][0] - (rows[lo - 1][0] if lo else rows[0][0] - rows[0][1])   # the first one starts when it ends minus its latency
    return [tuple(b) for b in out if b[0]]


class Cell:
    def __init__(self, mode, clients):
        self.mode, self.clients, self.lat, self.batches, self.est, self.total = mode, clients, [], [], 0.0, 0   # total: every transaction, the dropped ones too

    def add(self, clients, warm):
        """the transactions of a chunk, the first `warm` of every client dropped"""
        self.total += sum(map(len, clients.values()))
        kept = {c: rows[warm:] for c, rows in clients.items() if len(rows) > warm}
        if kept:
            self.lat += [lat for rows in kept.values() for _, lat in rows]
            self.batches += batches(kept)

    @property
    def n(self):
        return len(self.lat)

    @property
    def seconds(self):
        """the time measured: the client seconds over the clients"""
        return sum(s for _, s in self.batches) / self.clients

    @property
    def rate(self):
        """transactions per second of all clients, the clients' time pooled (a few slow transactions weigh what they last): measured,
        else the estimate of the first chunk or of the hint"""
        return self.clients * sum(n for n, _ in self.batches) / sum(s for _, s in self.batches) if self.batches else self.est

    def rel_se_batches(self):
        k, total, rate = len(self.batches), sum(s for _, s in self.batches), self.rate
        if k < 4:
            return 0.0
        return math.sqrt(sum((s * (self.clients * n / s - rate)) ** 2 for n, s in self.batches) / total ** 2 * k / (k - 1)) / rate if rate else math.inf

    def rel_se_mean_latency(self):
        return sem(self.lat) / mean(self.lat) if self.n > 1 else math.inf

    def p50(self):
        v = sorted(self.lat)
        return v[int((len(v) + 1) / 2) - 1]

    def p95(self):
        v = sorted(self.lat)
        return v[max(int(len(v) * 0.95), 1) - 1]

    def se_p50(self):
        v, n = sorted(self.lat), self.n
        if n < 2:
            return math.inf
        lo, hi = max(1, math.floor((n - math.sqrt(n)) / 2)), min(n, math.ceil((n + math.sqrt(n)) / 2))
        return (v[hi - 1] - v[lo - 1]) / 2

    def rel_se(self):
        if not self.n:
            return math.inf
        if self.mode == 'latency':
            return self.se_p50() / self.p50() if self.p50() else math.inf
        return max(self.rel_se_batches(), self.rel_se_mean_latency())


def next_chunk(cell, a):
    """(pgbench flag, amount, transactions dropped per client) of the next chunk, None when the cell is done"""
    rate, left = cell.rate, max(a.max_time - cell.seconds, 0.0)
    seconds = a.min_time
    if cell.n:
        rse = cell.rel_se()
        if cell.n >= a.min_n and cell.seconds >= a.min_time and (rse <= a.rse or not left):
            return None
        # the error shrinks with the square root of the time: the time still needed, at most three times what there is
        growth = 1.0 if rse == math.inf else min(3.0, (rse / a.rse) ** 2 - 1) if a.rse > 0 else 3.0
        seconds = max(min(max(a.min_time - cell.seconds, cell.seconds * growth), left), (a.min_n - cell.n) / rate, 0.02)
    if a.mode == 'qps' and (a.rse <= 0 or rate * a.max_time < 1 / a.rse ** 2):
        seconds = max(left, seconds)   # no precision within the time: one chunk of all of it, not a tail of idle clients per chunk
    each = seconds * rate / cell.clients   # transactions per client
    if cell.n >= a.min_n and each < 1:
        return None   # less than one more transaction per client would do: not worth a chunk
    warm = int(each >= FEW)
    if cell.clients > 1 and (seconds >= 1 or each < FEW):
        return '-T', max(1, round(seconds)), warm
    return '-t', max(1, math.ceil(each)) + warm, warm


# --- batch: the persistent libpq driver of the single-client latency cells
LIBPQ = (('PQconnectdb', ctypes.c_void_p, [ctypes.c_char_p]), ('PQstatus', ctypes.c_int, [ctypes.c_void_p]), ('PQerrorMessage', ctypes.c_char_p, [ctypes.c_void_p]),
         ('PQexec', ctypes.c_void_p, [ctypes.c_void_p, ctypes.c_char_p]), ('PQresultStatus', ctypes.c_int, [ctypes.c_void_p]),
         ('PQresultErrorMessage', ctypes.c_char_p, [ctypes.c_void_p]), ('PQnfields', ctypes.c_int, [ctypes.c_void_p]), ('PQntuples', ctypes.c_int, [ctypes.c_void_p]),
         ('PQfname', ctypes.c_char_p, [ctypes.c_void_p, ctypes.c_int]), ('PQgetvalue', ctypes.c_char_p, [ctypes.c_void_p, ctypes.c_int, ctypes.c_int]),
         ('PQclear', None, [ctypes.c_void_p]), ('PQfinish', None, [ctypes.c_void_p]))
COMMAND_OK, TUPLES_OK = 1, 2


def libpq():
    lib = ctypes.CDLL(ctypes.util.find_library('pq') or 'libpq.so.5')
    for name, res, args in LIBPQ:
        getattr(lib, name).restype, getattr(lib, name).argtypes = res, args
    return lib


VARIABLE = re.compile(r'(?<!:):([A-Za-z_]\w*)')


def parse_script(text):
    """a pgbench script as steps: ('set', name, lo, hi) for `\\set name random(lo, hi)`, ('sql', parts, gset) for a statement, gset when it ends in \\gset; parts alternates the text of the
    statement (bytes) and the names of its :variables (str), so that a transaction only joins them"""
    steps, buf = [], []

    def statement(lines, gset):
        pieces = VARIABLE.split('\n'.join(lines))
        steps.append(('sql', [p.encode() if i % 2 == 0 else p for i, p in enumerate(pieces)], gset))
    for line in text.splitlines():
        s = line.strip()
        if m := re.fullmatch(r'\\set\s+(\w+)\s+random\(\s*(\d+)\s*,\s*(\d+)\s*\)', s):
            steps.append(('set', m[1], int(m[2]), int(m[3])))
        elif s.endswith('\\gset'):
            statement(buf + [s[:-5].rstrip()], True); buf = []
        elif s:
            buf.append(s)
            if s.endswith(';'):
                statement(buf[:-1] + [buf[-1][:-1]], False); buf = []
    if buf:
        raise ValueError(f'statement without a terminating ; or \\gset: {buf[0][:60]}')
    return steps


def expand(parts, values):
    """the statement with :name replaced by the value of the variable (a name that is not set, and a cast ::type, stay as they are), what pgbench does"""
    return b''.join(p if p.__class__ is bytes else values.get(p) or b':' + p.encode() for p in parts)


class Connection:
    def __init__(self, lib, conninfo):
        self.lib, self.handle = lib, lib.PQconnectdb(conninfo.encode())
        if lib.PQstatus(self.handle):
            raise RuntimeError(lib.PQerrorMessage(self.handle).decode().strip())

    def run(self, sql, gset=False):
        """the statement through the simple protocol; the first row's columns as {name: bytes} when gset"""
        lib = self.lib
        res = lib.PQexec(self.handle, sql)
        try:
            if lib.PQresultStatus(res) not in (COMMAND_OK, TUPLES_OK):
                raise RuntimeError(lib.PQresultErrorMessage(res).decode().strip().splitlines()[0])
            if gset and lib.PQntuples(res):
                return {lib.PQfname(res, i).decode(): lib.PQgetvalue(res, 0, i) for i in range(lib.PQnfields(res))}
        finally:
            lib.PQclear(res)
        return None

    def close(self):
        self.lib.PQfinish(self.handle)


def transaction(conn, steps, rng, values):
    """seconds of one execution of the script: from the first statement sent to the last result"""
    t = time.perf_counter()
    for kind, a, b, *c in steps:
        if kind == 'set':
            values[a] = b'%d' % rng.randint(b, c[0])
        elif got := conn.run(expand(a, values), b):
            values.update(got)
    return time.perf_counter() - t


def pressure():
    """(cpu, io) microseconds of `some` stall time so far and the monotonic time, from /proc/pressure"""
    def total(name):
        with open(f'/proc/pressure/{name}') as f:
            return int(f.readline().rsplit('total=', 1)[1])
    return total('cpu'), total('io'), time.monotonic()


def noisy(before, after, limit):
    """whether the host stalled on CPU or IO for more than `limit` percent of the window (Pressure Stall Information, `some`)"""
    wall = (after[2] - before[2]) * 1e6
    return wall > 0 and max(after[0] - before[0], after[1] - before[1]) / wall * 100 > limit


def measure(conn, steps, a, seed, used=None):
    """(cell, noisy) of one cell on an open connection; cell.spent is the CPU ms and MB per transaction of the server `used` (a Meter) over it, the three first executions too"""
    rng, values, cell = random.Random(seed), {}, Cell('latency', 1)
    if used:
        used.start()
    for _ in range(3):
        transaction(conn, steps, rng, values)   # the statement's first executions: plan and caches
    before, t0, check = pressure(), time.monotonic(), 1
    while True:
        cell.lat += [transaction(conn, steps, rng, values) for _ in range(check)]
        elapsed = time.monotonic() - t0
        if elapsed >= a.max_time * 4 or (cell.n >= a.min_n and (elapsed >= a.max_time or (elapsed >= a.min_time and cell.rel_se() <= a.rse))):
            break
        check = max(1, int(0.02 / max(mean(cell.lat), 1e-6)))   # about 20 ms of work between two looks at the error
    cell.wall = time.monotonic() - t0
    if used:
        conn.run(b'SELECT pg_stat_force_next_flush()')   # the buffers of this session are counted when the next transaction ends
        used.stop()
    cell.spent = used.per(cell.n + 3) if used else (None, None)
    return cell, noisy(before, pressure(), float(os.environ.get('SIFT_QUIET_MAX', 10)))


def server(conn, port, db):
    """the Meter of the Postgres server behind a connection: its postmaster is the parent of the backend"""
    return meter.Meter(meter.stat(int(conn.run(b'SELECT pg_backend_pid()', True)['pg_backend_pid']))[0], ['psql', '-p', str(port)], db)


def batch(argv):
    p = argparse.ArgumentParser(prog='cell.py batch')
    p.add_argument('plan'), p.add_argument('out')
    p.add_argument('--rse', type=float, default=0.05), p.add_argument('--min-time', type=float, default=0.25), p.add_argument('--max-time', type=float, default=2.0)
    p.add_argument('--min-n', type=int, default=3), p.add_argument('--seed', type=int, default=7), p.add_argument('--timeout', default='120s'), p.add_argument('--retries', type=int, default=2)
    a = p.parse_args(argv)
    benchlib.resumable()
    os.environ['PGOPTIONS'] = f'-c statement_timeout={a.timeout}'
    cells = [l.rstrip('\n').split('\t') for l in open(a.plan) if l.strip()]
    scripts, lib, conns, done, meters = {c[0]: parse_script(open(c[3]).read()) for c in cells}, libpq(), {}, {}, {}
    try:
        for attempt in range(1 + a.retries):
            for id, port, db, _ in [c for c in cells if attempt == 0 or done[c[0]][1]]:
                try:
                    if (port, db) not in conns:
                        conns[port, db] = Connection(lib, f'port={port} dbname={db} application_name=sift-cell')
                        meters[port, db] = server(conns[port, db], port, db)
                    done[id] = measure(conns[port, db], scripts[id], a, a.seed, meters[port, db])
                except RuntimeError as e:
                    print(f'{id}: {e}', file=sys.stderr)
                    done[id] = (None, False)
    finally:
        for c in conns.values():
            c.close()
    row = lambda id, cell, noise: '\t'.join([id, *(['nan', 'nan', 'NA', '0', '0', 'NA', 'NA'] if cell is None else
                                                    [f'{cell.p50() * 1e3:.3f}', f'{cell.p95() * 1e3:.3f}', f'{cell.se_p50() * 1e3:.4f}', str(cell.n), f'{cell.wall:.2f}', *map(figure, cell.spent)]), str(int(noise))]) + '\n'
    benchlib.write_text(a.out, ''.join(row(id, *done[id]) for id, *_ in cells))   # all or nothing: an interrupted batch leaves no file, the round is measured again

def main():
    if sys.argv[1:2] == ['batch']:
        return batch(sys.argv[2:])
    p = argparse.ArgumentParser()
    p.add_argument('mode', choices=['qps', 'latency'])
    p.add_argument('--rse', type=float, default=0.05)
    p.add_argument('--min-time', type=float, default=0.25)
    p.add_argument('--max-time', type=float, default=2.0)
    p.add_argument('--min-n', type=int, default=3)
    p.add_argument('--seed', type=int, default=7)
    p.add_argument('--hint')
    p.add_argument('--tmp', default=tempfile.gettempdir())
    p.add_argument('--xbench', action='store_true')
    p.add_argument('--server', help='pid of the server, or its data directory: its CPU and buffers per transaction are the last two fields')
    p.add_argument('--db', help='the database whose buffers are counted (Postgres)')
    split = sys.argv.index('--')
    a, argv = p.parse_args(sys.argv[1:split]), sys.argv[split + 1:]
    cell, start, chunk = Cell(a.mode, int(argv[argv.index('--clients' if a.xbench else '-c') + 1])), time.monotonic(), 0
    used = meter.Meter(meter.postmaster(a.server) if os.path.isdir(a.server) else int(a.server), dbname=a.db).start() if a.server else None
    try:
        if a.hint and os.path.exists(a.hint):
            cell.est = float(open(a.hint).read())
        else:   # one transaction per client: how fast the cell is (a lower bound: a backend's first query is its slowest), kept when it is slow
            first = run_chunk(argv, a.tmp, '-t', 1, a.seed, a.xbench)
            lat = [lat for rows in first.values() for _, lat in rows]
            cell.est = cell.clients / mean(lat)
            if a.min_time / mean(lat) < FEW:
                cell.add(first, 0)
            else:
                cell.total += sum(map(len, first.values()))
        while (step := next_chunk(cell, a)) and chunk < MAX_CHUNKS:
            flag, amount, warm = step
            chunk, t0 = chunk + 1, time.monotonic()
            cell.add(run_chunk(argv, a.tmp, flag, amount, a.seed + chunk, a.xbench), warm)
            if os.environ.get('CELL_DEBUG'):
                print(f'chunk {chunk}: {flag} {amount} warm {warm}: {time.monotonic() - t0:.2f} s, now n {cell.n} {cell.seconds:.2f} s rse {cell.rel_se():.3f}', file=sys.stderr)
    except RuntimeError as e:
        print(e, file=sys.stderr)
        print('0\tNA\t0\t0\tNA\tNA' if a.mode == 'qps' else 'nan\tnan\tNA\t0\t0\tNA\tNA')
        return
    if a.hint:
        open(a.hint, 'w').write(f'{cell.rate}\n')
    wall = time.monotonic() - start
    if used:
        used.stop()
    spent = '\t'.join(map(figure, used.per(cell.total))) if used else 'NA\tNA'
    if a.mode == 'qps':
        print(f'{cell.rate:.2f}\t{cell.rel_se() * cell.rate:.2f}\t{cell.n}\t{wall:.2f}\t{spent}')
    else:
        print(f'{cell.p50() * 1e3:.3f}\t{cell.p95() * 1e3:.3f}\t{cell.se_p50() * 1e3:.4f}\t{cell.n}\t{wall:.2f}\t{spent}')


if __name__ == '__main__':
    main()
