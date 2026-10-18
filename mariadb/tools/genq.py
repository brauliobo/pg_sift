"""SiftQL queries for the comparison with pg_sift, from the benchmark trace.

    python3 -I genq.py TRACE [PER_KIND] [--regex]  >  queries.tsv        lines "id TAB query"

TRACE: lines "id TAB mode TAB term term ..." (mode and/or/phrase of the same substring come in threes). The first three
kinds are the trace itself; the others cover the rest of SiftQL on the same terms: prefix, wildcard, fuzzy, NOT, boosts,
groups, nesting, a phrase next to a term. PER_KIND limits the substrings used for the derived kinds (default 150).
--regex appends regular expressions (classes, repetition, alternation, anchors, next to a term, negated) after them.
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


def regexes(t):
    """Regular expressions over the terms of an article (pg_sift /re/ syntax): classes, repetition, alternation, anchors."""
    a, b = t[0], t[1] if len(t) > 1 else t[0]
    out = [
        '/' + a[:3] + '.*/' if len(a) > 3 else None,
        '/' + a[:2] + '[a-z]+' + a[-1] + '/' if len(a) > 3 else None,
        '/^' + a[0] + '.' + a[2:] + '$/' if len(a) > 3 else None,
        '/' + a[:2] + '(' + b[:2] + '|' + a[2:4] + ').*/' if len(a) > 4 and len(b) > 2 else None,
        '/' + a[:3] + '.*/ AND ' + b if len(t) > 1 and len(a) > 3 else None,
        '/[' + a[0] + b[0] + ']' + a[1:3] + '.{1,4}/' if len(a) > 3 and len(b) > 1 else None,
        '/' + a[:2] + '.*/ AND NOT ' + b if len(t) > 1 and len(a) > 2 else None,
    ]
    return [q for q in out if q]


def main():
    per = int(sys.argv[2]) if len(sys.argv) > 2 and sys.argv[2].isdigit() else 150
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
    if '--regex' in sys.argv:
        for t in subs[:per]:
            for q in regexes(t):
                n += 1
                print('%d\t%s' % (n, q))


main()
