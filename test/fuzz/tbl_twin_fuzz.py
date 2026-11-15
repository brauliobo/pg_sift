#!/usr/bin/env python3
"""Differential fuzz of a sift_table against a plain heap twin (test/t/tbl_twin_fuzz.sh runs it).

  test/fuzz/tbl_twin_fuzz.py [--seeds 1-24] [--seconds 80] [--hold P]    needs PGHOST / PGPORT of a cluster with pg_sift and pg_sift_test, and SIFT_FUZZ_OUT, a directory for the logs

A seed drives two writers (own key ranges) that apply the same INSERT / UPDATE / DELETE / upsert / aborted-transaction statements to the table T (sift_table) and
to its twin H (heap) in one transaction, a long REPEATABLE READ or SERIALIZABLE reader, and two maintenance sessions (VACUUM, VACUUM (FREEZE),
sift.table_maintain) with the background maintenance worker on. In a round, a helper session often holds the maintenance lock (sift._test_tbl_hold_maint) so that
the VACUUMs queue behind it while the writers fill and close write buffers, and promotes under the lock (sift._test_tbl_promote_held): the interleaving in which a
VACUUM that waited for the lock used a list of segments it had read before. After each round the full row set of T and H must be the same through the default plan,
the index and a sequential scan (EXCEPT ALL both ways, no key twice, the primary key probed through the index), the reader's snapshot must give the same answer
on T and H and the same on every re-read, and no statement may fail. Seeds are serial (one cluster); the interleaving of the sessions is the server's, so a seed
that fails on an old build is a probability, not a switch.
"""
import argparse, json, os, random, re, selectors, subprocess, sys, time

SOCK = os.environ['PGHOST']
PORT = os.environ['PGPORT']
DB = os.environ.get('PGDATABASE', 'postgres')
OUT = os.environ['SIFT_FUZZ_OUT']   # the logs of the seeds
T, H = 'tw_sift', 'tw_heap'
ERR = re.compile(r'^(ERROR|FATAL|PANIC|server closed|connection to server|psql: error)', re.M)
BIG = 2_000_000


class Fail(Exception):
    pass


class Harness(Exception):
    pass


class Ctx:
    def __init__(self, label, seed):
        os.makedirs(OUT, exist_ok=True)
        self.path = f'{OUT}/seed_{seed}.log'
        self.f = open(self.path, 'w')
        self.seq = 0
        self.t0 = time.time()
        self.stats = dict(txn_commit=0, txn_rollback=0, ops=0, big_inserts=0, rounds=0, holds=0,
                          hold_waiters_ok=0, hold_released_early=0, promote_held=0, maint_stmts=0, reader_checks=0,
                          readers=0, full_checks=0)

    def log(self, who, text):
        self.seq += 1
        self.f.write(f'-- #{self.seq} t={time.time() - self.t0:7.2f} {who}\n{text.rstrip()}\n')
        self.f.flush()


class Sess:
    def __init__(self, name, ctx, setup=''):
        self.name, self.ctx, self.n, self.buf, self.tok = name, ctx, 0, '', None
        env = dict(os.environ, LC_ALL='C', PGHOST=SOCK, PGPORT=PORT)
        for k in ('PGDATABASE', 'PGSERVICE', 'PGOPTIONS'):
            env.pop(k, None)
        self.p = subprocess.Popen(['psql', '-X', '-A', '-t', '-F', '|', '-U', 'postgres', '-d', DB], stdin=subprocess.PIPE,
                                  stdout=subprocess.PIPE, stderr=subprocess.STDOUT, text=False, env=env)
        self.fd = self.p.stdout.fileno()
        os.set_blocking(self.fd, False)
        if setup:
            self.send(setup)
            self.wait_one(60)

    def send(self, sql):
        assert self.tok is None, f'{self.name} busy'
        self.n += 1
        self.tok = f'@@END {self.n}'
        self.ctx.log(self.name, sql)
        self.p.stdin.write((sql.rstrip() + f'\n\\echo {self.tok}\n').encode())
        self.p.stdin.flush()

    def pump(self):
        try:
            d = os.read(self.fd, 1 << 20)
        except BlockingIOError:
            return
        if not d:
            raise Fail(f'{self.name}: psql exited ({self.p.poll()}): {self.buf[-500:]}')
        self.buf += d.decode(errors='replace')

    def done(self):
        return self.tok is not None and re.search(rf'^{re.escape(self.tok)}$', self.buf, re.M) is not None

    def take(self):
        m = re.search(rf'^{re.escape(self.tok)}$\n?', self.buf, re.M)
        out, self.buf, self.tok = self.buf[:m.start()], self.buf[m.end():], None
        self.ctx.log(self.name + ' result', out)
        return out

    def wait_one(self, timeout):
        r = wait([self], timeout)
        if r is None:
            raise Harness(f'{self.name}: no answer in {timeout}s')
        return r[0]

    def close(self):
        try:
            self.p.stdin.close()
        except Exception:
            pass
        try:
            self.p.wait(timeout=5)
        except Exception:
            self.p.kill()


