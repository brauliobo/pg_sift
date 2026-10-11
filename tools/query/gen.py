#!/usr/bin/env python3
"""Grammar-aware SiftQL generator for fuzzing and benchmarks.

Writes one query per line in COPY text format. Modes:
  valid    queries from the grammar (DESIGN.md (h)), mostly well-formed
  fuzz     valid queries, mutated ones and hostile inputs, mixed
"""
import argparse
import random

SPECIALS = '(){}"^~/\\*? '
WORDS = ['fox', 'quick', 'brown', 'dog', 'lazy', 'bear', 'honey', 'the', 'e-mail', 'x86_64',
         'Fox', 'QUICK', 'café', 'naïve', 'жук', 'жучок', '日本語', 'straße', '😀', 'a', 'of',
         'AND', 'OR', 'NOT', 'and', 'or', 'not', '2024', 'v1.2', "don't"]
REGEXES = ['b.*n', '(fox|dog)', '[a-z]{3}', 'x8\\/6', '.*', '[[:alpha:]]+', 'a+?b', '(?i)FOX',
           '\\d+', 'a{2,3}', '[^a]', '(a|b)*c', 'ж.к', '\\m', '(', ')', '[', '*', 'a**',
           '(?q)x', '***=x', 'a{3,1}', '\\', 'a\\/b']
UNICODE = [(0x20, 0x7E), (0xA0, 0x24F), (0x400, 0x4FF), (0x3040, 0x30FF), (0x4E00, 0x4E80),
           (0x1F600, 0x1F64F), (0x300, 0x36F), (0x1, 0x1F), (0x2000, 0x206F)]


def uchar(r):
    lo, hi = r.choice(UNICODE)
    return chr(r.randint(lo, hi))


def word(r):
    if r.random() < 0.7:
        w = r.choice(WORDS)
    else:
        w = ''.join(uchar(r) for _ in range(r.randint(1, 8)))
    if r.random() < 0.1:
        w = w * r.randint(2, 60)
    return ''.join('\\' + c if c in SPECIALS or ord(c) < 0x21 else c for c in w)


def boost(r):
    if r.random() < 0.85:
        return ''
    return '^' + r.choice(['2', '0', '0.5', '.25', '1e-3', '3.75', '10000', '1E2', '9999.5',
                           '10001', '-1', '1e', 'x', '1..2', '', '1e40'])


def leaf(r, atom=False):
    k = r.randrange(9)
    if k == 0 and not atom:
        body = ' '.join(word(r) for _ in range(r.randint(0, 5)))
        s = '"' + body.replace('"', '\\"') + '"'
    elif k == 1:
        s = word(r)[:r.randint(1, 4)].rstrip('\\') + '*'
    elif k == 2:
        w = list(word(r))
        for _ in range(r.randint(1, 3)):
            w.insert(r.randint(0, len(w)), r.choice('*?'))
        s = ''.join(w)
    elif k == 3:
        s = '/' + r.choice(REGEXES) + '/'
    elif k == 4:
        s = word(r) + r.choice(['~', '~1', '~2', '~3'])
    else:
        s = word(r)
    return s if atom else s + boost(r)


def expr(r, depth):
    if depth <= 0 or r.random() < 0.3:
        return leaf(r)
    n = r.randint(2, 4)
    k = r.randrange(4)
    if k == 0:
        return '(' + ' OR '.join(expr(r, depth - 1) for _ in range(n)) + ')' + boost(r)
    if k == 1:
        return '{' + ' '.join(leaf(r, True) for _ in range(n)) + '}' + boost(r)
    parts = [expr(r, depth - 1)]
    parts += [r.choice(['', '', 'NOT ', 'NOT NOT ']) + expr(r, depth - 1) for _ in range(n - 1)]
    return '(' + r.choice([' ', ' AND ']).join(parts) + ')' + boost(r)


def mutate(r, s):
    s = list(s)
    for _ in range(r.randint(1, 6)):
        op = r.randrange(6)
        i = r.randint(0, len(s))
        if op == 0:
            s.insert(i, r.choice(SPECIALS + 'ANDORNOT~^'))
        elif op == 1 and s:
            del s[i:i + r.randint(1, 5)]
        elif op == 2:
            s[i:i] = list(r.choice([' AND ', ' OR ', ' NOT ', '\\', '"', '/', '((', '))', '^1e9']))
        elif op == 3:
            s.insert(i, uchar(r))
        elif op == 4 and s:
            j = r.randint(0, len(s))
            s[i:i] = s[min(i, j):max(i, j)][:200]
        elif op == 5:
            s = s[:i]
    return ''.join(s)


def hostile(r):
    k = r.randrange(9)
    n = r.choice([10, 33, 100, 1000, 10000, 100000])
    if k == 0:
        return '(' * n + 'a' + ')' * n
    if k == 1:
        return 'NOT ' * n + 'a b'
    if k == 2:
        return ' '.join(leaf(r) for _ in range(r.randint(250, 300)))
    if k == 3:
        return 'x' * r.choice([255, 256, 1000, 262144, 262145])
    if k == 4:
        return r.choice(['*', '?', 'a*', '\\', '"', '/', '{', '(', '^', '~']) * n
    if k == 5:
        return '"' + ' '.join(['w'] * r.randint(30, 40)) + '"'
    if k == 6:
        return ''.join(uchar(r) for _ in range(min(n, 5000)))
    if k == 7:
        n = min(n, r.choice([100, 333, 3000]))
        return '/' + '(a|' * n + 'b' + ')' * n + '/'
    return '(a^100)' * 2 + '^' + str(r.choice([1, 100, 101]))


def copy_escape(s):
    return s.replace('\\', '\\\\').replace('\n', '\\n').replace('\r', '\\r').replace('\t', '\\t')


def main():
    ap = argparse.ArgumentParser(description=__doc__)
    ap.add_argument('--mode', choices=['valid', 'fuzz'], default='fuzz')
    ap.add_argument('-n', type=int, default=10000)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('-o', '--out', required=True)
    a = ap.parse_args()
    r = random.Random(a.seed)
    with open(a.out, 'w', encoding='utf-8', newline='\n') as f:
        for _ in range(a.n):
            q = expr(r, r.randint(0, 5))
            if a.mode == 'fuzz':
                x = r.random()
                q = mutate(r, q) if x < 0.4 else hostile(r) if x < 0.45 else q
            f.write(copy_escape(q) + '\n')


if __name__ == '__main__':
    main()
