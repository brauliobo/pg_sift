#!/usr/bin/env python3
"""The slope of a case between two sizes, from the records of one run at each (the quick release profile: 100,000 and 300,000 rows instead of 1,000,000 and 10,000,000).

    python3 -I slope.py SMALL.tsv LARGE.tsv

For each statement present in both: the time of pg_sift (the sift arm; a load's wall time, a read's execution time) and of the faster of PostgreSQL's plans (the heap arm and, where the table
has one, the b-tree plan the planner kept) at each size, and the growth of each: LARGE / SMALL. A heap scan grows with the rows (the ratio of the sizes); pg_sift grows slower where the
units it skips grow with them, and a case whose pg_sift time grows faster than the rows is marked `faster than the rows`: the claim that the time grows slower than the rows is the one
this table checks. A case with no PostgreSQL plan to compare (a build) shows its own growth alone. Times under MIN_MS at the small size are not read as a slope (the clock reads of a
statement of 50 microseconds are most of it).
"""
import os
import statistics
import sys

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
import schema  # noqa: E402

MIN_MS = 0.2


def times(records):
    """{statement: (rows, pg_sift ms, other ms or None)} of the warm rows: the medians of the sift arm and of the faster of the heap arm and the default plan when it is a b-tree"""
    cells = {}
    for r in records:
        if r['mode'] == 'unavailable' or r['temp'] != 'warm':
            continue
        t = r['wall_ms'] if r['kind'] != 'read' else r.get('exec_ms')
        if t is None:
            continue
        side = None
        if r['mode'] == 'forced' and r['arm'] == 'sift' and r['arm_option'] == 'default':
            side = 'sift'
        elif r['mode'] == 'forced' and r['arm'] in ('heap', 'btree') and r['arm_option'] == 'default':
            side = 'other'
        elif r['mode'] == 'default' and r['structure'] in ('btree', 'heap'):
            side = 'other'
        if side:
            cells.setdefault((r['statement'], r['rows'], side, r['arm'] if r['mode'] == 'forced' else 'picked'), []).append(t)
    out = {}
    for (statement, rows, side, arm), v in cells.items():
        m = statistics.median(v)
        cur = out.setdefault(statement, [rows, None, None])
        if side == 'sift':
            cur[1] = m
        elif cur[2] is None or m < cur[2]:
            cur[2] = m
    return {s: tuple(v) for s, v in out.items() if v[1] is not None}


def slope(small, large):
    """[(statement, rows small, rows large, sift small, sift large, other small, other large, flag)]: the flag is `faster than the rows` for a pg_sift time that grew by more than the rows did"""
    a, b = times(small), times(large)
    out = []
    for s in sorted(set(a) & set(b)):
        (n1, s1, o1), (n2, s2, o2) = a[s], b[s]
        flag = 'faster than the rows' if s1 >= MIN_MS and n2 > n1 and s2 / s1 > n2 / n1 else ''
        out.append((s, n1, n2, s1, s2, o1, o2, flag))
    return out


def table(rows):
    fmt = lambda x: ('%.3g' % x) if x < 100 else '{:,.0f}'.format(x)   # noqa: E731
    lines = ['| case | rows | pg_sift ms | growth | other plan ms | growth | |', '|---|---:|---|---:|---|---:|---|']
    for s, n1, n2, s1, s2, o1, o2, flag in rows:
        other = ('%s -> %s | %.1fx' % (fmt(o1), fmt(o2), o2 / o1)) if o1 and o2 else '- | -'
        lines.append('| `%s` | %s -> %s | %s -> %s | %.1fx | %s | %s |' % (s, '{:,}'.format(n1), '{:,}'.format(n2), fmt(s1), fmt(s2), s2 / s1, other, flag))
    return '\n'.join(lines)


def main(argv):
    if len(argv) != 2:
        sys.exit(__doc__)
    small, large = (schema.read(p) for p in argv)
    rows = slope(small, large)
    print(table(rows))
    over = [r for r in rows if r[7]]
    print('slope: %d cases at both sizes, %d with a pg_sift time that grew faster than the rows (%s)' % (len(rows), len(over), ', '.join(r[0] for r in over) or 'none'), file=sys.stderr)
    return 0


if __name__ == '__main__':
    sys.exit(main(sys.argv[1:]))
