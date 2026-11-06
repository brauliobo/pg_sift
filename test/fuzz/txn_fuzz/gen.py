# A seed: the configuration of a table and its sift_col index, and a script of actions: statements of a session that writes (A), of a second session that
# reads and writes beside it (B), and checks that compare the plain heap plan with the scan of the index. An action is a dict:
#   {'s': 'A'|'B'|'F', 't': 'sql', 'sql': text}   statements of a session (F: a fresh backend that runs one check)
#   {'s': ..., 't': 'check', 'pts': [..], 'rng': [a, b], 'ten': n}   the comparison (run.py)
#   {'s': ..., 't': 'reconnect'}   the session ends and a new backend takes its place

import random
from dataclasses import dataclass, asdict

DOMAIN_B = 40_000_000       # session B writes its own rows from here: no lock waits with A
MOVED = 50_000_000          # a row whose key changes goes here (+ a counter)


@dataclass
class Config:
    seed: int
    keytype: str        # int | text
    rows: int
    track: str          # auto | key | off | ctid
    fillfactor: int
    wbuf: bool          # a share of the rows is inserted after the index: it is in the write buffer
    promote: bool       # ... and promoted before the run
    vacuum: bool        # VACUUM (FREEZE) before the run: the pages are all-visible
    trigger: str        # none | after | before
    warm: bool          # the first action of A is a read, as a backend that has worked
    prepared: bool      # prepared transactions are part of the script (the server allows them)
    trunc: bool = True  # VACUUM may truncate the heap: False runs VACUUM (TRUNCATE false)
    oc: bool = True     # INSERT .. ON CONFLICT (speculative insertion): False leaves it out
    spi: bool = True    # statements that run SQL through SPI (a volatile function, a DO block) and a trigger that updates t: False leaves them out

    def key(self, e):
        return str(e) if self.keytype == 'int' else f"'c' || ({e})"

    def tag(self):
        return '_'.join(str(v) for v in asdict(self).values() if not isinstance(v, bool) or v).replace('/', '')


def config(seed, prepared, tracks, spi=True, oc=True, trunc=True):
    r = random.Random(f'cfg{seed}')
    return Config(seed, r.choice(['int', 'text']), r.choice([4000, 12000, 20000]), r.choices(tracks, [{'auto': 4, 'key': 3, 'off': 1, 'ctid': 3}[t] for t in tracks])[0], r.choice([100, 100, 75]),
                  r.random() < 0.4, r.random() < 0.5, r.random() < 0.75, r.choice(['none', 'none', 'after', 'before']), r.random() < 0.6, prepared, trunc, oc, spi)


def setup_sql(c):
    kt = 'int' if c.keytype == 'int' else 'text'
    opts = {'auto': '', 'key': " WITH (track_changes = 'k')", 'off': " WITH (track_changes = 'off')", 'ctid': " WITH (track_changes = 'ctid')"}[c.track]
    r0 = c.rows * 6 // 10 if c.wbuf else c.rows
    s = f"""SET client_min_messages = warning;
CREATE EXTENSION IF NOT EXISTS pg_sift;
CREATE TABLE t (k {kt} PRIMARY KEY, tenant int NOT NULL, amount int NOT NULL, pad int NOT NULL DEFAULT 0) WITH (autovacuum_enabled = false, fillfactor = {c.fillfactor});
INSERT INTO t (k, tenant, amount) SELECT {c.key('i')}, i % 10, i FROM generate_series(1, {r0}) i;
CREATE INDEX t_c ON t USING sift_col (k, tenant, amount){opts};
"""
    if c.wbuf:
        s += f"INSERT INTO t (k, tenant, amount) SELECT {c.key('i')}, i % 10, i FROM generate_series({r0 + 1}, {c.rows}) i;\n"
        if c.promote:
            s += "SELECT sift.col_promote('t_c');\n"
    s += "VACUUM (FREEZE, ANALYZE) t;\n" if c.vacuum else "ANALYZE t;\n"
    s += f"""CREATE FUNCTION tf_one() RETURNS int LANGUAGE plpgsql AS $$ BEGIN PERFORM 1; RETURN 0; END $$;
CREATE FUNCTION tf_bump(kk {kt}) RETURNS int LANGUAGE plpgsql AS $$ BEGIN UPDATE t SET amount = amount + 1 WHERE k = kk; RETURN 0; END $$;
"""
    if c.trigger == 'after' and c.spi:
        s += """CREATE FUNCTION tf_trg() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF pg_trigger_depth() = 1 AND NEW.amount % 7 = 0 THEN UPDATE t SET pad = pad + 1 WHERE k = NEW.k; END IF; RETURN NULL; END $$;
CREATE TRIGGER tf_t AFTER UPDATE ON t FOR EACH ROW EXECUTE FUNCTION tf_trg();
"""
    elif c.trigger == 'before':
        s += """CREATE FUNCTION tf_trg() RETURNS trigger LANGUAGE plpgsql AS $$ BEGIN IF NEW.amount % 11 = 0 THEN NEW.amount := NEW.amount + 1; END IF; RETURN NEW; END $$;
CREATE TRIGGER tf_t BEFORE UPDATE ON t FOR EACH ROW EXECUTE FUNCTION tf_trg();
"""
    return s


