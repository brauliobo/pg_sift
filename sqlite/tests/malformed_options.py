"""Malformed table options and commands must end in a clean SQL error, never a crash of the host process: every
statement runs in a child interpreter, so a SIGFPE or a failed mmap shows as the child's exit status.

    python3 -I tests/malformed_options.py [-v]           (needs build/sift.so)
"""
import os
import subprocess
import sys
import unittest

sys.path.insert(0, os.path.dirname(os.path.abspath(__file__)))
from sift import EXT, TmpDb  # noqa: E402

CHILD = """
import sqlite3, sys
db = sqlite3.connect(sys.argv[2], isolation_level=None)
db.enable_load_extension(True)
db.load_extension(sys.argv[1])
try:
    for sql in sys.argv[3:]:
        db.execute(sql).fetchall()
except sqlite3.OperationalError as e:
    print(e)
    sys.exit(3)
"""

CREATE = 'CREATE VIRTUAL TABLE t USING sift(body, %s)'
BAD_OPTIONS = [
    'chunk=0', "chunk='abc'", 'chunk=-1', 'chunk=100', 'chunk=2048', 'chunk=131072', 'chunk=3000000000',
    'cache_size=-1', 'cache_size=99999999999', "cache_size='x'", 'cache_size=16777217',
    'parallel=-1', 'parallel=100000', "parallel='x'", 'automerge=-1', "automerge='x'",
    'pending_postings=-1', 'pending_postings=999', "pending_postings='x'",
    'k1=-1', "k1='nan'", "k1='inf'", "k1='x'", 'b=-0.5', 'b=2.0', "b='nan'", "b='x'",
]
GOOD_OPTIONS = ['chunk=4096', 'chunk=65536', 'cache_size=0', 'cache_size=1', 'cache_size=16777216', 'parallel=0',
                'parallel=64', 'automerge=0', 'pending_postings=1000', 'k1=0', 'k1=2.5', 'b=0', 'b=1']
BAD_COMMANDS = ["INSERT INTO t(t, rank) VALUES ('cache-size', 99999999999)",
                "INSERT INTO t(t, rank) VALUES ('cache-size', 16777217)",
                "INSERT INTO t(t, rank) VALUES ('parallel', 100000)",
                "INSERT INTO t(t, rank) VALUES ('automerge', 99999999999)"]


class MalformedOptions(TmpDb):
    def run_sql(self, *sql, fresh=True):
        if fresh and os.path.exists(self.path):
            os.remove(self.path)
        return subprocess.run([sys.executable, '-I', '-c', CHILD, EXT, self.path, *sql], capture_output=True, text=True)

    def assert_clean_error(self, *sql, fresh=True):
        p = self.run_sql(*sql, fresh=fresh)
        self.assertEqual(p.returncode, 3, f'{sql}: exit {p.returncode}, stderr {p.stderr[-300:]!r}')
        self.assertIn('sift', p.stdout)

    def test_bad_option_values_are_errors(self):
        for opt in BAD_OPTIONS:
            with self.subTest(opt=opt):
                self.assert_clean_error(CREATE % opt)

    def test_good_option_values_still_work(self):
        for opt in GOOD_OPTIONS:
            with self.subTest(opt=opt):
                self.assertEqual(self.run_sql(CREATE % opt).returncode, 0)

    def test_bad_commands_are_errors(self):
        for cmd in BAD_COMMANDS:
            with self.subTest(cmd=cmd):
                self.assert_clean_error(CREATE % 'stemmer=none', cmd)

    def test_schema_with_a_bad_chunk_is_an_error_when_opened(self):
        self.assertEqual(self.run_sql(CREATE % 'chunk=4096').returncode, 0)
        edit = ["PRAGMA writable_schema=ON", "UPDATE sqlite_master SET sql = replace(sql, 'chunk=4096', 'chunk=0') WHERE name = 't'"]
        self.assertEqual(self.run_sql(*edit, fresh=False).returncode, 0)
        self.assert_clean_error('SELECT count(*) FROM t', fresh=False)


if __name__ == '__main__':
    unittest.main()
