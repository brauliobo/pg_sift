#!/usr/bin/env python3
"""Markdown tables that fit a GitHub page (stdlib only). The layout rules and the budget are in AGENTS.md, "Tables in the docs".

`table` renders rows and, when they exceed the budget, splits the value columns over several tables that repeat the key columns;
`details` folds a block; `survey` and `lint` read the tables of a text back, which is what `scripts/gen-docs.py --tables` and `--check` use."""
import re

MAX_COLS, MAX_ROW, MAX_CELL = 6, 100, 40   # columns, characters of a source row, characters of a cell
NUMBER = re.compile(r'(\*\*)?([-+~]?\d|n/a|NA|error|-$)')


def line_of(cells):
    return '| ' + ' | '.join(cells) + ' |'


def table(head, body, keys=1):
    """head and body (lists of cell texts) as table lines. Columns after the first `keys` are values: when the rows exceed the budget they are
    split evenly over several tables that repeat the key columns (a long key column cannot be fixed that way: shorten its labels). A column is
    right-aligned when all its cells are numbers."""
    rows = [head] + body
    fits = lambda cols: len(cols) <= MAX_COLS and all(len(line_of([r[j] for j in cols])) <= MAX_ROW for r in rows)
    values = list(range(keys, len(head)))
    for parts in range(1, len(values) // 2 + 1):
        size = -(-len(values) // parts)
        chunks = [values[i:i + size] for i in range(0, len(values), size)]
        if all(fits(list(range(keys)) + c) for c in chunks):
            break
    else:
        chunks = [values]
    out = []
    for c in chunks:
        cols = list(range(keys)) + c
        align = ['---:' if all(NUMBER.match(r[j]) for r in body) else '---' for j in cols]
        out += [''] * bool(out) + [line_of([head[j] for j in cols]), '|' + '|'.join(align) + '|'] + [line_of([r[j] for j in cols]) for r in body]
    return out


def details(summary, lines):
    return ['<details><summary>%s</summary>' % summary, ''] + lines + ['', '</details>']


# ---- reading tables back -----------------------------------------------------------------------------------------------------------------

def split_row(line):
    return [c.strip() for c in re.split(r'(?<!\\)\|', line.strip().strip('|'))]


def survey(text):
    """(line, columns, longest row, longest cell, generated, header) of each table outside code fences; generated: inside a bench:, coverage: or gen: block"""
    out, cur, fence, gen = [], None, False, False
    for n, line in enumerate(text.splitlines(), 1):
        fence ^= line.lstrip().startswith('```')
        gen = (gen or bool(re.search(r'<!-- (?:bench:[\w-]+:begin|coverage:start|gen:\S+)', line))) and not re.search(r'<!-- (?:bench:[\w-]+:end|coverage:end|/gen:\S+)', line)
        if not fence and line.lstrip().startswith('|'):
            if cur is None:
                cur = [n, [], gen]
                out.append(cur)
            cur[1].append(line)
        else:
            cur = None
    return [(n, len(split_row(rows[0])), max(map(len, rows)), max(len(c) for r in rows if not re.fullmatch(r'[\s|:-]+', r) for c in split_row(r)), g, rows[0].strip())
            for n, rows, g in out]


def lint(text, name, generated_only=True):
    """one message for each table of the text over the budget (by default only those inside generated blocks)"""
    msgs = []
    for n, cols, row, cell, gen, head in survey(text):
        bad = [w for w, over in (('%d columns' % cols, cols > MAX_COLS), ('a row of %d characters' % row, row > MAX_ROW), ('a cell of %d characters' % cell, cell > MAX_CELL)) if over]
        if bad and (gen or not generated_only):
            msgs.append('%s:%d: %s: %s' % (name, n, ' and '.join(bad), head[:50]))
    return msgs
