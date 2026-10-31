"""The generated parts of README.md and bench/out/BENCHMARKS.md, from the rows of bench/results/*.tsv of one tier (the slim tier by default).

    python3 -I bench/steps/readme.py [--results DIR] [--readme FILE] [--benchmarks FILE] [--tier slim]
                                     (bench/bench readme; bench/bench report --docs and bench/bench all --docs run it)

Prints the Benchmarks block of README.md and rewrites, in place, the blocks between these marker lines (a file without them is left alone,
with an error):

                      <!-- bench:readme:begin  --> ... <!-- bench:readme:end -->    the articles table, the bullets, the charts of the Search benchmark (the places table is in bench/out/BENCHMARKS.md)
  bench/out/BENCHMARKS.md  <!-- bench:slim:begin -->   ... <!-- bench:slim:end -->      the tier's tables (the Markdown of steps/tables.py)

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
import engines  # noqa: E402  (steps/engines.py: bench/engines.tsv)
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
    return [r['id'] for r in engines.rows() if r['chart'] == 'yes']


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

    def qps(self, e, c):
        return self.get('articles', self.tier, e, c, '-', 'qps')

    def p50(self, e, c, wl):
        return self.get('places', self.tier, e, c, wl, 'p50_ms')

    def sizes(self, dataset):
        return self.get(dataset, self.tier, 'sift', '-', '-', 'index_mib')


def absolute(v, digits=3):
    """a measured value with `digits` significant figures and thousands separators: 9420, 1,240,000, 0.19"""
    return '{:,.0f}'.format(float('%.*g' % (digits, v))) if v >= 10 ** (digits - 1) else '%.*g' % (digits, v)


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
    """the README's search block: the findings and the charts; the scope, method, CPU budget and host are the footnote [^search], every cell with its standard error is in bench/out/BENCHMARKS.md"""
    s = t.session
    articles, places = format(t.corpus('articles'), ','), format(t.corpus('places'), ',')
    rounds = max((int(r[11]) for r in t.rows if r[:2] == ('articles', t.tier) and r[5] == 'qps'), default=0)
    budget, details = tables.cpus_parts(s.get('quota', ''))
    out = ['Simple English Wikipedia (%s articles, 8 clients) and GeoNames cities15000 (%s places, autocomplete, 1 client), every engine on the same data, queries and clients[^search]. Throughput in queries per second, higher is better; '
           'the error bars are one standard error over the interleaved rounds.' % (articles, places), '',
           '![Throughput by workload](img/articles-throughput.svg)', '', '![Index size](img/index-size.svg)', '']
    out += [places_bullet(t)]
    out += [b for b in (positions_bullet(t),) if b]
    out += ['- Prefix ranking agrees less with a BM25-over-n-grams oracle than pg_search\'s does, because sift metrics_a a prefix as the best word it expands to ([correctness](BENCHMARKS.md#correctness)).', '',
            '![Autocomplete latency](img/places-latency.svg)', '',
            'Every cell with its standard error, the cold-cache tables and the method: [BENCHMARKS.md]; reproduce with `bench/bench all` (%s for the slim tier, %s with the cold-cache tables of `--docs`; `--full` runs every article of the dump and GeoNames cities500, about an hour, and writes results that are not published here; `bench/bench regress --run` compares this checkout with an earlier release).' % (timing[0], timing[1]), '']
    foot = ['[^search]: Scope: every engine, query class and workload of [bench/README.md](bench/README.md) on a smaller corpus (the slim tier, the default of `bench/bench all`); cells stop at a 5%% relative standard error (at most 1 s). '
            'Method: one engine at a time, median of %s interleaved rounds. CPUs: %s%s' % ('2-%d' % rounds if rounds > 2 else '2', budget, (': ' + '; '.join(details) + '.') if details else '.')]
    if s:
        at = ' at pg_sift %s (%s)' % (s['version'], s['sift'][:7]) if s.get('version') and s.get('sift') else ''
        foot[0] += ' Measured%s %s on %s, PostgreSQL %s.' % (at, s['date'], s['cpu'], s['pg'].replace('PostgreSQL ', ''))
    foot[0] += ' The host was shared, so absolute numbers are noisy and ratios close to each other are not distinguishable.'
    return out + foot


def replace_block(path, name, lines):
    text = open(path).read() if os.path.exists(path) else ''   # the pages of bench/out start empty: a block is appended
    begin, end = '<!-- bench:%s:begin' % name, '<!-- bench:%s:end -->' % name
    m = re.search(r'(%s[^\n]*-->\n)(.*?)(%s)' % (re.escape(begin), re.escape(end)), text, re.S)
    body = '\n'.join(lines).rstrip('\n') + '\n'
    new = text[:m.start(2)] + body + text[m.start(3):] if m else text + '%s -->\n%s%s\n\n' % (begin, body, end)
    if new != text:
        os.makedirs(os.path.dirname(path), exist_ok=True)
        open(path, 'w').write(new)


def slim_section(t, rows):
    s = t.session
    rounds = max((int(r[11]) for r in rows if r[:2] == ('articles', t.tier) and r[5] == 'qps'), default=0)
    out = ['## Slim tier (the default): %s articles, %s places' % (format(t.corpus('articles'), ','), format(t.corpus('places'), ',')), '',
           'Simple English Wikipedia (%s articles, 8 clients) and GeoNames cities15000 (%s places, 1 client), every engine of `bench/engines.tsv`, the same query classes as the full tier and its workloads but the unordered scan, '
           'cells of at most 1 s that stop at a relative standard error of 5%%, %s interleaved rounds. Generated by `bench/bench all --docs` from [results/](results/) (tier `%s`); '
           'the README quotes these tables. %s' % (format(t.corpus('articles'), ','), format(t.corpus('places'), ','), '2-%d' % rounds if rounds > 2 else '2', t.tier,
                                                   'Measured %s on %s, PostgreSQL %s, sift %s%s.' % (
                                                       s['date'], s['cpu'], s['pg'].replace('PostgreSQL ', ''), s.get('sift', '?'),
                                                       ', Elasticsearch %s' % s['elasticsearch'] if s.get('elasticsearch') else '') if s else ''), '']
    for dataset, title, md in (('articles', 'Articles', tables.md_articles), ('places', 'Places', tables.md_places)):
        if not any(r[:2] == (dataset, t.tier) and r[5] in ('qps', 'p50_ms') for r in rows):
            continue
        out += ['### %s' % title, '', tables.method(t.session.get('quota', ''))]
        out += [''] + md(rows, t.tier) + ['']
        if any(r[:2] == (dataset, t.tier + '-cold') for r in rows):
            out += ['#### Cold cache', ''] + tables.md_cold(rows, dataset, t.tier + '-cold') + ['']
    return out


def main():
    ap = argparse.ArgumentParser(description=__doc__.split('\n')[0])
    ap.add_argument('--results', default=os.path.join(REPO, 'bench', 'results'))
    ap.add_argument('--readme', default=os.path.join(REPO, 'bench', 'out', 'README-blocks.md'))
    ap.add_argument('--benchmarks', default=os.path.join(REPO, 'bench', 'out', 'BENCHMARKS.md'))
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
    import cases_render   # the "What got faster in 3.3" block, from bench/results/cases.tsv (it imports extras, which imports this module)
    cases_render.render(a.results, a.readme, a.benchmarks)


if __name__ == '__main__':
    main()
