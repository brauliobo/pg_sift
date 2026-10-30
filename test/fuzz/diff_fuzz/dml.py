# The DML between groups of queries: dirty rows, write-buffer rows, VACUUM and the per-family statements.
# It runs on the heap tables and, with --table, on the sift_tables.

from . import tablemode
from .fam_units import KCUT_KEYS, WINDOW_COLS

# ---------- DML (dirty rows and the write buffer) ----------
def dml(r, ctx):
    """One random statement; ids of new rows come from the counters of ctx."""
    m, k = r.choice([40, 97, 211, 500]), None
    k = r.randrange(m)
    ids = f'id % {m} = {k}'
    e = r.randrange(16)
    t = r.choice(['events', 'events', 'events', 'ev_part'])
    if e == 0:
        return f'UPDATE {t} SET amount = amount + {r.randrange(1, 9)} WHERE {ids}'
    if e == 1:
        return f"UPDATE {t} SET kind = 'k{r.randrange(5)}' WHERE {ids}"
    if e == 2:
        return f'UPDATE {t} SET grp = {r.randrange(12)} WHERE {ids}'
    if e == 3:
        return f'UPDATE {t} SET ts = ts + {r.randrange(1, 40)} WHERE {ids}'
    if e == 4:
        return f'UPDATE {t} SET val = {r.choice(["NULL", "val * 2", "val + 1", "0.1"])} WHERE {ids}'
    if e == 5 and t == 'events':
        return f'UPDATE events SET big = big + {r.randrange(1, 9)} WHERE {ids}'
    if e == 6 and t == 'events':
        return f"UPDATE events SET metrics = jsonb_set(metrics, '{{m{r.randrange(1, 4)}}}', '{r.choice(['7', '1.5', chr(34) + 'zz' + chr(34), 'null', 'true'])}') WHERE metrics IS NOT NULL AND {ids}"
    if e == 7 and t == 'events':
        return f"UPDATE events SET doc = jsonb_set(doc, '{{parts,0,cat}}', '\"cat_{r.randrange(7)}\"') WHERE doc IS NOT NULL AND {ids}"
    if e == 8:
        return f'UPDATE {t} SET parent_id = {r.choice(["NULL", "parent_id + 1"])} WHERE {ids}'
    if e in (9, 10):
        return f'DELETE FROM {t} WHERE {ids}'
    if e in (11, 12, 13):
        n = r.choice([5, 40, 200])
        if t == 'events':
            lo = ctx['next_ev']; ctx['next_ev'] += n
            return f'INSERT INTO events SELECT * FROM fz.gen_events({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
        lo = ctx['next_pt']; ctx['next_pt'] += n
        return f'INSERT INTO ev_part SELECT * FROM fz.gen_part({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
    if e == 14:
        return r.choice([f'UPDATE items SET enabled = NOT enabled WHERE {ids}', f'UPDATE dims SET score = score + 1 WHERE a % {m % 50 + 3} = 0',
                         'DELETE FROM dims WHERE a = %d' % r.randrange(200), "UPDATE lookup SET derived_cat = 'm9' WHERE kind = 'k%d'" % r.randrange(5)])
    return r.choice(['VACUUM events', 'VACUUM ev_part', 'VACUUM (FREEZE) events', 'ANALYZE events'] + (dml_table(r, ctx, t, ids) if tablemode.TABLE else []))


TABLE_GEN = {'events': ('gen_events', 'next_ev'), 'ev_part': ('gen_part', 'next_pt'), 'nzf': ('gen_nzf', 'next_nz'), 'exf': ('gen_exf', 'next_ex')}


def bulk_insert(r, ctx, t):
    """A COPY of a few rows: a table without a unique index takes it as an unfrozen group (the rows carry their transaction) and not in the write buffer."""
    gen, counter = TABLE_GEN[t]
    n = r.choice([5, 40, 200])
    lo = ctx[counter]; ctx[counter] += n
    return f"SELECT fz.bulk_insert('{t}', '{gen}', {lo}, {lo + n - 1}, {r.randrange(1, 9)})"


