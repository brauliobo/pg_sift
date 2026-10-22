"""The generated parts of README.md and docs/BENCHMARKS.md, from the rows of bench/results/*.tsv of one tier (the slim tier by default).

    python3 -I bench/steps/readme.py [--results DIR] [--readme FILE] [--benchmarks FILE] [--tier slim]
                                     (bench/bench readme; bench/bench report --docs and bench/bench all --docs run it)

Prints the Benchmarks block of README.md and rewrites, in place, the blocks between these marker lines (a file without them is left alone,
with an error):

                      <!-- bench:readme:begin  --> ... <!-- bench:readme:end -->    the articles table, the bullets, the charts of the Search benchmark (the places table is in docs/BENCHMARKS.md)
  docs/BENCHMARKS.md  <!-- bench:slim:begin -->   ... <!-- bench:slim:end -->      the tier's tables (the Markdown of steps/tables.py)

Nothing in a block is typed by hand: every number is a row of bench/results (tables.py writes the corpus size, the seconds of the phases and
the host as rows too), the engines are those of bench/engines.tsv that have rows for the tier, in its order, so a new row of the registry
shows up after the next `bench all --docs`. A ratio carries its standard error where that exceeds 10% of it.
"""
import argparse
import math
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
import tables  # noqa: E402  (steps/tables.py: the Markdown tables of a tier)

BENCH = os.path.dirname(HERE)
REPO = os.path.dirname(BENCH)
FLAG = 0.10


def versions():
    """name -> value of bench/versions.env (the `NAME=${NAME:-value}` lines)"""
    out = {}
    for line in open(os.path.join(BENCH, 'versions.env')):
        m = re.match(r'(\w+)=\$\{\w+:-([^}]*)\}', line)
        if m:
            out[m.group(1)] = m.group(2)
    return out


def engine_registry():
    """(ids drawn in the README in table order, label by id) from bench/engines.tsv: the engines with chart = yes"""
    rows = [l.rstrip('\n').split('\t') for l in open(os.path.join(BENCH, 'engines.tsv')) if l.strip() and not l.startswith('#')]
    return [r[0] for r in rows if r[4] == 'yes']


def load(results):
    rows = []
    for d in ('articles', 'places'):
        path = os.path.join(results, d + '.tsv')
        if os.path.exists(path):
            rows += [tuple(r) for r in tables.rows_of(path, skip_header=False)[2:]]
    return rows


def number(r):
    return None if r is None or r[6] in ('NA', 'error') else float(r[6])


def rel(r):
    """the relative standard error of a row, None when it has none"""
    v = number(r)
    return None if v is None or v == 0 or len(r) < 11 or r[10] == 'NA' else float(r[10]) / v


def x(v):
    return '%.0fx' % v if v >= 10 else '%.1fx' % v if v >= 1 else '%.2fx' % v if v >= 0.1 else '%.2gx' % v


def ratio(a, b):
    """the value of row a over the value of row b, a float, and its relative standard error (None when a row has none)"""
    va, vb = number(a), number(b)
    if va is None or not vb:
        return None, None
    ra, rb = rel(a), rel(b)
    return va / vb, (math.hypot(ra, rb) if ra is not None and rb is not None else None)


def ratio_cell(a, b):
    r, se = ratio(a, b)
    if r is None:
        return a[6] if a is not None and a[6] in ('NA', 'error') else 'n/a'
    return x(r) + (' ± %.2g' % (r * se) if se is not None and se > FLAG else '')


def minutes(seconds):
    return 'about %d minute%s' % (max(1, round(seconds / 60)), '' if round(seconds / 60) <= 1 else 's')


