# The episodes of a seed and the run of a script under the reference and the variants.
# A query that raises must raise the same SQLSTATE everywhere.

import random
import re

from . import tablemode
from .db import REF, psql
from .tablemode import TABLE_SKIP
from .fam_lists import dml_trail, dml_txn
from .fam_cte import dml_ctecopies, dml_dictcount, dml_exceptions, dml_flatkeys, dml_gorder, dml_keylist, dml_keytable
from .registry import FAMILIES, variants
from .dml import dml, dml_arrlen, dml_dead, dml_floor, dml_kcut, dml_nullzone, dml_window

# ---------- a run ----------
def sqlq(s):
    assert '$fzq$' not in s
    return f'$fzq${s}$fzq$'


def keycols(ordered):
    return 'NULL::int[]' if ordered else "'{}'::int[]"


def query_script(qid, sql, ordered, base, vs):
    out = [f'\\echo FZQ|{qid}', 'SELECT fz.start();', f"SELECT fz.ref('{qid}', {sqlq(sql)}, '{base};{REF}');"]
    for name, spec in vs:
        out.append(f"SELECT fz.var('{qid}', {sqlq(sql)}, {keycols(ordered)}, '{name}', '{base};{spec}');")
    return '\n'.join(out)


def lines_of(out):
    return [l for l in out.splitlines() if l.startswith('FZ|')]


def gen_query(seed, i, ctx, only):
    r = random.Random(seed * 1000003 + i)
    names = [n for n in FAMILIES if (not only or n in only) and not (tablemode.TABLE and n in TABLE_SKIP)]
    fam = r.choices(names, [FAMILIES[n][2] for n in names])[0]
    gen, render, _ = FAMILIES[fam]
    parts = gen(r, ctx)
    return fam, parts, variants(r, fam), r.random() < 0.3


def episodes(seed, nq, ctx, only, dmls=4):
    """[(dml statements, [query entries])]: 4-10 queries after 0-4 statements."""
    r = random.Random(seed * 7919 + 1)
    rk = random.Random(seed * 104729 + 3)
    rn = random.Random(seed * 1299709 + 5)
    rx = random.Random(seed * 15485863 + 7)
    rt = random.Random(seed * 32452843 + 17)
    rw = random.Random(seed * 32452843 + 11)
    ru = random.Random(seed * 49979687 + 13)
    rd = random.Random(seed * 67867979 + 19)
    rq = random.Random(seed * 86028121 + 23)
    rq2 = random.Random(seed * 94906249 + 29)
    ra = random.Random(seed * 122949829 + 31)
    rg = random.Random(seed * 32452843 + 13)
    rb = random.Random(seed * 122949829 + 31)
    rf = random.Random(seed * 179424673 + 37)
    rc = random.Random(seed * 217645177 + 43)
    rl = random.Random(seed * 198491317 + 41)
    done, eps = 0, []
    while done < nq:
        st = [dml(r, ctx) for _ in range(r.randrange(dmls + 1))]
        if not only or 'nullzone' in only:
            st += [dml_nullzone(rn, ctx) for _ in range(rn.randrange(dmls + 1))]
        if not tablemode.TABLE and (not only or 'arrlen' in only):
            st += [dml_arrlen(ra, ctx) for _ in range(ra.randrange(dmls + 1))]
        if not only or 'exceptions' in only:
            st += [dml_exceptions(rx, ctx) for _ in range(rx.randrange(dmls + 1))]
        if not tablemode.TABLE and (not only or 'trail' in only):
            st += [dml_trail(rt, ctx) for _ in range(rt.randrange(dmls + 1))]
        if not tablemode.TABLE and (not only or 'gorder' in only):
            st += [dml_gorder(rg, ctx) for _ in range(rg.randrange(dmls + 1))]
        if not tablemode.TABLE and (not only or 'keycut' in only):
            st += [q for _ in range(rk.randrange(dmls + 1)) for q in dml_kcut(rk, ctx)]
        if not tablemode.TABLE and (not only or 'window' in only):
            st += [q for _ in range(rw.randrange(dmls + 1)) for q in dml_window(rw, ctx)]
        if not tablemode.TABLE and (not only or 'keytable' in only):
            st += [dml_keytable(rq, ctx) for _ in range(rq.randrange(dmls + 1))]
        if not tablemode.TABLE and (not only or 'dictcount' in only):
            st += [dml_dictcount(rq2, ctx) for _ in range(rq2.randrange(dmls + 1))]
        if not tablemode.TABLE and (not only or 'txnmix' in only):
            st += [dml_txn(rb, ctx) for _ in range(rb.randrange(dmls + 1))]
        if not tablemode.TABLE and (not only or 'ctecopies' in only):
            st += [dml_ctecopies(rc, ctx) for _ in range(rc.randrange(dmls + 1))]
        if not tablemode.TABLE and (not only or 'flatkeys' in only):
            st += [dml_flatkeys(rf, ctx) for _ in range(rf.randrange(dmls + 1))]
        if not tablemode.TABLE and (not only or 'keylist' in only):
            st += [dml_keylist(rl, ctx) for _ in range(rl.randrange(dmls + 1))]
        if not only or 'floor' in only:
            st += [q for _ in range(ru.randrange(dmls + 1)) for q in dml_floor(ru, ctx)]
        if not only or 'dead' in only:
            st += [q for _ in range(rd.randrange(dmls + 1)) for q in dml_dead(rd, ctx)]
        qs = []
        for _ in range(min(nq - done, r.randrange(4, 11))):
            fam, parts, vs, nl = gen_query(seed, done, ctx, only)
            qs.append((f's{seed}q{done}', fam, parts, vs, nl))
            done += 1
        eps.append((st, qs))
    return eps


def render_query(fam, parts):
    sql, ordered = FAMILIES[fam][1](parts)
    return sql, ordered


CRASH = re.compile(r'server closed the connection|connection to server was lost|terminating connection because of crash|could not send data to server')
PRE = "SET statement_timeout = '10s';\nSET jit = off;\nLOAD 'pg_sift';\n"


def run_script(db, stmts, entries):
    """Runs DML statements then queries; returns {qid: {'ref': status, variant: (status, used, detail)}}."""
    text = PRE + ''.join(s + ';\n' for s in stmts) + '\n'.join(entries) + '\n'
    out, err = psql(db, text, check=False)
    res = {}
    for l in lines_of(out):
        f = l.split('|', 5) + [''] * 6
        q = res.setdefault(f[1], {})
        if f[2] == 'ref':
            q['ref'] = (f[3], f[4])
        else:
            q[f[2]] = (f[3], f[4], f[5])
    return res, err


def diverged(res):
    return {n: v for n, v in res.items() if n != 'ref' and v[0] == 'DIFF'}
