"""SiftQL queries for the comparison with pg_sift, from the benchmark trace.

    python3 -I genq.py TRACE [PER_KIND]  >  queries.tsv        lines "id TAB query"

TRACE: lines "id TAB mode TAB term term ..." (mode and/or/phrase of the same substring come in threes). The first three
kinds are the trace itself; the others cover the rest of SiftQL on the same terms: prefix, wildcard, fuzzy, NOT, boosts,
groups, nesting, a phrase next to a term. PER_KIND limits the substrings used for the derived kinds (default 150).
"""
import sys


def derived(t):
    a, b = t[0], t[1] if len(t) > 1 else t[0]
    c = t[2] if len(t) > 2 else b
    out = {
        'prefix': a[:3] + '*' if len(a) > 3 else None,
        'prefix2': a[:2] + '*' + ' AND ' + b[:3] + '*' if len(a) > 2 and len(b) > 3 else None,
        'wildcard': a[:2] + '*' + a[-1] if len(a) > 3 else None,
        'qmark': a[:-2] + '??' if len(a) > 4 else None,
        'fuzzy': a + '~1' if len(a) > 5 else None,
        'not': a + ' AND NOT ' + b if len(t) > 1 else None,
        'boost_or': a + '^3 OR ' + b if len(t) > 1 else None,
        'boost_and': a + '^2.5 AND ' + b + '^0.5' if len(t) > 1 else None,
        'group': '{' + ' '.join(t[:4]) + '}' if len(t) > 1 else None,
        'nested': '(' + a + ' OR ' + b + ') AND ' + c if len(t) > 2 else None,
        'phrase_and': '"' + a + ' ' + b + '" AND ' + c if len(t) > 2 else None,
        'phrase_not': '"' + a + ' ' + b + '" AND NOT ' + c if len(t) > 2 else None,
    }
    return [q for q in out.values() if q]


def main():
    per = int(sys.argv[2]) if len(sys.argv) > 2 else 150
    n = 0
    subs = []
    for line in open(sys.argv[1], encoding='utf-8'):
        f = line.rstrip('\n').split('\t')
        t = f[2].split()
        n += 1
        if f[1] == 'and':
            subs.append(t)
        sep = {'and': ' AND ', 'or': ' OR '}.get(f[1])
        print('%d\t%s' % (n, sep.join(t) if sep else '"' + ' '.join(t) + '"'))
    for t in subs[:per]:
        for q in derived(t):
            n += 1
            print('%d\t%s' % (n, q))


main()
