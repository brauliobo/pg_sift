#!/usr/bin/env python3
"""One measured cell of steps/articles.sh and steps/places.sh: pgbench runs in chunks until the cell is precise enough.

    python3 -I cell.py qps|latency [--rse X] [--min-time S] [--max-time S] [--min-n N] [--seed N] [--hint FILE] [--tmp DIR] -- PGBENCH_ARGS...

PGBENCH_ARGS are the arguments of pgbench without -t, -T, -l, --log-prefix and --random-seed (the clients are its -c). Every
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
           there are fewer, that of the mean latency: sd / (mean * sqrt(n)), whichever is larger. Prints: qps, stderr, n, seconds.
  latency  value = p50 and p95 of the transaction latency in ms. The standard error of the p50 is distribution free: half the
           distance between the order statistics n/2 -+ sqrt(n)/2 (the 68% interval of the median). Prints: p50, p95, stderr of p50, n,
           seconds. n counts the transactions kept.

--hint FILE remembers the rate of the cell; the next round of the same cell needs no first chunk to size the others. A failed pgbench
prints 0 (qps) or nan (latency) like before, its error on stderr.
"""
import argparse, math, os, shutil, subprocess, sys, tempfile, time

FEW = 10          # transactions per client that make a chunk worth a dropped first transaction and a count instead of seconds
MAX_CHUNKS = 12
BATCH = 5         # transactions of a client in a batch, at least


def run_chunk(argv, tmp, flag, amount, seed):
    """pgbench -t or -T: {client: [(end time s, latency s)]} in transaction order"""
    log = tempfile.mkdtemp(dir=tmp)
    try:
        run = subprocess.run(['pgbench', *argv, flag, str(amount), '-l', f'--log-prefix={log}/l', f'--random-seed={seed}'], capture_output=True, text=True)
        if run.returncode:
            raise RuntimeError(next((l for l in run.stderr.splitlines() if 'error:' in l), f'pgbench exited with {run.returncode}'))
        clients = {}
        for name in os.listdir(log):
            with open(os.path.join(log, name)) as f:
                for line in f:
                    c, i, lat, _, sec, us = line.split()[:6]
                    clients.setdefault(int(c), []).append((int(i), int(sec) + int(us) / 1e6, int(lat) / 1e6))
        return {c: [(t, lat) for _, t, lat in sorted(rows)] for c, rows in clients.items()}
    finally:
        shutil.rmtree(log, ignore_errors=True)


def mean(v):
    return sum(v) / len(v)


def stdev(v):
    m = mean(v)
    return math.sqrt(sum((x - m) ** 2 for x in v) / (len(v) - 1))


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
        self.mode, self.clients, self.lat, self.batches, self.est = mode, clients, [], [], 0.0

    def add(self, clients, warm):
        """the transactions of a chunk, the first `warm` of every client dropped"""
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
        return stdev(self.lat) / (mean(self.lat) * math.sqrt(self.n)) if self.n > 1 else math.inf

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


def main():
    p = argparse.ArgumentParser()
    p.add_argument('mode', choices=['qps', 'latency'])
    p.add_argument('--rse', type=float, default=0.05)
    p.add_argument('--min-time', type=float, default=0.25)
    p.add_argument('--max-time', type=float, default=2.0)
    p.add_argument('--min-n', type=int, default=3)
    p.add_argument('--seed', type=int, default=7)
    p.add_argument('--hint')
    p.add_argument('--tmp', default=tempfile.gettempdir())
    split = sys.argv.index('--')
    a, argv = p.parse_args(sys.argv[1:split]), sys.argv[split + 1:]
    cell, start, chunk = Cell(a.mode, int(argv[argv.index('-c') + 1])), time.monotonic(), 0
    try:
        if a.hint and os.path.exists(a.hint):
            cell.est = float(open(a.hint).read())
        else:   # one transaction per client: how fast the cell is (a lower bound: a backend's first query is its slowest), kept when it is slow
            first = run_chunk(argv, a.tmp, '-t', 1, a.seed)
            lat = [lat for rows in first.values() for _, lat in rows]
            cell.est = cell.clients / mean(lat)
            if a.min_time / mean(lat) < FEW:
                cell.add(first, 0)
        while (step := next_chunk(cell, a)) and chunk < MAX_CHUNKS:
            flag, amount, warm = step
            chunk, t0 = chunk + 1, time.monotonic()
            cell.add(run_chunk(argv, a.tmp, flag, amount, a.seed + chunk), warm)
            if os.environ.get('CELL_DEBUG'):
                print(f'chunk {chunk}: {flag} {amount} warm {warm}: {time.monotonic() - t0:.2f} s, now n {cell.n} {cell.seconds:.2f} s rse {cell.rel_se():.3f}', file=sys.stderr)
    except RuntimeError as e:
        print(e, file=sys.stderr)
        print('0\tNA\t0\t0' if a.mode == 'qps' else 'nan\tnan\tNA\t0\t0')
        return
    if a.hint:
        open(a.hint, 'w').write(f'{cell.rate}\n')
    wall = time.monotonic() - start
    if a.mode == 'qps':
        print(f'{cell.rate:.2f}\t{cell.rel_se() * cell.rate:.2f}\t{cell.n}\t{wall:.2f}')
    else:
        print(f'{cell.p50() * 1e3:.3f}\t{cell.p95() * 1e3:.3f}\t{cell.se_p50() * 1e3:.4f}\t{cell.n}\t{wall:.2f}')


main()