def dml_table(r, ctx, t, ids):
    """Statements that matter to the table only: the maintenance of its segments, a rewrite, COPY, and writes that roll back (their rows stay in the write buffers and the
    tombstones until a VACUUM drops them) or half roll back."""
    m = "sift.table_maintain('%s')" % r.choice(['events', 'events', 'kc', 'ev_part'])
    n = r.choice([5, 40])
    lo = ctx['next_ev'] if t == 'events' else ctx['next_pt']
    ctx['next_ev' if t == 'events' else 'next_pt'] += n
    ins = f"INSERT INTO {t} SELECT * FROM fz.{'gen_events' if t == 'events' else 'gen_part'}({lo}, {lo + n - 1}, {r.randrange(1, 9)})"
    return [f'SELECT {m}', f'SELECT {m}', 'VACUUM FULL events', f'BEGIN; {ins}; ROLLBACK', f'BEGIN; UPDATE {t} SET amount = amount + 1 WHERE {ids}; ROLLBACK',
            f'BEGIN; DELETE FROM {t} WHERE {ids}; ROLLBACK',
            f'BEGIN; {ins}; SAVEPOINT s; UPDATE {t} SET grp = 3 WHERE {ids}; ROLLBACK TO s; DELETE FROM {t} WHERE {ids}; COMMIT',
            bulk_insert(r, ctx, t), f'BEGIN; {bulk_insert(r, ctx, t)}; ROLLBACK']


KCUT_COLS = {'ki': ['ki + 1', 'NULL', '39 - ki', '7'], 'kd': ["kd + {d}", "'infinity'", "'-infinity'", 'NULL', "date '2025-03-01'"],
             'kt': ["kt + interval '{d} days'", "'infinity'", 'NULL', "timestamp '2025-02-02 02:02:02'"], 'kn': ['kn * 2', "'NaN'", 'NULL', '1.5'],
             'kf': ['kf + 1', "'-0'", "'NaN'", 'NULL', "'Infinity'"]}
KCUT_KEY_OF = {'a': 'ki, kd', 'b': 'kt, kn', 'c': 'kf, ki', 'd': 'ki, kd'}


def dml_kcut(r, ctx):
    """Statements on the key-cut tables: churn of the keys (rows move between units), deletes, inserts, VACUUM, and every way the units are written again: a promotion of the write
    buffer, a merge, a refresh, a REINDEX, and a merge of units written before the key was declared (a key that is not in the order of the segments it merges)."""
    x = r.choice('abcd')
    t, idx = f'kcut_{x}', f'kcut_{x}_idx'
    m = r.choice([13, 40, 97, 211])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(16)
    if e < 4:
        c = r.choice(KCUT_KEYS)
        return [f'UPDATE {t} SET {c} = {r.choice(KCUT_COLS[c]).format(d=r.randrange(1, 40))} WHERE {ids}']
    if e == 4:
        return [f'UPDATE {t} SET amount = amount + {r.randrange(1, 9)}, val = val + 1 WHERE {ids}']
    if e in (5, 6):
        return [f'DELETE FROM {t} WHERE {ids}']
    if e in (7, 8, 9):
        n = r.choice([5, 40, 200])
        lo = ctx['next_kc']; ctx['next_kc'] += n
        return [f'INSERT INTO {t} SELECT * FROM fz.gen_kcut({lo}, {lo + n - 1}, {r.randrange(1, 9)})']
    if e == 10:
        return [f'VACUUM {t}']
    if e == 11:
        return [f"SELECT sift.col_promote('{idx}')"]
    if e == 12:
        return [r.choice([f"SELECT sift.col_merge('{idx}', true)", f"SELECT sift.col_merge('{idx}', true)", f"SELECT count(*) FROM sift.col_refresh('{idx}')", f'REINDEX INDEX {idx}'])]
    if e == 13:
        # segments written without the key, merged into units cut by it
        lo = ctx['next_kc']; ctx['next_kc'] += 60
        return [f'ALTER INDEX {idx} RESET (key_cut)', f'INSERT INTO {t} SELECT * FROM fz.gen_kcut({lo}, {lo + 59}, {r.randrange(1, 9)})', f"SELECT sift.col_promote('{idx}')",
                f"ALTER INDEX {idx} SET (key_cut = '{KCUT_KEY_OF[x]}')", f"SELECT sift.col_merge('{idx}', true)"]
    if e == 14:
        # another key: the segments cut by the old one stop a merge (the REINDEX hint of the error), so the index is built again
        return [f"ALTER INDEX {idx} SET (key_cut = '{r.choice(['ki', 'kd', 'kt', 'kn', 'kf', 'ki, kd', 'kd, kt', 'kn, kf', KCUT_KEY_OF[x]])}')", f'REINDEX INDEX {idx}']
    return [f'ANALYZE {t}']


