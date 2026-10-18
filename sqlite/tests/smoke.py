"""Smoke test of a built or packaged extension: loads it into the system SQLite and runs the quick start of README.md.

    python3 -I tests/smoke.py PATH/TO/sift[.so] [EXPECTED_VERSION]

Needs only python3 with loadable-extension support (and, when present, the sqlite3 shell); no test data.
"""
import os
import shutil
import sqlite3
import subprocess
import sys
import tempfile

ext = os.path.abspath(sys.argv[1])
version = sys.argv[2] if len(sys.argv) > 2 else None

db = sqlite3.connect(':memory:')
db.enable_load_extension(True)
db.load_extension(ext)
got = db.execute('SELECT sift_version()').fetchone()[0]
assert got.startswith('sift-sqlite '), got
assert version is None or got.split()[1] == version, f'sift_version() is {got!r}, expected {version}'

db.execute('CREATE VIRTUAL TABLE docs USING sift(title, body)')
db.executemany('INSERT INTO docs(rowid, title, body) VALUES (?, ?, ?)', [
    (1, 'Wireless headphones', 'Battery was great and the support was friendly. The case was squeaky.'),
    (2, 'Charger', 'Friendly support, no battery, but a sturdy case and clip.'),
    (3, 'Resume tips', 'A good résumé is short. Cafés near the office help.'),
    (4, 'Running', 'Runners were running; she runs every day.'),
])


def rows(q, *args):
    return [r[0] for r in db.execute(q, args)]


assert rows("SELECT rowid FROM docs WHERE docs MATCH 'battery friendly'") == [1, 2]
assert rows("SELECT rowid FROM docs WHERE docs MATCH '\"friendly support\"'") == [2]
assert rows("SELECT rowid FROM docs WHERE docs MATCH 'resume AND cafe'") == [3]
assert rows("SELECT rowid FROM docs WHERE docs MATCH 'run' ORDER BY rank LIMIT 5") == [4]
assert rows("SELECT sift_count('docs', 'friendly')") == [2]
assert db.execute("SELECT sift_highlight(docs, 1, '[', ']') FROM docs WHERE docs MATCH 'battery'").fetchone()[0].count('[') >= 1
db.execute("INSERT INTO docs(docs) VALUES('integrity-check')")
db.execute("INSERT INTO docs(docs) VALUES('optimize')")
assert rows("SELECT rowid FROM docs WHERE docs MATCH 'battery friendly'") == [1, 2]
db.close()

sqlite3_shell = shutil.which('sqlite3')
if sqlite3_shell:
    with tempfile.TemporaryDirectory() as tmp:
        r = subprocess.run([sqlite3_shell, os.path.join(tmp, 'smoke.db'), f'.load {ext}',
                            'CREATE VIRTUAL TABLE t USING sift(body);', "INSERT INTO t(rowid, body) VALUES (1, 'hello world');",
                            "SELECT rowid FROM t WHERE t MATCH 'hello';"], capture_output=True, text=True)
        assert r.returncode == 0 and r.stdout.strip() == '1', r.stderr
print(f'smoke test passed: {got}' + ('' if sqlite3_shell else ' (no sqlite3 shell: only the python module was tested)'))
