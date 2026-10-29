"""One result cell of a benchmark table: the time, the CPU and the IO of a system for a question, and the same three against pg_sift's.

    4.4 · 6.1 · 0.5                       pg_sift: ms · cpu-ms · MB read
    9.2 · 11 · 3.1 (2.1x · 1.8x · 6.2x)   another system, then its figure over pg_sift's for each measure: above 1x it uses more, a tie is within 5%

A figure is a number or `NA(reason)`: a measure nobody took (or the engine cannot report) prints `n/a` and its reason goes into the legend under the table, so a cell
never hides a missing measure and never guesses one. `legend` writes that line.
"""
import mdtable

TIE = 0.05
SEP = ' · '
NOT_MEASURED = 'not measured under 3.7 yet'
MINE = ('sift', 'sift_col', 'sift_table')   # the systems that are the reference: no factors
MAX_COLS = 4   # a cell is about 40 characters: four of them and the system fit the 880 px of the README


class NA(str):
    """a figure that is not there, with the reason"""


def num(v):
    return 'n/a' if v is None or isinstance(v, NA) else '<0.01' if 0 < v < 0.005 else mdtable.ms(v)


def times(v, ref):
    """v over ref as '2.1x': 'n/a' when either is missing, a tie within 5% as '1.0x', '∞x' for a measure that is zero for pg_sift only"""
    if v is None or ref is None or isinstance(v, NA) or isinstance(ref, NA):
        return 'n/a'
    if ref == 0:
        return '1.0x' if v == 0 else '∞x'
    r = v / ref
    if abs(r - 1) <= TIE:
        return '1.0x'
    return f'{r:.0f}x' if r >= 9.5 else f'{r:.1f}x' if r >= 1 else f'{r:.2g}x'


def fig(get):
    """(ms, cpu_ms, mb) of a system on a question from a lookup get(metric) -> result row or None; None when it did not answer, `NA` for a measure the run did not record"""
    t = get('latency_ms')
    return t and (t['value'], *((r['value'] if (r := get(m)) else NA(NOT_MEASURED)) for m in ('cpu_ms', 'read_mb')))


def best(figs):
    """the figure with the least time (pg_sift's faster layout), None when there is none"""
    return min((f for f in figs if f), key=lambda f: f[0], default=None)


def cell(fig, ref=None, bold=False):
    """fig, ref: (ms, cpu_ms, mb) of the system and of pg_sift, ref None for pg_sift itself; no figure at all (the system did not run the question) is 'n/a'"""
    if fig is None or fig[0] is None:
        return 'n/a'
    out = SEP.join(map(num, fig))
    if ref is not None:
        out += ' (' + SEP.join(times(v, r) for v, r in zip(fig, ref)) + ')'
    return f'**{out}**' if bold else out


def legend(figs, read='MB read'):
    """the lines under a table of cells: the units, the factor, and the reason of each `n/a` measure"""
    why = list(dict.fromkeys(v for f in figs if f for v in f if isinstance(v, NA)))
    return [f'Each cell is milliseconds{SEP}CPU milliseconds of the whole server{SEP}{read}; in parentheses the same three over pg_sift\'s (above 1x the system uses more, 1.0x is within 5%).'
            + ''.join(f' `n/a`: {w}.' for w in why)]


def table(systems, columns, fig, ref, first='system', read='MB read'):
    """lines of the tables of `systems` (label -> key) against `columns` (key, header): fig(system, column) is the (ms, cpu_ms, mb) or None, ref(system, column) the figure it is compared with (pg_sift's);
    the fastest system of a column and anything within 5% of it is in bold, pg_sift's own rows have no factors"""
    best = {}
    for c, _ in columns:
        ts = [f[0] for s in systems.values() if (f := fig(s, c)) and f[0] is not None]
        best[c] = min(ts) if ts else None

    def text(label, c):
        s, f = systems[label], fig(systems[label], c)
        return cell(f, None if s in MINE else ref(s, c), bold=bool(f) and f[0] is not None and f[0] <= best[c] * (1 + TIE))

    figs = [f for s in systems.values() for c, _ in columns if (f := fig(s, c))]
    return mdtable.tables(list(systems), [('', columns)], text, max_cols=MAX_COLS, first=first) + [''] + legend(figs, read)