def wait(sessions, timeout):
    """Outputs of the sessions (in order) once every one has answered, None on timeout."""
    sel = selectors.DefaultSelector()
    for s in sessions:
        sel.register(s.fd, selectors.EVENT_READ, s)
    end = time.time() + timeout
    res = {}
    try:
        while len(res) < len(sessions):
            for s in sessions:
                if s.name in res:
                    continue
                s.pump()
                if s.done():
                    res[s.name] = s.take()
            if len(res) == len(sessions):
                break
            left = end - time.time()
            if left <= 0:
                return None
            sel.select(min(left, 0.2))
    finally:
        sel.close()
    return [res[s.name] for s in sessions]


def pad(i, length):
    return f"repeat(chr(97 + {i} % 26), {length})"


class Writer:
    """Generates ops over its own pk space [base, base + 1e6) (and the negated one), so two writers commute."""

    def __init__(self, name, rng, base):
        self.name, self.rng, self.base, self.next = name, rng, base, base

    def window(self, recent):
        r = self.rng
        hi = max(self.next - 1, self.base)
        a = r.randint(max(self.base, hi - 3000), hi) if recent and r.random() < 0.6 else r.randint(self.base, hi)
        b = min(hi, a + r.choice([10, 100, 1000, 5000, 20000]))
        return a, b

    def ins(self, n, plen):
        a = self.next
        self.next += n
        return (f"INSERT INTO {{t}} SELECT i, (i % 10007), i % 7, {pad('i', plen)} "
                f"FROM generate_series({a}, {a + n - 1}) i")

    def op(self, kind=None, recent=True):
        r = self.rng
        kind = kind or r.choices(['ins_small', 'ins_mid', 'upsert', 'insnothing', 'upd', 'upd_pad', 'upd_pk', 'upd_grp', 'del'],
                                 [18, 12, 10, 5, 25, 5, 4, 5, 16])[0]
        if kind == 'ins_small':
            return self.ins(r.randint(1, 60), r.randint(10, 120))
        if kind == 'ins_mid':
            return self.ins(r.randint(500, 5000), r.randint(30, 150))
        if kind == 'ins_big':
            return self.ins(r.randint(20000, 45000), r.randint(250, 400))
        a, b = self.window(recent)
        m = r.choice([1, 1, 2, 3, 5])
        k = r.randint(0, m - 1)
        sel = f"(pk BETWEEN {a} AND {b} OR pk BETWEEN {-b} AND {-a}) AND pk % {m} = {k}"
        if kind in ('upsert', 'insnothing'):
            tail = ("ON CONFLICT (pk) DO UPDATE SET v = x.v + 1000, grp = excluded.grp" if kind == 'upsert'
                    else "ON CONFLICT DO NOTHING")
            hi = b
            return (f"INSERT INTO {{t}} AS x SELECT i, (i % 9973), i % 5, {pad('i', r.randint(10, 100))} "
                    f"FROM generate_series({a}, {hi}) i {tail}")
        if kind == 'upd':
            return f"UPDATE {{t}} SET v = v + {r.randint(1, 9)} WHERE {sel}"
        if kind == 'upd_pad':
            return f"UPDATE {{t}} SET pad = repeat('z', {r.randint(10, 300)}) WHERE {sel}"
        if kind == 'upd_grp':
            return f"UPDATE {{t}} SET grp = (grp + 1) % 7, v = v - 1 WHERE {sel}"
        if kind == 'upd_pk':
            return f"UPDATE {{t}} SET pk = -pk WHERE {sel} AND NOT EXISTS (SELECT 1 FROM {{t}} o WHERE o.pk = -{{t}}.pk)"
        return f"DELETE FROM {{t}} WHERE {sel}"

    def txns(self, n, kinds=None, recent=True):
        """n transactions of 1..3 ops; kinds forces the kinds of the first txn (the fill)."""
        r, txns = self.rng, []
        for i in range(n):
            ks = [kinds[i]] if kinds and i < len(kinds) else [None] * r.choice([1, 1, 2, 3])
            ops = [self.op(k, recent) for k in ks]
            txns.append((ops, r.random() > 0.15))
        return txns


