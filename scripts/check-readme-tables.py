#!/usr/bin/env python3
"""The tables of README.md stay inside its 880 px column, the milliseconds table still says what docs/results/store.json says, and a result cell carries its CPU and IO.

    scripts/check-readme-tables.py             exit 1 when a table (outside code blocks) has more than MAX_COLS columns, a header word longer than MAX_WORD characters
                                               (a header wraps on spaces, hyphens and <br>), or an empty cell, or when a cell of the `numbers` tables differs from the stored median;
                                               in a table of README.md, docs/BENCHMARKS.md and docs/perf/AGGREGATES.md with `T · C · M` cells (steps/triple.py), a cell that is a bare time
                                               with no CPU and IO, or an `n/a` measure whose reason the legend under the table does not give
    scripts/check-readme-tables.py --selftest  proves each rule fails on a table that breaks it

The tables are written by bench/steps/mdtable.py (a row for each system, a column for each benchmark, split in tables of at most 6 columns); the other cells of the generated
blocks are checked by `scripts/gen-docs.py --check`, which renders every block again from docs/results.
"""
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAX_COLS = 8   # the system and up to 7 benchmarks
MAX_WORD = 16


def tables(text):
    """[[row cells]] of every table outside code blocks"""
    out, cur, fence = [], None, False
    for line in text.splitlines():
        fence ^= line.startswith('```')
        if not fence and line.startswith('|'):
            if cur is None:
                cur = []
                out.append(cur)
            cells = [c.strip() for c in re.split(r'(?<!\\)\|', line.strip()[1:-1])]
            if not all(re.fullmatch(r':?-+:?', c) for c in cells):
                cur.append(cells)
        else:
            cur = None
    return out


def problems(text):
    bad = []
    for t in tables(text):
        head = t[0]
        if len(head) > MAX_COLS:
            bad.append('%d columns (at most %d): %s' % (len(head), MAX_COLS, ' | '.join(head)[:80]))
        for h in head:
            for w in re.split(r'<br>|[\s-]+', re.sub(r'\]\([^)]*\)|[`*\[\]]', '', h)):
                if len(w) > MAX_WORD:
                    bad.append('header word of %d characters (at most %d): %s' % (len(w), MAX_WORD, w))
        bad += ['an empty cell in the row: ' + ' | '.join(r)[:80] for r in t[1:] if '' in r]
    return bad


TRIPLE = re.compile(r'\**.+ · .+ · .+')   # a cell of steps/triple.py: time · CPU · IO (also the ranges of the At a glance cells)
BARE = re.compile(r'\**[\d,.]+( \([\d,.-]+\))?\**')   # a time on its own, with or without the range of the runs


def triples(text):
    """per generated block, the tables whose result cells are `T · C · M`: no bare time among them, and the block gives the reason of an `n/a` measure (`n/a`: ...)"""
    bad = []
    for block in re.split(r'(?=<!-- bench:[\w-]+:begin)', text):
        for t in tables(block):
            cells = [c for r in t[1:] for c in r[1:]]
            if not any(TRIPLE.match(c) for c in cells):
                continue
            bad += ['a time without CPU and IO in the table of %s: %s' % (t[0][1], c) for c in cells if BARE.fullmatch(c)]
            if any(TRIPLE.match(c) and 'n/a' in c for c in cells) and '`n/a`: ' not in block:
                bad.append('an n/a measure in the table of %s and no reason in its block (`n/a`: ...)' % t[0][1])
    return bad


def numbers(text):
    """the cells of the two `numbers` tables against the medians of docs/results/store.json"""
    sys.path.insert(0, os.path.join(REPO, 'bench', 'steps'))
    import bench_charts
    import extras
    d = bench_charts.load(os.path.join(REPO, 'docs', 'results'))
    block = re.search(r'<!-- bench:numbers:begin.*?-->\n(.*?)<!-- bench:numbers:end', text, re.S).group(1)
    name = {v + '†' * (e == 'duckdb'): e for e, v in bench_charts.SHORT.items()}
    bad = []
    for part, (S, D, size) in zip(block.split('The same questions on 10,000,000 orders'), ((d['S'], d['D1'], 1_000_000), (d['SS'], d['D10'], 10_000_000))):
        tier = S.tier_of(size, 3)
        for t in tables(part):
            for row in t[1:]:
                for q, cell in zip(t[0][1:], row[1:]):
                    v = bench_charts.lat(S, D, tier, name[row[0]], q)
                    if cell.replace('*', '').split(' · ')[0] != (extras.ms(v[0]) if v else 'n/a'):
                        bad.append('%s, %s at %s orders: %s, the results say %s' % (row[0], q, size, cell, v and extras.ms(v[0])))
    return bad


def selftest():
    ok = '| system | a b | c-d |\n|---|---:|---:|\n| x | 1 | 2 |\n'
    three = '| system | q |\n|---|---:|\n| x | 1.5 · 2 · n/a |\n| y | 3 · 4 · 5 (2.0x · 2.0x · n/a) |\n\nEach cell is milliseconds · CPU milliseconds of the whole server · MB read. `n/a`: not measured under 3.7 yet.\n'
    for what, text, words in (('a table that fits', ok, ''), ('too many columns', '| s |' + ' a |' * 8 + '\n|---|\n| x |\n', 'columns'),
                              ('a long header word', ok.replace('a b', 'a' * 17), 'header word'), ('a header that breaks', ok.replace('a b', 'a' * 9 + '<br>' + 'b' * 9), ''),
                              ('an empty cell', ok.replace('| 1 |', '|  |'), 'empty cell'), ('a table in a code block', '```\n' + ok.replace('a b', 'a' * 17) + '```\n', ''),
                              ('cells with CPU and IO and a reason for the n/a', three, ''), ('a bare time among the cells', three.replace('3 · 4 · 5 (2.0x · 2.0x · n/a)', '3'), 'without CPU and IO'),
                              ('an n/a with no reason', three.replace(' `n/a`: not measured under 3.7 yet.', ''), 'no reason')):
        got = ' '.join(problems(text) + triples(text))
        if (words and words not in got) or (not words and got):
            sys.exit('selftest: %s: expected %r, got %r' % (what, words, got))
    print('check-readme-tables: selftest passed (a table that fits passes; too many columns, a long header word, an empty cell, a bare time and an n/a with no reason fail)')


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        selftest()
        sys.exit(0)
    text = open(os.path.join(REPO, 'README.md')).read()
    bad = problems(text) + numbers(text)
    for f in ('README.md', 'docs/BENCHMARKS.md', 'docs/perf/AGGREGATES.md'):
        bad += triples(text if f == 'README.md' else open(os.path.join(REPO, f)).read())
    if bad:
        sys.exit('check-readme-tables:\n  ' + '\n  '.join(bad))
    print('check-readme-tables: %d tables, none wider than %d columns, no header word over %d characters, the numbers tables match docs/results/store.json' % (len(tables(text)), MAX_COLS, MAX_WORD))
