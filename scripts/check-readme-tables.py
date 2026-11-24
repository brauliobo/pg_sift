#!/usr/bin/env python3
"""Guard table readability in README.md and verify v6 benchmark snapshot alignment.

    scripts/check-readme-tables.py             exit 1 when any table in README.md (outside code blocks) has more than MAX_COLS columns,
                                               a header word longer than MAX_WORD characters, a cell longer than MAX_CELL characters,
                                               an empty cell, when a table in a <details> expander has more than MAX_ROWS rows, when an expander lacks
                                               its <summary> or the blank lines after </summary> and before </details> (without them GitHub shows the table as text),
                                               or when <!-- bench:v6-readme:begin --> disagrees with docs/results/v6-readme.tsv (if present)
    scripts/check-readme-tables.py --selftest  proves each rule fails on a table that breaks it
"""
import csv
import os
import re
import sys

REPO = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
MAX_COLS = 5
MAX_ROWS = 6
MAX_WORD = 16
MAX_CELL = 140


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


def visible_len(cell):
    """Display character width of a Markdown table cell (excluding link URLs)."""
    plain = re.sub(r'\[([^\]]+)\]\([^)]*\)', r'\1', cell)
    plain = re.sub(r'<a [^>]*></a>', '', plain)
    return len(plain.strip())


def expanders(text):
    """the <details> expanders of the page, outside code blocks"""
    return re.findall(r'<details.*?</details>', re.sub(r'^```.*?^```', '', text, flags=re.S | re.M), re.S)


def expander_problems(text):
    """a table in an expander is a mini-table (at most MAX_ROWS rows), and the blank lines that let GitHub render Markdown inside the HTML are there"""
    bad = []
    for e in expanders(text):
        summary = re.search(r'<summary>(.*?)</summary>', e, re.S)
        title = re.sub(r'<[^>]+>', '', summary.group(1)).strip()[:40] if summary else e[:40]
        if not summary or not e.endswith('\n\n</details>') or not e.split('</summary>', 1)[1].startswith('\n\n'):
            bad.append('expander "%s" needs a <summary>, a blank line after </summary> and a blank line before </details>' % title)
        bad += ['table of %d rows in expander "%s" (at most %d)' % (len(t) - 1, title, MAX_ROWS) for t in tables(e) if len(t) - 1 > MAX_ROWS]
    return bad


def problems(text):
    bad = expander_problems(text)
    for t in tables(text):
        head = t[0]
        if len(head) > MAX_COLS:
            bad.append('%d columns (at most %d): %s' % (len(head), MAX_COLS, ' | '.join(head)[:80]))
        for h in head:
            for w in re.split(r'<br>|[\s-]+', re.sub(r'\]\([^)]*\)|[`*\[\]]', '', h)):
                if len(w) > MAX_WORD:
                    bad.append('header word of %d characters (at most %d): %s' % (len(w), MAX_WORD, w))
        for r in t[1:]:
            if '' in r and not (len(r) == 2 and r[0].startswith('**') and r[0].endswith('**') and r[1] == ''):
                bad.append('an empty cell in the row: ' + ' | '.join(r)[:80])
            for c in r:
                n = visible_len(c)
                if n > MAX_CELL:
                    bad.append('cell of %d characters (at most %d): %s' % (n, MAX_CELL, c[:80]))
    return bad


TRIPLE = re.compile(r'\**.+ · .+ · .+')
BARE = re.compile(r'\**[\d,.]+( \([\d,.-]+\))?\**')