def txn_sql(ops, commit):
    s = 'BEGIN;\n'
    for op in ops:
        s += f"\\echo @@T\n{op.format(t=T)};\n\\echo @@H\n{op.format(t=H)};\n\\echo @@E\n"
    return s + ('COMMIT;\n' if commit else 'ROLLBACK;\n')


def norm(s):
    return s.replace(T, 'X').replace(H, 'X').strip()


def check_writer(ctx, name, out, txns):
    """Per op the tags / errors of the sift_table and of the heap must be the same."""
    m = ERR.search(out)
    if m:
        raise Fail(f'{name}: unexpected error (T=sift_table, H=heap; the statement is in the log): {out.strip()[:1500]}')
    got = {'@@T': [], '@@H': []}
    cur, ops = None, []
    for line in out.split('\n'):
        if line in ('@@T', '@@H'):
            cur = line
            got[cur].append([])
        elif line == '@@E':
            cur = None
            ops.append((got['@@T'][-1], got['@@H'][-1]))
        elif cur:
            got[cur][-1].append(line)
    bad = []
    for t, h in ops:
        if norm('\n'.join(t)) != norm('\n'.join(h)):
            bad.append((t, h))
    if bad:
        raise Fail(f'{name}: sift_table and heap answered differently to the same statement: T={bad[0][0]} H={bad[0][1]}')
    for _, commit in txns:
        ctx.stats['txn_commit' if commit else 'txn_rollback'] += 1
    ctx.stats['ops'] += len(ops)


MODES = {
    'default': 'RESET sift.enable_count; RESET enable_seqscan; RESET enable_indexscan; RESET enable_indexonlyscan; RESET enable_bitmapscan;',
    'index': 'RESET sift.enable_count; SET enable_seqscan = off; SET enable_bitmapscan = off; RESET enable_indexscan; RESET enable_indexonlyscan;',
    'seq': 'SET enable_indexscan = off; SET enable_indexonlyscan = off; SET enable_bitmapscan = off; SET sift.enable_count = off; RESET enable_seqscan;',
}

FULL = f"""
WITH a AS MATERIALIZED (SELECT pk, v, grp, md5(pad) m FROM {T} WHERE pk >= -2147483648 ORDER BY pk),
     b AS MATERIALIZED (SELECT pk, v, grp, md5(pad) m FROM {H} WHERE pk >= -2147483648 ORDER BY pk)
SELECT (SELECT count(*) FROM (SELECT * FROM a EXCEPT ALL SELECT * FROM b) x),
       (SELECT count(*) FROM (SELECT * FROM b EXCEPT ALL SELECT * FROM a) x),
       (SELECT count(*) - count(DISTINCT pk) FROM a), (SELECT count(*) FROM a), (SELECT count(*) FROM b);
WITH a AS MATERIALIZED (SELECT pk FROM {T} WHERE pk >= -2147483648 ORDER BY pk),
     b AS MATERIALIZED (SELECT pk FROM {H} WHERE pk >= -2147483648 ORDER BY pk)
SELECT (SELECT count(*) FROM (SELECT * FROM a EXCEPT ALL SELECT * FROM b) x),
       (SELECT count(*) FROM (SELECT * FROM b EXCEPT ALL SELECT * FROM a) x), 0, 0, 0;
"""


def probe(ws):
    qs = ''
    for w in ws:
        for t in (T, H):
            qs += (f"SELECT '{w.name}', count(*), coalesce(sum(t.v), 0) FROM generate_series({-w.next}, {w.next}, 7) g "
                   f"JOIN {t} t ON t.pk = g;\n")
    return ('SET enable_hashjoin = off; SET enable_mergejoin = off; SET enable_seqscan = off;\n' + qs +
            'RESET enable_hashjoin; RESET enable_mergejoin; RESET enable_seqscan;\n')


