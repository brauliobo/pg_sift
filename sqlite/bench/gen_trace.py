"""A query trace like the one of bench/sql/load.sql, without PostgreSQL: runs of 2..15 consecutive plain words
from random documents, each rendered as and / or / phrase.

    python3 -I gen_trace.py ARTICLES_DB NSUB > trace.tsv       (columns: sub, mode, terms)

The benchmark itself draws them with hashint8 and ts_debug('simple'); the scripts here read that trace when a PostgreSQL
export of it exists (bench.py --trace), and use this generator otherwise.
"""
import random
import re
import sqlite3
import sys

WORD = re.compile(r'[^\W_]+', re.UNICODE)


def main(path, nsub):
    db = sqlite3.connect(path)
    rnd = random.Random(7)
    n = db.execute('SELECT max(id) FROM articles').fetchone()[0]
    sub = 0
    while sub < nsub:
        body = db.execute('SELECT body FROM articles WHERE id = ?', (1 + rnd.randrange(n),)).fetchone()
        if body is None:
            continue
        words = [w.lower() for w in WORD.findall(body[0]) if len(w) <= 100 and w.isascii()]
        k = 2 + rnd.randrange(14)
        if len(words) < k:
            continue
        s = rnd.randrange(len(words) - k + 1)
        sub += 1
        for mode in ('and', 'or', 'phrase'):
            print(sub, mode, ' '.join(words[s:s + k]), sep='\t')


main(sys.argv[1], int(sys.argv[2]))
