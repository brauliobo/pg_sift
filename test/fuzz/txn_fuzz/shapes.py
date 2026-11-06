# The shapes that found wrong answers and crashes by hand and that a single query of the plain checks does not run: reads through a cursor (SCROLL, FETCH BACKWARD, MOVE,
# WITH HOLD, a cursor that stays open while another session commits and the index is changed), prepared statements that live as long as the session (the generic plan
# after the fifth execution), a hash partitioned table with one index per partition (DETACH and ATTACH between statements, a row that moves to another partition) and
# upserts, a COPY that fails after it inserted rows and triggers that read the table, all mixed with functions that run SQL through SPI. `txn_fuzz.py --shapes`.
# The checks are actions of kind 'cursor', 'prep', 'curdecl', 'curfetch' and 'curclose' (run.py); the statements are those of gen.py, with the shapes added.

import dataclasses
import random
import re

from .gen import Gen, config as base_config
from .run import cursor_queries, prep_queries

MOVES = ['FETCH 3', 'FETCH BACKWARD 2', 'MOVE FORWARD 4', 'FETCH PRIOR', 'FETCH FIRST', 'FETCH LAST', 'MOVE ABSOLUTE 2', 'FETCH ABSOLUTE 5', 'FETCH 1', 'MOVE BACKWARD 3',
         'FETCH FORWARD 6', 'MOVE BACKWARD ALL', 'FETCH ALL', 'FETCH BACKWARD ALL']
KINDS = ('cursor', 'prep', 'persist')       # --only: the checks of one shape, in place of the plain ones


def shape_config(seed, prepared, tracks, spi=True, oc=True, trunc=True, parts=None):
    """The configuration of config(): a third of the seeds are partitioned (parts forces 0 or 3), and the triggers of the shapes (BEFORE INSERT and an AFTER INSERT statement that read t) are drawn too"""
    c = base_config(seed, prepared, tracks, spi, oc, trunc)
    r = random.Random(f'shape{seed}')
    n = r.choice([0, 0, 3]) if parts is None else parts
    trigger = r.choice(['insert', 'stmt']) if r.random() < 0.3 and spi else c.trigger
    return dataclasses.replace(c, parts=n, trigger=trigger, track='auto' if n and c.track in ('key', 'ctid') else c.track)    # no statement trigger of a partition hears a statement on the parent: the index refuses track_changes on a key or ctid