def full_check(ctx, ck, ws, label):
    ctx.stats['full_checks'] += 1
    for mode, setup in MODES.items():
        ck.send(setup + FULL)
        out = ck.wait_one(120)
        if ERR.search(out):
            raise Fail(f'{label} [{mode}]: error: {ERR.search(out).group(0)}: {out}')
        rows = [l for l in out.split('\n') if re.match(r'^\d+\|\d+\|\d+\|\d+\|\d+$', l)]
        if len(rows) != 2:
            raise Harness(f'{label} [{mode}]: unexpected output {out!r}')
        for r in rows[:1]:
            tmh, hmt, dup, nt, nh = map(int, r.split('|'))
            if tmh or hmt or dup or nt != nh:
                raise Fail(f'{label} [{mode}]: rows T-H={tmh} H-T={hmt} dup_pk={dup} count T={nt} H={nh}')
        tmh, hmt = map(int, rows[1].split('|')[:2])
        if tmh or hmt:
            raise Fail(f'{label} [{mode}] pk-only: rows T-H={tmh} H-T={hmt}')
    ck.send(MODES['default'] + probe(ws))
    out = ck.wait_one(120)
    if ERR.search(out):
        raise Fail(f'{label} probe: error: {out}')
    lines = [l for l in out.split('\n') if l.count('|') == 2]
    for i in range(0, len(lines), 2):
        a, b = lines[i].split('|')[1:], lines[i + 1].split('|')[1:]
        if a != b:
            raise Fail(f'{label} index probes: T={a} H={b}')


def snap_sql(t):
    return (f"SELECT 'tot', count(*), count(DISTINCT pk), coalesce(sum(v), 0), coalesce(sum(grp), 0), "
            f"coalesce(sum(length(pad)), 0) FROM {t};\n"
            f"SELECT 'grp', grp, count(*), count(DISTINCT pk), sum(v), sum(length(pad)) FROM {t} GROUP BY grp ORDER BY grp;\n")


def reader_sql():
    s = ''
    for mode in ('default', 'index', 'seq'):
        s += MODES[mode]
        for t in (T, H):
            s += f"\\echo @@{t}\n" + snap_sql(t)
    return s + MODES['default'] + "\\echo @@X\n"


def reader_parse(out):
    res, cur = {}, None
    for line in out.split('\n'):
        if line.startswith('@@'):
            cur = line
            if line != '@@X':
                res.setdefault(line, []).append([])
        elif cur and cur != '@@X' and line.split('|')[0] in ('tot', 'grp'):
            res[cur][-1].append(line)
    return res


def reader_check(ctx, rd, state, label):
    out = rd.wait_one(120)
    ctx.stats['reader_checks'] += 1
    m = ERR.search(out)
    if m:
        raise Fail(f'{label}: reader error: {m.group(0)}: {out}')
    p = reader_parse(out)
    for i, mode in enumerate(('default', 'index', 'seq')):
        a, b = '\n'.join(p[f'@@{T}'][i]), '\n'.join(p[f'@@{H}'][i])
        if a != b:
            raise Fail(f'{label}: reader [{mode}] snapshot differs T vs H:\nT:\n{a}\nH:\n{b}')
        if state['base'] is None:
            state['base'] = {}
        if mode in state['base'] and state['base'][mode] != a:
            raise Fail(f'{label}: reader [{mode}] re-read in the same snapshot changed:\nbefore:\n{state["base"][mode]}\nnow:\n{a}')
        state['base'].setdefault(mode, a)
    # the three modes of one snapshot agree too
    if len({state['base'][m] for m in state['base']}) != 1:
        raise Fail(f'{label}: reader modes differ in one snapshot: {state["base"]}')


def maint_batch(rng, k):
    pool = [f'VACUUM {T};', f"SELECT busy, dead_rows, promoted, merged FROM sift.table_maintain('{T}');",
            f'VACUUM (FREEZE) {T};']
    return '\n'.join(rng.choice(pool) for _ in range(k))


