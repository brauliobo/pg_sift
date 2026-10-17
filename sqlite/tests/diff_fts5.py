"""Differential check of match sets: sift (stemmer=none) against FTS5 (unicode61 remove_diacritics 2, no stemmer).

    python3 -I diff_fts5.py [--articles DB] [--ascii] [--n 300] [--seed 1] [--ext build/sift] [--show 5]

DB holds articles(id, body) and gets the two virtual tables added (use a copy); without it a synthetic Zipf corpus is
generated. For random queries built from words of random documents (AND, OR, phrase, prefix, AND NOT) the complete rowid
sets of both engines are compared. Exit status 1 when any set differs.

Analyzer differences seen on the Simple English Wikipedia (all outside ASCII, so --ascii agrees exactly): sift ends a
token at a script change (digits next to CJK: "2002年" is "2002" for sift, one token for unicode61), folds stroke letters
(Đ to d) that unicode61 keeps, and joins words across the soft hyphen, modifier letters and combining marks (IPA
"faˈdʊt͡s" is one word for sift, unicode61 splits it and indexes "s").
"""
import argparse
import os
import random
import re
import sqlite3
import sys

WORD = re.compile(r'[^\W_]+', re.UNICODE)


def synthetic(db, ndocs=4000):
    rnd = random.Random(3)
    vocab = [''.join(rnd.choice('abcdefghilmnoprstu') for _ in range(rnd.randrange(3, 9))) for _ in range(3000)]
    weights = [1 / (i + 1) for i in range(len(vocab))]
    db.execute('CREATE TABLE articles(id INTEGER PRIMARY KEY, body TEXT)')
    db.executemany('INSERT INTO articles VALUES (?, ?)',
                   ((i, ' '.join(rnd.choices(vocab, weights, k=rnd.randrange(5, 80)))) for i in range(1, ndocs + 1)))


def fts5_term(w):
    return '"' + w + '"'


def make_queries(docs, rnd, n):
    out = []
    while len(out) < n * 5:
        words = [w.lower() for w in WORD.findall(rnd.choice(docs)) if len(w) < 60]
        if len(words) < 8:
            continue
        s = rnd.randrange(len(words) - 4)
        a, b, c = words[s], words[s + 1], words[s + 2]
        other = rnd.choice(rnd.choice(docs).split()).lower()
        other = (WORD.findall(other) or [a])[0]
        out.append(('and', f'{a} AND {b} AND {c}', f'{fts5_term(a)} AND {fts5_term(b)} AND {fts5_term(c)}'))
        out.append(('or', f'{a} OR {other}', f'{fts5_term(a)} OR {fts5_term(other)}'))
        out.append(('phrase', f'"{a} {b} {c}"', f'"{a} {b} {c}"'))
        out.append(('prefix', f'{a[:3]}*', f'{fts5_term(a[:3])} *'))
        out.append(('not', f'{a} AND NOT {other}', f'{fts5_term(a)} NOT {fts5_term(other)}'))
    return out


def main():
    ap = argparse.ArgumentParser()
    ap.add_argument('--articles')
    ap.add_argument('--n', type=int, default=300)
    ap.add_argument('--seed', type=int, default=1)
    ap.add_argument('--show', type=int, default=5)
    ap.add_argument('--ascii', action='store_true', help='index only documents that are pure ASCII')
    ap.add_argument('--ext', default=os.path.join(os.path.dirname(os.path.abspath(__file__)), '..', 'build', 'sift'))
    a = ap.parse_args()
    db = sqlite3.connect(a.articles or ':memory:')
    db.enable_load_extension(True)
    db.load_extension(a.ext)
    if not a.articles:
        synthetic(db)
    db.execute('CREATE VIRTUAL TABLE s USING sift(body, stemmer=none)')
    db.execute("CREATE VIRTUAL TABLE f USING fts5(body, tokenize='unicode61 remove_diacritics 2')")
    db.create_function('is_ascii', 1, lambda t: t.isascii() or not a.ascii)
    db.execute('INSERT INTO s(rowid, body) SELECT id, body FROM articles WHERE is_ascii(body)')
    db.execute('INSERT INTO f(rowid, body) SELECT id, body FROM articles WHERE is_ascii(body)')
    rnd = random.Random(a.seed)
    docs = [r[0] for r in db.execute('SELECT body FROM articles WHERE is_ascii(body) ORDER BY random() LIMIT 2000')]
    stats, shown = {}, {}
    for cls, sq, fq in make_queries(docs, rnd, a.n):
        c = stats.setdefault(cls, [0, 0])
        if c[0] >= a.n:
            continue
        c[0] += 1
        got = {r[0] for r in db.execute('SELECT rowid FROM s WHERE s MATCH ?', (sq,))}
        want = {r[0] for r in db.execute('SELECT rowid FROM f WHERE f MATCH ?', (fq,))}
        if got != want:
            c[1] += 1
            if shown.setdefault(cls, 0) < a.show:
                shown[cls] += 1
                print(f'DIFF {cls} {sq!r}: sift {len(got)}, fts5 {len(want)}, only sift {sorted(got - want)[:5]}, only fts5 {sorted(want - got)[:5]}')
    for cls, (n, bad) in stats.items():
        print(f'{cls}: {n} queries, {bad} differ')
    sys.exit(1 if any(bad for _, bad in stats.values()) else 0)


main()
