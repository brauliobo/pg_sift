# Shrinking a divergence to a reproducer and writing it to --out.
# It drops parts of the query and DML statements while the divergence stays.

import os

from .db import REF, create_db, data_sql, psql
from .registry import base_of
from .run import diverged, query_script, render_query, run_script

# ---------- shrinking ----------
def shrink_parts(db, qid, fam, parts, vs, nl, base):
    """Drops list elements and optional parts while the query still diverges in the state of db."""
    def reproduces(p):
        try:
            sql, ordered = render_query(fam, p)
        except Exception:
            return False
        res, _ = run_script(db, [], [query_script(qid, sql, ordered, base, vs)])
        return bool(diverged(res.get(qid, {})))

    changed = True
    while changed:
        changed = False
        for k, v in list(parts.items()):
            if isinstance(v, list):
                for i in range(len(v) - 1, -1, -1):
                    cand = dict(parts, **{k: v[:i] + v[i + 1:]})
                    if reproduces(cand):
                        parts, v, changed = cand, cand[k], True
            elif isinstance(v, bool) and v and k in ('neg', 'total'):
                cand = dict(parts, **{k: False})
                if reproduces(cand):
                    parts, changed = cand, True
            elif v and k in ('join', 'parent', 'order', 'spine', 'lookup', 'nested', 'grouped', 'offset') and not isinstance(v, list):
                cand = dict(parts, **{k: None if k not in ('join', 'parent') else '', })
                if k == 'nested':
                    cand[k] = False
                if k == 'offset':
                    cand[k] = 0
                if reproduces(cand):
                    parts, changed = cand, True
    return parts


def shrink_dml(base_db, work, hist, check):
    """Delta debugging of the DML statements before the query: drops chunks while check(db) still diverges."""
    n = len(hist) // 2 or 1
    while n >= 1 and hist:
        i, progressed = 0, False
        while i < len(hist):
            cand = hist[:i] + hist[i + n:]
            create_db(work, base_db)
            if check(work, cand):
                hist, progressed = cand, True
            else:
                i += n
        if not progressed:
            n //= 2
    return hist


def repro_sql(seed, rows, parts_n, hist, sql, base, ref_spec, var):
    name, spec = var
    return f"""-- pg_sift differential fuzz reproducer: seed {seed}, rows {rows}, partitions {parts_n}; the query returns other rows under the settings of
-- "{name}" than under the reference (sift.enable_agg off). Run it on a database with the extension, psql -f this file.
{data_sql(seed, rows, parts_n)}
-- the statements that ran before the query
{chr(10).join(s + ';' for s in hist)}
-- the settings, the reference first
CREATE TEMP TABLE r_ref AS
{sql};
\\set ON_ERROR_STOP off
{chr(10).join(f"SET {kv.split('=', 1)[0]} = '{kv.split('=', 1)[1]}';" for kv in (base + ';' + spec).split(';') if kv)}
CREATE TEMP TABLE r_got AS
{sql};
-- exact rows only on one side (floats can differ in the last digits: tolerance 1e-12 + 1e-9 relative)
SELECT 'only in reference' AS side, * FROM (TABLE r_ref EXCEPT ALL TABLE r_got) a
UNION ALL SELECT 'only in variant', * FROM (TABLE r_got EXCEPT ALL TABLE r_ref) b;
"""


def handle(seed, rows, parts_n, qid, fam, parts, vs, nl, hist, res, out_dir, tag):
    """Shrinks a divergence and writes the reproducer."""
    base_db = f'fz_{tag}_base'
    work = f'fz_{tag}_work'
    bad = diverged(res)
    # the variants that differ, the state replayed once
    vs_bad = [(n, s) for n, s in vs if n in bad]
    base = base_of(parts, nl)
    create_db(work, base_db)
    psql(work, ''.join(s + ';\n' for s in hist), check=False)
    p2 = shrink_parts(work, qid, fam, parts, vs_bad, nl, base)

    def check(db, h):
        psql(db, ''.join(s + ';\n' for s in h), check=False)
        sql, ordered = render_query(fam, p2)
        r2, _ = run_script(db, [], [query_script(qid, sql, ordered, base, vs_bad)])
        return bool(diverged(r2.get(qid, {})))

    h2 = hist
    if len(hist) <= 80:
        h2 = shrink_dml(base_db, work, hist, lambda db, h: check(db, h))
    sql, ordered = render_query(fam, p2)
    create_db(work, base_db)
    psql(work, ''.join(s + ';\n' for s in h2), check=False)
    r3, _ = run_script(work, [], [query_script(qid, sql, ordered, base, vs_bad)])
    bad3 = diverged(r3.get(qid, {}))
    name = next(iter(bad3), vs_bad[0][0]) if bad3 else vs_bad[0][0]
    spec = dict(vs)[name]
    text = repro_sql(seed, rows, parts_n, h2, sql, base, REF, (name, spec))
    detail = bad3.get(name, bad.get(name, ('', '', '')))[2] if bad3 else list(bad.values())[0][2]
    path = None
    if out_dir:
        os.makedirs(out_dir, exist_ok=True)
        path = f'{out_dir}/{qid}.sql'
        open(path, 'w').write(f'-- family {fam}; variant {name}; {detail[:600]}\n' + text)
    psql('postgres', f'DROP DATABASE IF EXISTS {work} WITH (FORCE)')
    return sql, name, detail, path, len(h2), len(hist)
