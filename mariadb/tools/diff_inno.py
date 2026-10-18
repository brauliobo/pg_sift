"""Match sets of sift against InnoDB FULLTEXT for the same trace queries.

    python3 -I diff_inno.py TRACE SIFT_OUT INNO_OUT

SIFT_OUT: `mdbq print` of mkstmts sift-matches (lines "qid TAB [pk,pk,...]"); INNO_OUT: of inno-matches (lines
"qid TAB pk", one per match, grouped by query). Both are streamed query by query. Per class (and/or/phrase) prints the
number of queries whose match sets are equal and how the others differ (sift only / InnoDB only), split by whether every
query term is a plain ASCII word.
"""
import collections
import itertools
import json
import re
import sys


def inno_groups(path):
    rows = (l.rstrip('\n').partition('\t') for l in open(path, encoding='utf-8'))
    for qid, grp in itertools.groupby(rows, key=lambda r: r[0]):
        yield qid, {int(r[2]) for r in grp if r[2].isdigit()}


def main():
    trace = {}
    for line in open(sys.argv[1], encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        trace[f[0]] = (f[1], f[2].split())
    inno = inno_groups(sys.argv[3])
    cur = next(inno, None)
    stats = collections.defaultdict(collections.Counter)
    samples = collections.defaultdict(list)
    for line in open(sys.argv[2], encoding='utf-8'):
        qid, _, js = line.rstrip('\n').partition('\t')
        mode, terms = trace[qid]
        s = set(json.loads(js)) if js.startswith('[') else None
        i = set()
        if cur and cur[0] == qid:
            i = cur[1]
            cur = next(inno, None)
        if s is None:
            stats[mode]['sift error'] += 1
            continue
        ascii_only = all(re.fullmatch(r'[a-z0-9]+', t) for t in terms)
        kind = 'equal' if s == i else 'sift superset' if i < s else 'InnoDB superset' if s < i else 'both have extras'
        stats[mode][kind] += 1
        stats[mode][('(ascii terms) ' if ascii_only else '(non-ascii terms) ') + kind] += 1
        if kind != 'equal' and len(samples[(mode, kind)]) < 3:
            samples[(mode, kind)].append((qid, ' '.join(terms)[:60], len(s), len(i), len(s - i), len(i - s)))
    for mode in ('and', 'or', 'phrase'):
        print('%s: %d queries' % (mode, sum(1 for m, _ in trace.values() if m == mode)))
        for k, v in sorted(stats[mode].items()):
            print('  %-40s %d' % (k, v))
        for (m, kind), rows in samples.items():
            if m == mode:
                for r in rows:
                    print('    e.g. %s [%s] %s: sift %d, innodb %d, only sift %d, only innodb %d' % ((r[0], r[1], kind) + r[2:]))


main()
