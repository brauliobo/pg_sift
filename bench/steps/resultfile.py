"""The results files of the benchmarks (docs/results/*.tsv, one file per benchmark) and the generated blocks of README.md and docs/BENCHMARKS.md.

A results file is `# title`, `# key: value` lines (the run: build, host, load, method) and a tab separated table:

    version  case  arm  metric  value  min  max  n  load  note

`case` is the point of the benchmark (a size, a query), `arm` what was measured there (a plan, a build, an engine), `metric` its unit's name
(`ms`, `bytes`, `rows_per_s`, ...), `load` the 1-minute load average while it ran. The runners write the files through `write`; scripts/gen-docs.py only
reads them, so no number of a block is typed by hand. Block markers are `<!-- bench:NAME:begin ... -->` / `<!-- bench:NAME:end -->` (`replace_block`).
"""
import csv
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
sys.path.insert(0, HERE)
from mdtable import ms  # noqa: E402,F401  (the milliseconds of a cell)

REPO = os.path.dirname(os.path.dirname(HERE))
RESULTS = os.path.join(REPO, 'docs', 'results')
COLUMNS = ['version', 'case', 'arm', 'metric', 'value', 'min', 'max', 'n', 'load', 'note']


def host_load():
    pressure = ''
    try:
        pressure = ' cpu_pressure %s%%' % re.search(r'some avg10=([0-9.]+)', open('/proc/pressure/cpu').read()).group(1)
    except (OSError, AttributeError):
        pass
    return 'load %s%s' % (' '.join('%.2f' % x for x in os.getloadavg()), pressure)


def write(path, title, meta, rows):
    os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
    with open(path, 'w') as f:
        f.write('# %s\n' % title)
        for k, v in meta.items():
            f.write('# %s: %s\n' % (k, v))
        f.write('\t'.join(COLUMNS) + '\n')
        for r in rows:
            f.write('\t'.join(str(r.get(c, '')) for c in COLUMNS) + '\n')


_READ = {}


def read(path):
    """(meta, rows) of a results file; a file is parsed once while it does not change (the renderers ask for the same few dozen times), and the rows are shared, so none writes to them"""
    st = os.stat(path)
    key = (os.path.abspath(path), st.st_mtime_ns, st.st_size)
    if key not in _READ:
        _READ[key] = _read(path)
    return _READ[key]


def _read(path):
    meta, lines = {}, []
    for line in open(path):
        if line.startswith('#'):
            m = re.match(r'# ([\w ]+): (.*)', line.rstrip('\n'))
            if m:
                meta[m.group(1)] = m.group(2)
            else:
                meta.setdefault('title', line[2:].strip())
        elif line.strip():
            lines.append(line.rstrip('\n'))
    return meta, list(csv.DictReader(lines, delimiter='\t'))


def replace_block(path, name, lines):
    text = open(path).read() if os.path.exists(path) else ''
    begin, end = '<!-- bench:%s:begin' % name, '<!-- bench:%s:end -->' % name
    m = re.search(r'(%s[^\n]*-->\n)(.*?)(%s)' % (re.escape(begin), re.escape(end)), text, re.S)
    body = '\n'.join(lines).rstrip('\n') + '\n'
    new = text[:m.start(2)] + body + text[m.start(3):] if m else text + '%s -->\n%s%s\n\n' % (begin, body, end)
    if new != text:
        os.makedirs(os.path.dirname(os.path.abspath(path)), exist_ok=True)
        open(path, 'w').write(new)


_INDEX = {}


def get(rows, **kw):
    """the first row whose columns equal kw; the rows are indexed once per list and set of columns (a renderer asks for tens of thousands, each a scan of every row before)"""
    names = tuple(sorted(kw))
    held, n, index = _INDEX.get((id(rows), names), (None, 0, None))
    if held is not rows or n != len(rows):
        index = {}
        for r in rows:
            index.setdefault(tuple(r[k] for k in names), r)
        _INDEX[(id(rows), names)] = (rows, len(rows), index)   # holding rows keeps its id from being reused
    return index.get(tuple(kw[k] for k in names))


def num(rows, **kw):
    r = get(rows, **kw)
    return None if r is None or r['value'] in ('', 'NA') else float(r['value'])


def table(head, body, align=None):
    align = align or ['---'] * len(head)
    out = ['| ' + ' | '.join(head) + ' |', '|' + '|'.join(align) + '|']
    return out + ['| ' + ' | '.join(r) + ' |' for r in body]


def footer(meta):
    return 'Measured %s on %s, %s.' % (meta.get('date', '?'), meta.get('cpu', '?'), meta.get('postgresql', '?'))
