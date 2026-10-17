"""Creates the plain SQLite database that the sift and FTS5 builds read: articles(id INTEGER PRIMARY KEY, body TEXT).

    python3 -I load_base.py DOCS_TSV OUT_DB [LIMIT]

DOCS_TSV is PostgreSQL COPY text: id TAB body (TAB more columns are ignored), as bench/steps/articles.sh keeps it
($SIFT_BENCH_ROOT/data/articles.tsv, 218,058 articles) or as exported from the benchmark's docs table.
"""
import os
import re
import sqlite3
import sys

ESC = re.compile(r'\\(.)')
MAP = {'n': '\n', 't': '\t', 'r': '\r', '\\': '\\', 'b': '\b', 'f': '\f', 'v': '\v'}


def unescape(s):
    return ESC.sub(lambda m: MAP.get(m.group(1), m.group(1)), s) if '\\' in s else s


def main(src, out, limit=0):
    if os.path.exists(out):
        os.unlink(out)
    db = sqlite3.connect(out)
    db.execute('CREATE TABLE articles(id INTEGER PRIMARY KEY, body TEXT NOT NULL)')
    rows = []
    with open(src, encoding='utf-8', newline='\n') as f:
        for n, line in enumerate(f, 1):
            cols = line.rstrip('\n').split('\t')
            rows.append((int(cols[0]), unescape(cols[1])))
            if limit and n >= limit:
                break
            if len(rows) == 20000:
                db.executemany('INSERT INTO articles VALUES(?, ?)', rows)
                rows = []
    db.executemany('INSERT INTO articles VALUES(?, ?)', rows)
    db.commit()
    print(db.execute('SELECT count(*), sum(length(body)), min(id), max(id) FROM articles').fetchone())


if __name__ == '__main__':
    main(sys.argv[1], sys.argv[2], int(sys.argv[3]) if len(sys.argv) > 3 else 0)