def dml_floor(r, ctx):
    """Statements on ufl, a plain index whose promotions are cut by TID range once the least number of superblocks and the floor are lowered for the session (a testing-build setting): sparse
    updates and deletes, inserts, VACUUM, every way the units are written again, and a promotion with the floor off between the others, so that both cuts are among the segments."""
    m = r.choice([13, 40, 97, 211])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(14)
    st = ['SET sift.test_col_floor_sbs = 2', f'SET sift.col_unit_floor = {r.choice([64, 128, 256, 1024])}']
    if e < 4:
        c = r.choice(KCUT_KEYS)
        return st + [f'UPDATE ufl SET {c} = {r.choice(KCUT_COLS[c]).format(d=r.randrange(1, 40))} WHERE {ids}']
    if e == 4:
        return st + [f'UPDATE ufl SET amount = amount + {r.randrange(1, 9)}, val = val + 1 WHERE {ids}']
    if e in (5, 6):
        return st + [f'DELETE FROM ufl WHERE {ids}']
    if e in (7, 8):
        n = r.choice([5, 40, 200])
        lo = ctx['next_kc']; ctx['next_kc'] += n
        return st + [f'INSERT INTO ufl SELECT * FROM fz.gen_ufl({lo}, {lo + n - 1}, {r.randrange(1, 9)})']
    if e == 9:
        return st + ['VACUUM ufl']
    if e == 10:
        return st + ["SELECT sift.col_promote('ufl_idx')"]
    if e == 11:
        return st + ["SET sift.col_unit_floor = 0", f'UPDATE ufl SET amount = amount + 1 WHERE {ids}', 'VACUUM ufl', "SELECT sift.col_promote('ufl_idx')"]
    if e == 12:
        return st + [r.choice(["SELECT sift.col_merge('ufl_idx', true)", "SELECT sift.col_merge('ufl_idx', false)", "SELECT count(*) FROM sift.col_refresh('ufl_idx')", 'REINDEX INDEX ufl_idx'])]
    return st + ['ANALYZE ufl']


def dml_dead(r, ctx):
    """Statements on udd, a plain index with cubes and a rollup over rows of 3 kB (a dozen superblocks): deletes spread over the units and VACUUM with every page visible afterwards (the
    units with dead rows), sparse updates, inserts, and the promotions and merges that clean them again."""
    m = r.choice([13, 40, 97, 211])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(14)
    if e < 5:
        return [f'DELETE FROM udd WHERE {ids}', 'VACUUM (DISABLE_PAGE_SKIPPING) udd']
    if e == 5:
        return [f'DELETE FROM udd WHERE kind = \'k{r.randrange(5)}\' AND id % {m} < {r.randrange(1, m)}', 'VACUUM (DISABLE_PAGE_SKIPPING) udd']
    if e == 6:
        return [f'DELETE FROM udd WHERE ki IS NULL AND {ids}', 'VACUUM (DISABLE_PAGE_SKIPPING) udd']
    if e == 7:
        c = r.choice(KCUT_KEYS)
        return [f'UPDATE udd SET {c} = {r.choice(KCUT_COLS[c]).format(d=r.randrange(1, 40))} WHERE {ids}']
    if e == 8:
        return [f'UPDATE udd SET amount = amount + {r.randrange(1, 9)}, val = val + 1 WHERE {ids}', 'VACUUM (DISABLE_PAGE_SKIPPING) udd']
    if e in (9, 10):
        n = r.choice([5, 40, 200])
        lo = ctx['next_kc']; ctx['next_kc'] += n
        return [f'INSERT INTO udd SELECT * FROM fz.gen_ufl({lo}, {lo + n - 1}, {r.randrange(1, 9)})']
    if e == 11:
        return ['VACUUM udd']
    if e == 12:
        return [r.choice(["SELECT sift.col_promote('udd_idx')", "SELECT sift.col_merge('udd_idx', true)", "SELECT sift.col_merge('udd_idx', false)"])]
    return ['ANALYZE udd']


def dml_window(r, ctx):
    """Statements on kcut_w, whose units keep a sorted run of the range leaves: updates that move rows in and out of a range, deletes, inserts and every way the units are written again."""
    m = r.choice([13, 40, 97, 211])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(12)
    if e < 4:
        c = r.choice(WINDOW_COLS)
        return [f'UPDATE kcut_w SET {c} = {r.choice(KCUT_COLS[c]).format(d=r.randrange(1, 40))} WHERE {ids}']
    if e in (4, 5):
        return [f'DELETE FROM kcut_w WHERE {ids}']
    if e in (6, 7):
        n = r.choice([5, 40, 200])
        lo = ctx['next_kc']; ctx['next_kc'] += n
        return [f'INSERT INTO kcut_w SELECT * FROM fz.gen_kcut({lo}, {lo + n - 1}, {r.randrange(1, 9)})']
    if e == 8:
        return ['VACUUM kcut_w']
    if e == 9:
        return ["SELECT sift.col_promote('kcut_w_idx')"]
    if e == 10:
        return [r.choice(["SELECT sift.col_merge('kcut_w_idx', true)", 'REINDEX INDEX kcut_w_idx'])]
    return ['ANALYZE kcut_w']