class Gen:
    def __init__(self, c, n):
        self.c, self.n = c, n
        self.r = random.Random(f'act{c.seed}')
        self.nxt = c.rows + 1           # the next row index no statement used
        self.moved = 0
        self.bnxt = DOMAIN_B
        self.out = []
        self.intxn = False
        self.bintxn = False
        self.sp = []
        self.cur = False
        self.pending = []               # the prepared transactions
        self.gid = 0

    # ---- pieces
    def k(self, e):
        return self.c.key(e)

    def idx(self):
        return self.r.randint(1, self.nxt)

    def pred(self, small=False, simple=False):
        r, hi = self.r, self.nxt
        a = r.randint(1, hi)
        step = r.choice([7, 13, 31, 101]) if small else r.choice([3, 5, 7, 13, 31, 101])
        b = min(hi, a + r.choice([50, 400, 2500, hi]))
        kind = r.choice(['series', 'series', 'between', 'tenant', 'mod', 'point', 'few', 'series'])
        if small and kind in ('tenant', 'mod'):
            kind = 'series'
        if simple and kind == 'series':
            kind = 'between'
        if kind == 'series':
            return f"k IN (SELECT {self.k('i')} FROM generate_series({a}, {b}, {step}) i)"
        if kind == 'between':
            return f"k BETWEEN {self.k(a)} AND {self.k(min(hi, a + r.choice([5, 60, 600])))}"
        if kind == 'tenant':
            return f"tenant = {r.randint(0, 9)}"
        if kind == 'mod':
            m = r.choice([7, 11, 17, 29])
            return f"amount % {m} = {r.randint(0, m - 1)}"
        if kind == 'point':
            return f"k = {self.k(a)}"
        return 'k IN (' + ', '.join(self.k(r.randint(1, hi)) for _ in range(r.randint(2, 6))) + ')'

    def a_stmt(self):
        r, c = self.r, self.c
        p = self.pred()
        d = r.randint(1, 900)
        kinds = ['upd_amount'] * 5 + ['upd_tenant'] * 2 + ['upd_pad', 'upd_both', 'upd_key', 'upd_vol', 'delete', 'delete', 'insert', 'insert', 'upsert', 'merge', 'copy', 'cte_upd', 'cte_del',
                 'bump', 'do_exc', 'do_spi', 'upd_amount']
        w = r.choice(kinds)
        if not c.spi and w in ('upd_vol', 'bump', 'do_exc', 'do_spi'):
            w = 'upd_amount'
        if not c.oc and w == 'upsert':
            w = 'insert'
        if w == 'upd_amount':
            return f"UPDATE t SET amount = amount + {d} WHERE {p}"
        if w == 'upd_tenant':
            return f"UPDATE t SET tenant = (tenant + {r.randint(1, 9)}) % 10 WHERE {p}"
        if w == 'upd_pad':
            return f"UPDATE t SET pad = pad + 1 WHERE {p}"
        if w == 'upd_both':
            return f"UPDATE t SET amount = amount + {d}, tenant = (tenant + 1) % 10 WHERE {p}"
        if w == 'upd_key':
            self.moved += 1
            e = f"k + {MOVED + self.moved * 1000}" if c.keytype == 'int' else f"k || '_{self.moved}'"
            return f"UPDATE t SET k = {e} WHERE {self.pred(True)}"
        if w == 'upd_vol':
            return f"UPDATE t SET amount = amount + {d} + tf_one() WHERE {p}"
        if w == 'delete':
            return f"DELETE FROM t WHERE {self.pred(True)}"
        if w in ('insert', 'upsert'):
            a = r.randint(1, self.nxt) if r.random() < 0.5 else self.nxt
            b = a + r.choice([20, 300, 2000])
            self.nxt = max(self.nxt, b + 1)
            if not c.oc:
                a = self.nxt
                b = a + r.choice([20, 300, 2000])
                self.nxt = b + 1
            conflict = ('' if not c.oc else 'ON CONFLICT DO NOTHING') if w == 'insert' else 'ON CONFLICT (k) DO UPDATE SET amount = t.amount + EXCLUDED.amount, tenant = EXCLUDED.tenant'
            return f"INSERT INTO t (k, tenant, amount) SELECT {self.k('i')}, i % 10, i FROM generate_series({a}, {b}, {r.choice([1, 2, 3])}) i {conflict}"
        if w == 'merge':
            a = r.randint(1, self.nxt)
            b = a + r.choice([60, 700, 3000])
            self.nxt = max(self.nxt, b + 1)
            return (f"MERGE INTO t USING (SELECT i FROM generate_series({a}, {b}) i) s ON t.k = {self.k('s.i')} WHEN MATCHED AND s.i % 3 = 0 THEN DELETE "
                    f"WHEN MATCHED THEN UPDATE SET amount = t.amount + {d} WHEN NOT MATCHED THEN INSERT (k, tenant, amount) VALUES ({self.k('s.i')}, s.i % 10, s.i)")
        if w == 'copy':
            a, m = self.nxt, r.randint(5, 400)
            self.nxt += m
            return 'COPY t (k, tenant, amount) FROM STDIN;\n' + '\n'.join(f"{i if c.keytype == 'int' else 'c' + str(i)}\t{i % 10}\t{i}" for i in range(a, a + m)) + '\n\\.'
        if w == 'cte_upd':
            return f"WITH u AS (UPDATE t SET amount = amount + 1 WHERE {p} RETURNING k) UPDATE t SET tenant = (tenant + 1) % 10 WHERE k IN (SELECT k FROM u)"
        if w == 'cte_del':
            self.moved += 1
            e = f"k + {MOVED + self.moved * 1000}" if c.keytype == 'int' else f"k || '_{self.moved}'"
            return f"WITH d AS (DELETE FROM t WHERE {self.pred(True)} RETURNING *) INSERT INTO t (k, tenant, amount) SELECT {e}, tenant, amount + 1 FROM d"
        if w == 'bump':
            return f"SELECT sum(tf_bump(k)) FROM t WHERE {self.pred(True)}"
        if w == 'do_exc':
            fail = r.choice(['true', 'false'])
            return (f"DO $$ BEGIN UPDATE t SET amount = amount + {d} WHERE {p}; IF {fail} THEN RAISE EXCEPTION 'tf'; END IF; "
                    f"EXCEPTION WHEN raise_exception THEN UPDATE t SET tenant = (tenant + 3) % 10 WHERE {self.pred(True)}; END $$")
        return f"DO $$ BEGIN EXECUTE 'UPDATE t SET amount = amount + {d} WHERE ' || $q${p}$q$; PERFORM tf_one(); UPDATE t SET pad = pad + 1 WHERE {self.pred(True)}; END $$"

    def b_stmt(self):
        r = self.r
        a = self.bnxt
        w = r.choice(['ins', 'ins', 'upd', 'del', 'upd_shared'])
        self.bnxt += 50
        if w == 'ins':
            return f"INSERT INTO t (k, tenant, amount) SELECT {self.k('i')}, i % 10, i FROM generate_series({a}, {a + 40}) i" + (' ON CONFLICT DO NOTHING' if self.c.oc else '')
        if w == 'upd':
            return f"UPDATE t SET amount = amount + 5, tenant = (tenant + 1) % 10 WHERE k >= {self.k(DOMAIN_B)} AND k < {self.k(a)}"
        if w == 'del':
            return f"DELETE FROM t WHERE k >= {self.k(DOMAIN_B)} AND k < {self.k(a)} AND amount % 3 = 0"
        return f"UPDATE t SET pad = pad + 1 WHERE {self.pred(True)}"

    def check(self, s):
        r = self.r
        a = r.randint(1, self.nxt)
        self.out.append({'s': s, 't': 'check', 'pts': [r.randint(1, self.nxt) for _ in range(6)] + [DOMAIN_B + 7], 'rng': [a, a + r.choice([80, 900, 6000])], 'ten': r.randint(0, 9)})

    def vacuum_sql(self):
        opts = (['FREEZE'] if self.r.random() < 0.5 else []) + ([] if self.c.trunc else ['TRUNCATE false'])
        return 'VACUUM ' + (f"({', '.join(opts)}) " if opts else '') + 't'

    def sql(self, s, text):
        self.out.append({'s': s, 't': 'sql', 'sql': text})

    # ---- the script
    def b_step(self):
        r = self.r
        if self.bintxn:
            w = r.choice(['check', 'check', 'write', 'end'])
        else:
            w = r.choice(['check', 'write', 'write', 'begin', 'vacuum', 'promote', 'reconnect'])
        if w == 'check':
            self.check('B')
        elif w == 'write':
            self.sql('B', self.b_stmt())
        elif w == 'begin':
            self.sql('B', 'BEGIN ISOLATION LEVEL REPEATABLE READ; SELECT count(*) FROM t WHERE tenant = 3')
            self.bintxn = True
        elif w == 'end':
            self.sql('B', r.choice(['COMMIT', 'ROLLBACK']))
            self.bintxn = False
        elif w == 'vacuum':
            self.sql('B', self.vacuum_sql())
        elif w == 'promote':
            self.sql('B', "SELECT sift.col_promote('t_c')")
        else:
            self.out.append({'s': 'B', 't': 'reconnect'})
            self.bintxn = False

    def finish_txn(self):
        r = self.r
        w = r.choice(['commit'] * 6 + ['rollback'] * 2 + (['prepare'] if self.c.prepared else []))
        self.cur = False
        self.sp = []
        if w == 'prepare':
            self.gid += 1
            self.pending.append(f'tf{self.c.seed}_{self.gid}')
            self.sql('A', f"PREPARE TRANSACTION '{self.pending[-1]}'")
        else:
            self.sql('A', w.upper())
        self.intxn = False
        self.check('A')
        if r.random() < 0.7:
            self.check('F')

    def resolve(self):
        gid = self.pending.pop(0)
        self.sql('F', f"{self.r.choice(['COMMIT', 'ROLLBACK'])} PREPARED '{gid}'")
        self.check('F')

    def build(self):
        r = self.r
        if self.c.warm:
            self.check('A')
        while len(self.out) < self.n:
            if self.pending and r.random() < 0.25:
                self.resolve()
            elif r.random() < 0.12:
                self.b_step()
            elif not self.intxn:
                w = r.choice(['begin'] * 6 + ['auto'] * 3 + ['vacuum', 'promote', 'reconnect'])
                if w == 'begin':
                    self.sql('A', 'BEGIN' + r.choice(['', '', ' ISOLATION LEVEL REPEATABLE READ', ' ISOLATION LEVEL READ COMMITTED']))
                    self.intxn = True
                elif w == 'auto':
                    self.sql('A', self.a_stmt())
                    self.check('A')
                elif w == 'vacuum':
                    self.sql('A', self.vacuum_sql())
                    self.check('A')
                elif w == 'promote':
                    self.sql('A', "SELECT sift.col_promote('t_c')")
                    self.check('A')
                else:
                    self.out.append({'s': 'A', 't': 'reconnect'})
            else:
                w = r.choice(['stmt'] * 14 + ['sp'] * 3 + ['cursor'] * 2 + ['end'] * 3)
                if w == 'stmt':
                    self.sql('A', self.a_stmt())
                    if r.random() < 0.6:
                        self.check('A')
                elif w == 'sp':
                    if self.sp and r.random() < 0.6:
                        name = r.choice(self.sp)
                        if r.random() < 0.6:
                            self.sql('A', f'ROLLBACK TO {name}')
                            self.sp = self.sp[:self.sp.index(name) + 1]
                        else:
                            self.sql('A', f'RELEASE {name}')
                            self.sp = self.sp[:self.sp.index(name)]
                        self.check('A')
                    else:
                        self.sp.append(f's{len(self.out)}')
                        self.sql('A', f'SAVEPOINT {self.sp[-1]}')
                elif w == 'cursor':
                    if not self.cur:
                        self.sql('A', f"DECLARE tfc CURSOR FOR SELECT * FROM t WHERE {self.pred(True, True)}")
                        self.cur = True
                    else:
                        self.sql('A', 'FETCH 3 FROM tfc; UPDATE t SET amount = amount + 1 WHERE CURRENT OF tfc; FETCH 2 FROM tfc; UPDATE t SET tenant = (tenant + 1) % 10 WHERE CURRENT OF tfc')
                        if r.random() < 0.4:
                            self.sql('A', 'CLOSE tfc')
                            self.cur = False
                        self.check('A')
                else:
                    self.finish_txn()
        if self.intxn:
            self.finish_txn()
        while self.pending:
            self.resolve()
        if self.bintxn:
            self.sql('B', 'COMMIT')
        self.check('A')
        self.check('B')
        self.check('F')
        self.sql('A', self.vacuum_sql())
        self.check('F')
        return self.out


def script(c, n):
    return Gen(c, n).build()
