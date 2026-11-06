# A failing script cut down to the actions that still fail, and written as a reproducer.

import dataclasses
import json
import os

from .gen import setup_sql
from .run import PLAIN, SCAN, build_base, check_sql, run
from .session import create_db, psql

MAX_RUNS = 160


class Replayer:
    """Runs a script on a database cloned from a template of the configuration (a clone of a database that never ran a session: the state a seed starts from)"""

    def __init__(self, c, prefix):
        self.c, self.prefix, self.runs, self.tpl = c, prefix, 0, None

    def template(self, c):
        self.tpl = f'{self.prefix}_tpl'
        build_base(c, self.tpl)

    def fails(self, c, acts):
        if self.runs >= MAX_RUNS:
            return False
        self.runs += 1
        db = f'{self.prefix}_w'
        create_db(db, self.tpl)
        return run(c, acts, db)[0] is not None


def ddmin(body, final, test):
    n = 2
    while len(body) >= 2:
        size = max(len(body) // n, 1)
        cuts = [body[i:i + size] for i in range(0, len(body), size)]
        for i in range(len(cuts)):
            rest = [a for j, ch in enumerate(cuts) if j != i for a in ch]
            if test(rest + [final]):
                body, n = rest, max(n - 1, 2)
                break
        else:
            if size == 1:
                break
            n = min(n * 2, len(body))
    return body


def shrink(c, acts, at, prefix):
    """-> (config, actions) of the smallest script found that fails"""
    rp = Replayer(c, prefix)
    rp.template(c)
    acts = acts[:at + 1]
    if not rp.fails(c, acts):
        print(f'  the failure does not repeat on a clone of a fresh database: not shrunk')
        return c, acts
    final = acts[-1]
    body = ddmin(acts[:-1], final, lambda a: rp.fails(c, a))
    for change in ({'rows': max(c.rows // 4, 1000)}, {'trigger': 'none'}, {'fillfactor': 100}, {'wbuf': False}, {'warm': False}):
        c2 = dataclasses.replace(c, **change)
        if c2 != c:
            rp.template(c2)
            if rp.fails(c2, body + [final]):
                c = c2
            else:
                rp.template(c)
    body = ddmin(body, final, lambda a: rp.fails(c, a))
    return c, body + [final]


def render_sql(c, acts, why):
    s = [f'-- txn_fuzz seed {c.seed}: {why}', f'-- config: {dataclasses.asdict(c)}',
         '-- session A runs the statements below in order; "-- [B]" lines belong to a second session open at the same time, "-- [F]" to a fresh connection that runs the check.',
         setup_sql(c), '\\c -', 'SET client_min_messages = warning;']
    for a in acts:
        tag = '' if a['s'] == 'A' else f"-- [{a['s']}] "
        if a['t'] == 'reconnect':
            s.append(f"{tag}\\c -" if a['s'] == 'A' else f'{tag}reconnects')
        elif a['t'] == 'sql':
            s.append(a['sql'] + ';' if a['s'] == 'A' and not a['sql'].endswith('\\.') else (tag + a['sql'].replace('\n', ' ') + ';' if a['s'] != 'A' else a['sql']))
        else:
            s.append(f"{tag}-- the check: its queries under plain, then the scan (the first line that differs is the finding)")
            s.append(check_sql(c, a))
    return '\n'.join(s) + '\n'


def write(out, c, acts, why):
    os.makedirs(out, exist_ok=True)
    base = f'{out}/txn_fuzz_{c.seed}'
    open(base + '.json', 'w').write(json.dumps({'config': dataclasses.asdict(c), 'actions': acts, 'why': why}, indent=1))
    open(base + '.sql', 'w').write(render_sql(c, acts, why))
    return base