def dml_nullzone(r, ctx):
    """Soft deletes and restores, NULLing and filling of the columns the family tests, inserts, deletes and VACUUM on nzf: units whose NULL count changes under the zones."""
    m = r.choice([13, 40, 97, 211])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(14)
    if e < 3:
        return f'UPDATE nzf SET deleted_at = now() WHERE {ids}'
    if e == 3:
        return f'UPDATE nzf SET deleted_at = NULL WHERE {ids}'
    if e == 4:
        return f'UPDATE nzf SET deleted_at = timestamptz \'2025-08-01\' WHERE id BETWEEN {r.randrange(1, 800)} AND {r.randrange(800, 2400)}'
    if e == 5:
        return f"UPDATE nzf SET seen_at = {r.choice(['NULL', 'date ' + chr(39) + '2025-05-05' + chr(39)])} WHERE {ids}"
    if e == 6:
        return f"UPDATE nzf SET tag = {r.choice(['NULL', chr(39) + 'zz' + chr(39)])} WHERE {ids}"
    if e == 7:
        return f"UPDATE nzf SET score = {r.choice(['NULL', chr(39) + 'NaN' + chr(39), '1.5'])} WHERE {ids}"
    if e == 8:
        return f'UPDATE nzf SET kind = NULL, amount = amount + 1 WHERE {ids}'
    if e == 9:
        return f'DELETE FROM nzf WHERE {ids}'
    if e in (10, 11):
        n = r.choice([5, 40, 300])
        lo = ctx['next_nz']; ctx['next_nz'] += n
        return f'INSERT INTO nzf SELECT * FROM fz.gen_nzf({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
    if e == 12:
        return r.choice(['VACUUM nzf', 'VACUUM (FREEZE) nzf'])
    if tablemode.TABLE:
        return r.choice(["SELECT sift.table_maintain('nzf')", "SELECT sift.table_maintain('nzf')", 'VACUUM FULL nzf', bulk_insert(r, ctx, 'nzf'), bulk_insert(r, ctx, 'nzf')])
    return r.choice(["SELECT sift.col_promote('nzf_col')", "SELECT sift.col_merge('nzf_col', true)"])


def dml_arrlen(r, ctx):
    """Arrays set to every shape (NULL, empty, longer, two dimensions), deleted, inserted, then the write buffer promoted, merged or the index rebuilt: lanes written by the buffer, the merge and the build."""
    m = r.choice([13, 40, 97, 211])
    ids = f'id % {m} = {r.randrange(m)}'
    e = r.randrange(15)
    if e == 0:
        return f'UPDATE arf SET ia = NULL, ta = NULL WHERE {ids}'
    if e == 1:
        return f"UPDATE arf SET ta = '{{}}', tb = '{{}}', ia = '{{}}' WHERE {ids}"
    if e == 2:
        return f"UPDATE arf SET ta = ARRAY['t1', 't2', 't3'], ia = ia || {r.randrange(9)} WHERE {ids}"
    if e == 3:
        return f"UPDATE arf SET ma = ARRAY[[{r.randrange(9)}, 2], [3, 4], [5, 6]], na = ARRAY[1, 2, 3, 4] WHERE {ids}"
    if e == 4:
        return f"UPDATE arf SET ma = NULL, na = '{{}}' WHERE {ids}"
    if e == 5:
        return f'UPDATE arf SET amount = amount + 1 WHERE {ids}'
    if e == 6:
        return f'DELETE FROM arf WHERE {ids}'
    if e in (7, 8):
        n = r.choice([5, 40, 300])
        lo = ctx['next_ar']; ctx['next_ar'] += n
        return f'INSERT INTO arf SELECT * FROM fz.gen_arf({lo}, {lo + n - 1}, {r.randrange(1, 9)})'
    if e == 9:
        return r.choice(['VACUUM arf', 'VACUUM (FREEZE) arf'])
    if e == 13:
        return f"UPDATE arf SET ta = ARRAY['t{r.randrange(32)}', 't{r.randrange(32)}', 't3'], tb = ARRAY['u1', NULL, 'u1'] WHERE {ids}"
    if e == 14:
        k = r.randrange(20)
        return f"UPDATE arf SET ia = ARRAY[{k}, {k}, {k + 1}], ta = ARRAY[['t1', 't2'], ['t{r.randrange(32)}', 't4']] WHERE {ids}"
    return r.choice(["SELECT sift.col_promote('arf_col')", "SELECT sift.col_merge('arf_col', true)", 'REINDEX INDEX arf_col'])
