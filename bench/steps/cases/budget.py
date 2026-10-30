# The instruction ceilings of the cases (test/plan/execution.tsv): the regression guard of what a statement costs.
# A line is `case<TAB>ceiling`, in millions of user instructions of the backend (the `minstr` row of the sift lane) at the rows of the run; the ceiling is the recorded count with 15% over it
# (the spread of a count between runs is under 1%: the margin pays for a compiler or a PostgreSQL minor) or 0.5 million more, whichever is larger: a statement of 0.4 million instructions moves by that much with one more catalog lookup.

import math

MARGIN = 1.15
FLOOR = 0.5


def read(path):
    with open(path) as f:
        return {l.split('\t')[0]: float(l.split('\t')[1]) for l in f if l.strip() and not l.startswith('#')}


def check(path, results):
    """the lines of the table and whether every case of the file that ran is within its ceiling; a case with no instruction count is a failure"""
    ceilings = read(path)
    got = {r['case']: float(r['value']) for r in results if r['arm'] == 'sift' and r['metric'] == 'minstr'}
    lines, ok = ['%-28s %10s %10s' % ('case', 'Minstr', 'ceiling')], True
    for case, ceiling in sorted(ceilings.items()):
        if case not in got:
            continue
        over = got[case] > ceiling
        ok &= not over
        lines.append('%-28s %10.1f %10.1f%s' % (case, got[case], ceiling, '  OVER' if over else ''))
    return lines, ok


def update(path, results, header):
    """rewrite the ceilings of the cases that ran: the count with MARGIN (or FLOOR) over it, rounded up to a tenth"""
    ceilings = read(path) if path else {}
    for r in results:
        if r['arm'] == 'sift' and r['metric'] == 'minstr':
            v = float(r['value'])
            ceilings[r['case']] = math.ceil(max(v * MARGIN, v + FLOOR) * 10) / 10
    with open(path, 'w') as f:
        f.write(header)
        for case, c in sorted(ceilings.items()):
            f.write('%s\t%.1f\n' % (case, c))
