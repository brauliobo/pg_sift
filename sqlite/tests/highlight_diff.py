"""Digest of sift_highlight / sift_snippet answers (on a row and on any text) over random texts and queries, to compare two builds:
the digests of the same cases must be equal.

    SIFT_EXT=build/sift python3 -I tests/highlight_diff.py [CASES]
"""
import hashlib
import os
import random
import sqlite3
import sys
import tempfile

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sift import EXT  # noqa: E402

CASES = int(sys.argv[1]) if len(sys.argv) > 1 else 3000
WORDS = ['alpha', 'beta', 'gamma', 'delta', 'running', 'runs', 'ran', 'quick', 'quickly', 'resume', 'cafe', 'naive', 'one', 'two', 'x1']
QUERIES = ['alpha', 'alpha beta', '"alpha beta"', '"alpha beta" OR beta', 'beta OR "alpha beta"', 'alpha*', 'al?ha', 'a*a', 'alpha NOT beta',
           'gamma OR delta OR runs', '"one two" "two one"', 'quik~1', 'quick~2 OR "x1 one"', '/r.n.*/', 'alpha AND (beta OR gamma)',
           'title:alpha', 'body:beta OR title:gamma', '"alpha beta gamma"', 'one "one two" two']


def main():
    path = os.path.join(tempfile.mkdtemp(prefix='sift-opt66b-hl-'), 'h.db')
    db = sqlite3.connect(path)
    db.enable_load_extension(True)
    db.load_extension(EXT)
    db.execute('CREATE VIRTUAL TABLE t USING sift(title, body, stemmer=none)')
    rnd = random.Random(5)
    digest = hashlib.sha256()
    rows = []
    for i in range(1, 201):
        title = ' '.join(rnd.choice(WORDS) for _ in range(rnd.randint(1, 6)))
        body = ' '.join(rnd.choice(WORDS) for _ in range(rnd.randint(3, 60)))
        rows.append((i, title, body))
        db.execute('INSERT INTO t(rowid, title, body) VALUES (?, ?, ?)', rows[-1])
    for q in QUERIES:
        for col in (0, 1):
            for r in db.execute("SELECT rowid, sift_highlight(t, ?, '<', '>'), sift_snippet(t, ?, '[', ']', '..', 8) FROM t WHERE t MATCH ? ORDER BY rowid",
                                (col, col, q)).fetchall():
                digest.update(repr(r).encode())
    for _ in range(CASES):
        q = rnd.choice(QUERIES)
        text = ' '.join(rnd.choice(WORDS) for _ in range(rnd.randint(0, 80)))
        digest.update(repr(db.execute("SELECT sift_highlight_text('t', ?, ?, '<', '>'), sift_snippet_text('t', ?, ?, '[', ']', '..', 6)",
                                      (text, q, text, q)).fetchone()).encode())
    print('highlight_diff', digest.hexdigest())


if __name__ == '__main__':
    main()