class Tier:
    def __init__(self, rows, tier):
        self.rows, self.tier = rows, tier
        shown = engine_registry()
        self.articles = [e for e in shown if any(r[:3] == ('articles', tier, e) and r[5] == 'qps' for r in rows)]
        self.places = [e for e in shown if any(r[:3] == ('places', tier, e) and r[5] == 'p50_ms' for r in rows)]
        self.classes = list(dict.fromkeys(r[3] for r in rows if r[:2] == ('articles', tier) and r[5] == 'qps'))
        self.ref = 'pg_search' if 'pg_search' in self.articles else (self.articles[-1] if self.articles else None)
        v = versions()
        self.version = {'pg_search': v.get('PARADEDB_VERSION'), 'pg_textsearch': v.get('PG_TEXTSEARCH_VERSION'), 'rum': v.get('RUM_VERSION')}
        self.session = self.words('session')

    def get(self, *key, **kw):
        return tables.get(self.rows, *key, **kw)

    def num(self, dataset, engine, cls='-', variant='-', metric=None):
        return number(self.get(dataset, self.tier, engine, cls, variant, metric))

    def label(self, e):
        v = self.version.get(e)
        return tables.NAME[e] + (' ' + v if v else '')

    def words(self, metric):
        """the key=value words of the session row"""
        r = next((r for r in self.rows if r[0] == 'articles' and r[1] == self.tier and r[5] == metric), None)
        return dict(w.split('=', 1) for w in r[4].split(';')) if r else {}

    def corpus(self, dataset):
        return int(self.num(dataset, '-', metric='corpus') or 0)

    def seconds(self, tier=None):
        """the seconds of the phases of both datasets, from the wall_s rows"""
        return sum(float(r[6]) for r in self.rows if r[1] == (tier or self.tier) and r[5] == 'wall_s')

    def load(self):
        rows = [r for r in self.rows if r[1] == self.tier and r[5] == 'load_1m' and r[4] == '-']
        return (min(float(r[7]) for r in rows), max(float(r[8]) for r in rows), sorted(float(r[6]) for r in rows)[len(rows) // 2]) if rows else None

    def late_loads(self):
        """[(classes, min, max, median)] of the cells measured in a later run merged into the tier (tables.py --merge)"""
        return [(r[4].replace(',', ', '), r[7], r[8], r[6]) for r in self.rows if r[1] == self.tier and r[5] == 'load_1m' and r[4] != '-']

    def qps(self, e, c):
        return self.get('articles', self.tier, e, c, '-', 'qps')

    def p50(self, e, c, wl):
        return self.get('places', self.tier, e, c, wl, 'p50_ms')

    def sizes(self, dataset):
        return self.get(dataset, self.tier, 'sift', '-', '-', 'index_mib')


def absolute(v, digits=3):
    """a measured value with `digits` significant figures and thousands separators: 9420, 1,240,000, 0.19"""
    return '{:,.0f}'.format(float('%.*g' % (digits, v))) if v >= 10 ** (digits - 1) else '%.*g' % (digits, v)


def measured_cell(r, edge, higher=True):
    """a value of a row, its standard error where that exceeds 10% of it, in bold when it is as good as `edge`"""
    v = number(r)
    if v is None:
        return r[6] if r is not None and r[6] in ('NA', 'error') else 'n/a'
    se = rel(r)
    text = absolute(v) + (' ± %s' % absolute(v * se, 2) if se is not None and se > FLAG else '')
    return '**%s**' % text if edge is not None and (v >= edge if higher else v <= edge) else text


def best_edge(column, higher=True):
    """the best value of a column moved by its own standard error: the cells at least that good are within the noise of the best"""
    rows = [r for r in column if number(r) is not None]
    if not rows:
        return None
    best = (max if higher else min)(rows, key=number)
    se = (rel(best) or 0) * number(best)
    return number(best) - se if higher else number(best) + se


def table(t):
    """the Markdown table of the README: queries per second of each engine and class (the fastest in bold), index size and build time"""
    classes = t.classes
    label = {'count': '`count(*)`'}
    head = ['articles (queries/s)', 'index', 'build'] + [label.get(c, c) for c in classes]
    best = {c: best_edge([t.qps(e, c) for e in t.articles]) for c in classes}
    body = []
    for e in t.articles:
        name = '**%s** (default)' % t.label(e) if e == 'sift' else t.label(e)
        size, build = t.num('articles', e, metric='index_mib'), t.num('articles', e, metric='build_s')
        body.append([name, '%.0f MiB' % size if size is not None else 'NA', '%.1f s' % build if build is not None else 'NA']
                    + [measured_cell(t.qps(e, c), best[c]) for c in classes])
    return tables.md_table(head, body)


def places_bullet(t):
    """sift against pg_search (and Elasticsearch) on the places: the range of the top-20 p50 over the classes, and the cells where sift is ahead"""
    p50 = [r for r in t.rows if r[:2] == ('places', t.tier) and r[5] == 'p50_ms']
    classes = tables.place_classes(r for r in p50 if r[2] == 'sift')
    span = lambda e: '%s-%s ms' % tuple('%.2g' % f(number(t.p50(e, c, 'top')) for c in classes) for f in (min, max))
    text = '- Autocomplete on %s places: sift answers the top 20 in %s in every query class' % (format(t.corpus('places'), ','), span('sift'))
    text += ''.join(', %s in %s' % (tables.NAME[e], span(e)) for e in ('elasticsearch', 'pg_search') if e in t.places)
    if 'pg_search' in t.places:
        cells = [ratio(t.p50('pg_search', c, wl), t.p50('sift', c, wl))[0] for wl in dict.fromkeys(r[4] for r in p50 if r[2] == 'sift') for c in classes]
        cells = [v for v in cells if v is not None]
        won = [v for v in cells if v > 1]
        if won:
            text += '; sift is ahead of pg_search in %d of %d warm cells (%s to %s)' % (len(won), len(cells), x(min(won)), x(max(won)))
    return text + '.'


def positions_bullet(t):
    if not {'sift', 'sift_nopos', 'pg_search'} <= set(t.articles):
        return None
    size = lambda e: t.num('articles', e, metric='index_mib')
    q = lambda e, c: absolute(t.num('articles', e, c, '-', 'qps'))
    return ('- `positions = off` shrinks the index to %.0f MiB (%s smaller than pg_search\'s %.0f MiB, the default sift index is %.0f MiB); phrase runs at %s queries/s and `count(*)` at %s '
            '(pg_search: %s and %s), against %s and %s with positions.' %
            (size('sift_nopos'), x(size('pg_search') / size('sift_nopos')), size('pg_search'), size('sift'),
             q('sift_nopos', 'phrase'), q('sift_nopos', 'count'), q('pg_search', 'phrase'), q('pg_search', 'count'), q('sift', 'phrase'), q('sift', 'count')))


def benchmarks_block(t, timing):
    s, p = t.session, t.load()
    articles, places = format(t.corpus('articles'), ','), format(t.corpus('places'), ',')
    rounds = max((int(r[11]) for r in t.rows if r[:2] == ('articles', t.tier) and r[5] == 'qps'), default=0)
    budget, details = tables.cpus_parts(s.get('quota', ''))
    out = ['Simple English Wikipedia (%s articles, 8 clients) and GeoNames cities15000 (%s places, autocomplete, 1 client): the slim tier of the suite, the default of `bench/bench all`.' % (articles, places), '',
           '- **Scope:** every engine, query class and workload of [bench/README.md](bench/README.md) on a smaller corpus; cells stop at a 5% relative standard error (at most 1 s).',
           '- **Method:** same data, queries and clients for every engine, one engine at a time, median of %s interleaved rounds.' % ('2-%d' % rounds if rounds > 2 else '2'),
           '- **CPUs:** ' + budget + (':' if details else '.')]
    out += ['  - ' + d for d in details]
    if s:
        out += ['- **Host:** measured %s on %s (%s threads), PostgreSQL %s.' % (s['date'], s['cpu'], s.get('threads', '?'), s['pg'].replace('PostgreSQL ', ''))]
    out += ['- **Cells:** queries per second (higher is better); '
            'the fastest engines of a column (all within one standard error of the best) are in bold; a `±` is one standard error where it exceeds 10% of the value. The places are in the chart below and in [docs/BENCHMARKS.md](docs/BENCHMARKS.md#places).', '']
    out += table(t) + ['']
    out += [places_bullet(t)]
    out += [b for b in (positions_bullet(t),) if b]
    out += ['- Prefix ranking agrees less with a BM25-over-n-grams oracle than pg_search\'s does, because sift metrics_a a prefix as the best word it expands to ([correctness](docs/BENCHMARKS.md#correctness)).']
    out += ['- The host was shared%s, so absolute numbers are noisy and ratios close to each other are not distinguishable.' % (' (load average %.0f-%.0f during the cells, median %.0f%s)' % (p + (''.join('; %s measured later: %.0f-%.0f, median %.0f' % (c, float(lo), float(hi), float(m)) for c, lo, hi, m in t.late_loads()),)) if p else '')]
    out += ['- Every cell with its standard error, the cold-cache tables and the method: [docs/BENCHMARKS.md].', '']
    out += ['![Throughput by workload](docs/img/articles-throughput.svg)', '![Index size](docs/img/index-size.svg)', '![Autocomplete latency](docs/img/places-latency.svg)', '']
    out += ['Reproduce:', '',
            '- `bench/bench all` (see [bench/README.md](bench/README.md)): %s of builds and measurements for the slim tier, %s with the cold-cache tables of `--docs`.' % (timing[0], timing[1]),
            '- `bench/bench all --docs` rewrites this section from a new session.',
            '- `--full` runs the full-size tier (every article of the dump, GeoNames cities500, about an hour) and writes its own results; they are not published here.',
            '- `bench/bench regress --run` compares this checkout with an earlier release and exits non-zero on a regression.']
    return out


def replace_block(path, name, lines):
    text = open(path).read()
    begin, end = '<!-- bench:%s:begin' % name, '<!-- bench:%s:end -->' % name
    m = re.search(r'(%s[^\n]*-->\n)(.*?)(%s)' % (re.escape(begin), re.escape(end)), text, re.S)
    if not m:
        sys.exit('readme.py: %s has no <!-- bench:%s:begin --> ... <!-- bench:%s:end --> block' % (path, name, name))
    new = text[:m.start(2)] + '\n'.join(lines).rstrip('\n') + '\n' + text[m.start(3):]
    if new != text:
        open(path, 'w').write(new)


def slim_section(t, rows):
    s = t.session
    rounds = max((int(r[11]) for r in rows if r[:2] == ('articles', t.tier) and r[5] == 'qps'), default=0)
    out = ['## Slim tier (the default): %s articles, %s places' % (format(t.corpus('articles'), ','), format(t.corpus('places'), ',')), '',
           'Simple English Wikipedia (%s articles, 8 clients) and GeoNames cities15000 (%s places, 1 client), every engine of `bench/engines.tsv`, the same query classes as the full tier and its workloads but the unordered scan, '
           'cells of at most 1 s that stop at a relative standard error of 5%%, %s interleaved rounds. Generated by `bench/bench all --docs` from [results/](results/) (tier `%s`); '
           'the README quotes these tables. %s' % (format(t.corpus('articles'), ','), format(t.corpus('places'), ','), '2-%d' % rounds if rounds > 2 else '2', t.tier,
                                                   'Measured %s on %s (%s threads), PostgreSQL %s, sift %s%s.' % (
                                                       s['date'], s['cpu'], s.get('threads', '?'), s['pg'].replace('PostgreSQL ', ''), s.get('sift', '?'),
                                                       ', Elasticsearch %s' % s['elasticsearch'] if s.get('elasticsearch') else '') if s else ''), '']
    for dataset, title, md in (('articles', 'Articles', tables.md_articles), ('places', 'Places', tables.md_places)):
        if not any(r[:2] == (dataset, t.tier) and r[5] in ('qps', 'p50_ms') for r in rows):
            continue
        load = tables.get(rows, dataset, t.tier, '-', '-', '-', 'load_1m')
        out += ['### %s' % title, '', tables.method(t.session.get('quota', ''))]
        if load:
            out += ['Host load average (1 min) during the cells: %s-%s (median %s).' % (load[7], load[8], load[6])]
        out += [''] + md(rows, t.tier) + ['']
        if any(r[:2] == (dataset, t.tier + '-cold') for r in rows):
            out += ['#### Cold cache', ''] + tables.md_cold(rows, dataset, t.tier + '-cold') + ['']
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'bench', 'results'))
    ap.add_argument('--readme', default=os.path.join(REPO, 'README.md'))
    ap.add_argument('--benchmarks', default=os.path.join(REPO, 'docs', 'BENCHMARKS.md'))
    ap.add_argument('--tier', default='slim')
    a = ap.parse_args()
    rows = load(a.results)
    t = Tier(rows, a.tier)
    if not t.articles or not t.classes:
        sys.exit('readme.py: no %s rows in %s: run bench/bench all --docs' % (a.tier, a.results))
    timing = (minutes(t.seconds()), minutes(t.seconds() + t.seconds(a.tier + '-cold')))
    block = benchmarks_block(t, timing)
    print('\n'.join(block))
    replace_block(a.readme, 'readme', block)
    replace_block(a.benchmarks, 'slim', slim_section(t, rows))


if __name__ == '__main__':
    main()
