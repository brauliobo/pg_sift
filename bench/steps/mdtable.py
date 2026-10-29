"""The one Markdown table writer of the README's benchmark tables: a row per system, a column per benchmark.

A group is (caption, [(key, header)]); `tables` writes one table for each group, or for each chunk of at most `max_cols` columns when a group has more, so that
a table stays inside the 880 px of the rendered README. A header is short (it wraps on its spaces, hyphens and `<br>`); `cell(row, key)` is the text of a cell,
`n/a` where the system was not measured.
"""
import math

MAX_COLS = 6


def ms(v):
    return 'n/a' if v is None else '%.2f' % v if v < 10 else '%.1f' % v if v < 100 else '%d' % round(v)


def tables(rows, groups, cell, max_cols=MAX_COLS, first='system'):
    """lines of one table per group (several when it has more than max_cols columns, chunks of the same size), each under its caption"""
    out = []
    for caption, cols in groups:
        n = math.ceil(len(cols) / max_cols)
        size = math.ceil(len(cols) / n) if n else 1
        for i in range(n):
            part = cols[i * size:(i + 1) * size]
            out += ([caption + (f' ({i + 1} of {n})' if n > 1 else ''), ''] if caption else []) + [
                '| ' + ' | '.join([first] + [h for _, h in part]) + ' |', '|---|' + '---:|' * len(part)]
            out += ['| ' + ' | '.join([r] + [cell(r, k) for k, _ in part]) + ' |' for r in rows] + ['']
    return out[:-1]