def triples(text):
    """For legacy docs/perf/AGGREGATES.md blocks using `T · C · M` cells only."""
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
    """Check <!-- bench:v6-readme:begin --> against docs/results/v6-readme.tsv when present."""
    block = re.search(r'<!-- bench:v6-readme:begin.*?-->\n(.*?)<!-- bench:v6-readme:end', text, re.S)
    if not block:
        return []
    tsv = os.path.join(REPO, 'docs', 'results', 'v6-readme.tsv')
    if not os.path.exists(tsv):
        return []
    with open(tsv, encoding='utf-8') as f:
        rows = list(csv.DictReader((l for l in f if l.strip() and not l.startswith('#')), delimiter='\t'))
    if not rows:
        return []
    tlist = tables(block.group(1))
    if not tlist:
        return ['v6-readme block has no table']
    got = {r[0]: r for r in tlist[0][1:]}
    sift_col = next((i for i, h in enumerate(tlist[0][0]) if 'pg_sift' in h), 1)
    bad = []
    for row in rows:
        w = row.get('workload', '')
        if w not in got:
            bad.append('v6-readme missing workload row: %s' % w)
            continue
        if row.get('sift_v6') and got[w][sift_col] != row['sift_v6']:
            bad.append('%s: README has %s, v6-readme.tsv says %s' % (w, got[w][sift_col], row['sift_v6']))
    return bad


def selftest():
    ok = '| System | Metric A | Metric B |\n|---|---:|---:|\n| pg_sift | 1.2 ms | 3.4 ms |\n'
    wide = '| c1 | c2 | c3 | c4 | c5 | c6 |\n|---|---|---|---|---|---|\n| 1 | 2 | 3 | 4 | 5 | 6 |\n'
    long_cell = '| System | Notes |\n|---|---|\n| pg_sift | ' + ('x' * (MAX_CELL + 5)) + ' |\n'
    rows = lambda n: ok.split('\n')[0] + '\n|---|---:|---:|\n' + '| pg_sift | 1.2 ms | 3.4 ms |\n' * n
    expander = lambda body, open_='': '<details%s>\n<summary><b>Title</b> <code>x</code></summary>\n\n%s\n</details>\n' % (open_, body)
    cases = (
        ('a table that fits', ok, ''),
        ('a mini-table in an expander (6 rows)', expander(rows(MAX_ROWS)), ''),
        ('a mini-table in an open expander', expander(rows(MAX_ROWS), ' open'), ''),
        ('a table of more rows outside an expander', rows(MAX_ROWS + 1), ''),
        ('a table of too many rows in an expander', expander(rows(MAX_ROWS + 1)), 'rows in expander'),
        ('a wide table in an expander', expander(wide), 'columns'),
        ('no blank line after </summary>', expander(rows(2)).replace('</summary>\n\n', '</summary>\n'), 'blank line'),
        ('no blank line before </details>', expander(rows(2)).replace('\n\n</details>', '\n</details>'), 'blank line'),
        ('an expander without a summary', '<details>\n\n' + rows(2) + '\n</details>\n', 'summary'),
        ('too many columns (>5)', wide, 'columns'),
        ('a long header word (>16)', ok.replace('Metric A', 'a' * (MAX_WORD + 1)), 'header word'),
        ('a header that wraps', ok.replace('Metric A', 'a' * 9 + '<br>' + 'b' * 9), ''),
        ('a long cell (>140)', long_cell, 'cell of'),
        ('an empty cell', ok.replace('| 1.2 ms |', '|  |'), 'empty cell'),
        ('a table in a code block', '```\n' + wide + '```\n', ''),
    )
    for what, text, words in cases:
        got = ' '.join(problems(text))
        if (words and words not in got) or (not words and got):
            sys.exit('selftest: %s: expected %r, got %r' % (what, words, got))
    print('check-readme-tables: selftest passed (5-col cap, 6-row cap in expanders, expander blank lines, 140-char cell cap, 16-char header word, empty cell, code block skip)')


if __name__ == '__main__':
    if sys.argv[1:] == ['--selftest']:
        selftest()
        sys.exit(0)
    text = open(os.path.join(REPO, 'README.md'), encoding='utf-8').read()
    bad = problems(text) + numbers(text)
    perf_page = os.path.join(REPO, 'docs', 'perf', 'AGGREGATES.md')
    if os.path.exists(perf_page):
        bad += triples(open(perf_page, encoding='utf-8').read())
    if bad:
        sys.exit('check-readme-tables:\n  ' + '\n  '.join(bad))
    print('check-readme-tables: %d tables, <= %d columns, <= %d chars/cell, header words <= %d chars' %
          (len(tables(text)), MAX_COLS, MAX_CELL, MAX_WORD))