def run_seed(label, seed, budget, hold=None):
    ctx = Ctx(label, seed)
    rng = random.Random(seed)
    prof = seed % 3
    p_hold = (0.7, 0.4, 0.15)[prof] if hold is None else hold
    p_reader = (0.25, 0.4, 0.5)[prof]
    cfg = f"SET sift.table_dead_rewrite_pct = {rng.choice([30, 10, 60])}; SET sift.table_hot_update = {rng.choice(['on', 'off'])};"
    ctx.log('config', f'seed={seed} profile={prof} {cfg}')
    S = {}
    try:
        ck = S['CK'] = Sess('CK', ctx, cfg)
        ck.send(f"DROP TABLE IF EXISTS {T}, {H};\n"
                f"CREATE TABLE {T} (pk int PRIMARY KEY, v int, grp int, pad text) USING sift_table;\n"
                f"CREATE TABLE {H} (pk int PRIMARY KEY, v int, grp int, pad text);")
        out = ck.wait_one(60)
        if ERR.search(out):
            raise Harness(f'setup: {out}')
        w1, w2 = Writer('W1', random.Random(seed * 10 + 1), 1), Writer('W2', random.Random(seed * 10 + 2), 1_000_000)
        ws = [w1, w2]
        W = {w.name: S.setdefault(w.name, Sess(w.name, ctx, cfg)) for w in ws}
        M1 = S['M1'] = Sess('M1', ctx, cfg)
        M2 = S['M2'] = Sess('M2', ctx, cfg)
        HO = S['HO'] = Sess('HO', ctx, cfg)
        RD = S['RD'] = Sess('RD', ctx, cfg)
        reader = None

        def send_writers(plan):
            for w in ws:
                txns = plan(w)
                W[w.name].txns = txns
                W[w.name].send(''.join(txn_sql(o, c) for o, c in txns))

        def collect_writers(timeout=120):
            r = wait([W[w.name] for w in ws], timeout)
            if r is None:
                return False
            for w, out in zip(ws, r):
                check_writer(ctx, w.name, out, W[w.name].txns)
            return True

        def collect_maint(sessions):
            r = wait(sessions, 180)
            if r is None:
                raise Harness('maintenance statements did not finish in 180 s')
            for s, out in zip(sessions, r):
                if ERR.search(out):
                    raise Fail(f'{s.name}: maintenance error: {out}')

        # first fill
        send_writers(lambda w: w.txns(1, ['ins_big']) + w.txns(2))
        collect_writers()
        full_check(ctx, ck, ws, 'initial')

        rnd = 0
        while time.time() - ctx.t0 < budget:
            rnd += 1
            ctx.stats['rounds'] = rnd
            label_r = f'round {rnd}'
            ctx.log('round', label_r)
            hold = rng.random() < p_hold
            if reader is None and rng.random() < p_reader:
                level = rng.choice(['REPEATABLE READ', 'SERIALIZABLE READ ONLY'])
                RD.send(f'BEGIN ISOLATION LEVEL {level};\n' + reader_sql())
                reader = {'base': None, 'left': rng.randint(1, 3)}
                ctx.stats['readers'] += 1
                reader_check(ctx, RD, reader, label_r + ' reader start')
            # phase A: a mixed batch (recent rows with committed tombstones in the write buffers)
            send_writers(lambda w: w.txns(rng.randint(2, 5)))
            if not collect_writers():
                raise Harness('writers phase A hang')
            if hold:
                ctx.stats['holds'] += 1
                HO.send(f"BEGIN;\nSELECT sift._test_tbl_hold_maint('{T}');")
                HO.wait_one(60)
                blockers = [(M1, f'VACUUM {T};')]
                if rng.random() < 0.5:
                    blockers.append((M2, rng.choice([f'VACUUM (FREEZE) {T};', f'VACUUM {T};'])))
                for s, q in blockers:
                    s.send(q)
                    ctx.stats['maint_stmts'] += 1
                for _ in range(100):
                    HO.send(f"SELECT count(*) FROM pg_locks WHERE locktype = 'page' AND NOT granted AND relation = '{T}'::regclass;")
                    n = int(HO.wait_one(30).strip().split('\n')[0])
                    if n >= len(blockers):
                        ctx.stats['hold_waiters_ok'] += 1
                        break
                    time.sleep(0.05)
                # phase B: fill to close write buffers, plus updates / deletes, while VACUUM waits
                fill = rng.choice([['ins_big'], ['ins_big', 'ins_big'], ['ins_mid', 'ins_big']])
                send_writers(lambda w: w.txns(len(fill) + rng.randint(1, 3), fill))
                if rng.random() < 0.6:
                    RD.send(reader_sql()) if reader else None
                if not collect_writers(40):
                    ctx.stats['hold_released_early'] += 1
                    HO.send('COMMIT;')
                    HO.wait_one(30)
                    if not collect_writers(120):
                        raise Harness('writers stuck even after the maintenance lock was released')
                    hold = False
                if hold:
                    if rng.random() < 0.8:
                        ctx.stats['promote_held'] += 1
                        HO.send(f"SELECT sift._test_tbl_promote_held('{T}');")
                        HO.wait_one(120)
                    HO.send('COMMIT;')
                    HO.wait_one(30)
                # phase C: writers and a reader while the released VACUUMs run
                send_writers(lambda w: w.txns(rng.randint(1, 3)))
                collect_writers()
                collect_maint([s for s, _ in blockers])
            else:
                for s in (M1, M2):
                    s.send(maint_batch(rng, rng.randint(1, 3)))
                    ctx.stats['maint_stmts'] += 1
                kinds = rng.choice([None, ['ins_big'], ['ins_mid']])
                send_writers(lambda w: w.txns(rng.randint(2, 6), kinds))
                if reader and rng.random() < 0.6:
                    RD.send(reader_sql())
                collect_writers()
                collect_maint([M1, M2])
            if reader:
                if RD.tok:
                    reader_check(ctx, RD, reader, label_r + ' reader mid-round')
                RD.send(reader_sql())
                reader_check(ctx, RD, reader, label_r + ' reader end')
                reader['left'] -= 1
                if reader['left'] <= 0:
                    RD.send('COMMIT;')
                    RD.wait_one(30)
                    reader = None
            full_check(ctx, ck, ws, label_r)
        if reader:
            RD.send('COMMIT;')
            RD.wait_one(30)
            reader = None
        # drain: nothing holds a snapshot; maintenance, then the background worker, then check again
        M1.send(f'VACUUM {T};\nSELECT sift.table_maintain(\'{T}\');\nVACUUM (FREEZE) {T};')
        M2.send(f'VACUUM (FREEZE) {T};')
        collect_maint([M1, M2])
        full_check(ctx, ck, ws, 'final after maintenance')
        for i in range(3):
            time.sleep(0.7)
            full_check(ctx, ck, ws, f'final worker pass {i + 1}')
        ck.send(f'DROP TABLE {T}, {H};')
        ck.wait_one(60)
        ctx.log('result', 'OK ' + json.dumps(ctx.stats))
        return 0, ctx
    except Fail as e:
        ctx.log('FAIL', str(e))
        try:
            ctx_dump(ctx, S)
        except Exception as e2:
            ctx.log('dump error', repr(e2))
        return 1, ctx
    except Harness as e:
        ctx.log('HARNESS', str(e))
        return 2, ctx
    finally:
        for s in S.values():
            s.close()


def ctx_dump(ctx, S):
    ck = Sess('CK2', ctx)
    ck.send(f"SELECT pk, count(*) FROM {T} GROUP BY pk HAVING count(*) > 1 ORDER BY pk LIMIT 8;\n"
            f"SELECT count(*), count(DISTINCT pk), sum(v) FROM {T};\nSELECT count(*), count(DISTINCT pk), sum(v) FROM {H};")
    ck.wait_one(60)
    ck.close()


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--seeds', default='1-3')
    ap.add_argument('--seconds', type=float, default=80)
    ap.add_argument('--hold', type=float)
    a = ap.parse_args()
    lo, _, hi = a.seeds.partition('-')
    bad = 0
    for seed in range(int(lo), int(hi or lo) + 1):
        rc, ctx = run_seed('twin', seed, a.seconds, a.hold)
        print(f'seed={seed} {("OK", "FAIL", "HARNESS")[rc]} {time.time() - ctx.t0:.0f}s log={ctx.path}', flush=True)
        bad += rc != 0
    sys.exit(1 if bad else 0)


if __name__ == '__main__':
    main()