class ShapeGen(Gen):
    def __init__(self, c, n, only=''):
        super().__init__(c, n)
        self.r = random.Random(f'shape_act{c.seed}')
        self.only = only
        self.atx = False            # session A is in a transaction block
        self.pc = None              # the cursors tfc_<variant> A has open: {'hold': bool} or None
        self.det = None             # the partition that is detached

    # ---- statements
    def sql(self, s, text):
        super().sql(s, text)
        if s == 'A':
            first = text.lstrip().split(None, 1)[0].upper().rstrip(';')
            ended = first in ('COMMIT', 'ROLLBACK', 'END', 'ABORT') and not re.match(r'\s*\w+\s+(TO|PREPARED)\b', text, re.I) or re.match(r'\s*PREPARE\s+TRANSACTION', text, re.I)
            if first in ('BEGIN', 'START'):
                self.atx = True
            elif ended:
                if self.pc and (not self.pc['hold'] or first in ('ROLLBACK', 'ABORT')):
                    self.pc = None
                self.atx = False

    def a_stmt(self):
        if self.r.random() >= 0.15:
            return super().a_stmt()
        r, c = self.r, self.c
        a = r.randint(1, self.nxt)
        b = a + r.choice([30, 400])
        m = r.choice(['upsert_spi', 'upsert_where', 'ins_nested_oc', 'do_upsert', 'copy_fail', 'upsert_spi', 'copy_fail'])
        if (m != 'copy_fail' and not c.oc) or (m in ('upsert_spi', 'ins_nested_oc', 'do_upsert') and not c.spi):
            m = 'copy_fail'
        if m in ('upsert_spi', 'upsert_where'):
            spi = ' + tf_one()' if m == 'upsert_spi' and c.spi else ''
            where = '' if m == 'upsert_spi' else f' WHERE t.amount % 2 = {r.randint(0, 1)}'
            self.nxt = max(self.nxt, b + 1)
            return (f"INSERT INTO t (k, tenant, amount) SELECT {self.k('i')}, i % 10, i FROM generate_series({a}, {b}) i "
                    f"ON CONFLICT (k) DO UPDATE SET amount = t.amount + EXCLUDED.amount{spi}, pad = t.pad + 1{where}")
        if m == 'ins_nested_oc':
            f = r.choice(['tf_ins', 'tf_bump', 'tf_del'])
            lo = self.nxt
            self.nxt += 70
            return (f"INSERT INTO t (k, tenant, amount) SELECT {self.k('i')}, i % 10, i + {f}({self.k('i')}) FROM generate_series({lo}, {lo + 60}) i "
                    f"ON CONFLICT DO NOTHING")
        if m == 'do_upsert':
            self.nxt = max(self.nxt, b + 1)
            return (f"DO $$ BEGIN INSERT INTO t (k, tenant, amount) SELECT {self.k('i')}, i % 10, i FROM generate_series({a}, {b}) i "
                    f"ON CONFLICT (k) DO UPDATE SET amount = t.amount + 1; PERFORM tf_bump({self.k(a)}); PERFORM tf_ins({self.k(a)}); "
                    f"EXCEPTION WHEN unique_violation THEN UPDATE t SET pad = pad + 1 WHERE k = {self.k(a)}; END $$")
        # a COPY whose last row is a key that exists: the rows it inserted before are aborted together (a savepoint keeps the transaction of A alive)
        lo, m_ = self.nxt, r.randint(5, 300)
        self.nxt += m_ + 1
        last = r.randint(1, lo - 1)
        rows = list(range(lo, lo + m_)) + [last]
        return ('SAVEPOINT tfs;\nCOPY t (k, tenant, amount) FROM STDIN;\n' + '\n'.join(f"{i if c.keytype == 'int' else 'c' + str(i)}\t{i % 10}\t{i}" for i in rows) + '\n\\.\nROLLBACK TO tfs')

    def maint_stmt(self):
        if self.c.parts and self.det is None and self.r.random() < 0.4:
            self.det = self.r.randrange(self.c.parts)
            return f'ALTER TABLE t DETACH PARTITION t_p{self.det}'
        return super().maint_stmt()

    # ---- checks
    def params(self):
        r = self.r
        a = r.randint(1, self.nxt)
        return {'pts': [r.randint(1, self.nxt) for _ in range(6)] + [r.randint(1, self.nxt)], 'rng': [a, a + r.choice([80, 900, 6000])], 'ten': r.randint(0, 9)}

    def prep_args(self, q):
        r, hi = self.r, self.nxt
        out = []
        for _ in range(r.randint(2, 4)):
            if q == 0:
                out.append([r.randint(1, hi)])
            elif q == 1:
                out.append([r.randint(1, hi), r.choice([5, 60, 600])])
            elif q == 2:
                out.append([r.randint(1, hi) for _ in range(3)])
            elif q == 3:
                out.append([r.randint(0, 9)])
            elif q == 4:
                out.append([r.randint(0, 9), r.randint(1, hi)])
            else:
                lo = r.randint(1, hi)
                out.append([lo, r.choice([60, 400])])
        return out

    def shape(self, s, kind):
        r = self.r
        a = {'s': s, 't': 'check', 'kind': kind, **self.params()}
        if kind == 'cursor':
            a.update(q=r.randrange(len(cursor_queries(self.c, a))), hold=r.random() < 0.4, ops=[r.choice(MOVES) for _ in range(r.randint(3, 7))])
        else:
            q = r.randrange(len(prep_queries(self.c)))
            a.update(q=q, pcm=r.choice(['auto', 'auto', 'auto', 'force_generic_plan', 'force_custom_plan']), args=self.prep_args(q))
        self.out.append(a)

    def persist(self):
        """Cursors that stay open: declared in a transaction under each plan (and held across the COMMIT, or not), read later, after the other session wrote and the index changed"""
        r = self.r
        if self.pc is None:
            if self.atx and r.random() < 0.3:
                hold = r.random() < 0.3
                a = {'s': 'A', 't': 'check', 'kind': 'curdecl', 'hold': hold, **self.params()}
                a['q'] = r.randrange(len(cursor_queries(self.c, a)))
                self.out.append(a)
                self.pc = {'hold': hold}
        elif r.random() < 0.7:
            self.out.append({'s': 'A', 't': 'check', 'kind': 'curfetch', 'ops': [r.choice(MOVES) for _ in range(r.randint(1, 4))]})
        else:
            self.out.append({'s': 'A', 't': 'check', 'kind': 'curclose'})
            self.pc = None

    def check(self, s):
        r = self.r
        if not self.only:
            super().check(s)
        w = r.random()
        if self.only in ('', 'cursor') and w < (0.2 if not self.only else 0.9):
            self.shape(s, 'cursor')
        elif self.only in ('', 'prep') and (w < 0.5 if not self.only else w < 0.9):
            self.shape(s, 'prep')
        if s == 'A' and self.only in ('', 'persist'):
            self.persist()
        if self.det is not None and r.random() < 0.5:
            self.out.append({'s': 'A', 't': 'sql', 'sql': f"ALTER TABLE t ATTACH PARTITION t_p{self.det} FOR VALUES WITH (MODULUS {self.c.parts}, REMAINDER {self.det})"})
            self.det = None
            if not self.only:
                super().check('A')
