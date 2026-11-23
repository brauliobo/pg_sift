"""The format contract of the fuzzers: --format f2|f1|release (SIFT_TEST_FORMAT, default f2).

  f2       the test library, new indexes and tables in Format 2.0 (sift.test_create_format1 = off)
  f1       the test library, new indexes and tables in Format 1 (sift.test_create_format1 = on)
  release  the release library: Format 2.0 only, no sift.test_* / sift._test_* setting or function

The library is probed once: a setting that only the test library registers (sift.test_create_format1) says whether the
hooks exist. Every stress knob of a fuzzer goes through Lib.sets() or is guarded by Lib.testing / Lib.v1_hooks, so a
fuzzer runs end to end on the release library.
"""
import os
import subprocess
import sys

FORMATS = ('f2', 'f1', 'release')
TESTING_PROBE = 'sift.test_create_format1'


def add_argument(ap):
    ap.add_argument('--format', choices=FORMATS, default=os.environ.get('SIFT_TEST_FORMAT') or 'f2',
                    help='f2: Format 2.0 on the test library (default); f1: Format 1 on the test library; release: the release library')


def libpq_env(extra=None):
    env = dict(os.environ, LC_ALL='C')
    for k in ('PGDATABASE', 'PGSERVICE', 'PGOPTIONS'):
        env.pop(k, None)
    env.update(extra or {})
    return env


class Lib:
    def __init__(self, name, db='postgres', env=None):
        env = env or libpq_env()
        r = subprocess.run(['psql', '-X', '-q', '-A', '-t', '-U', env.get('PGUSER', 'postgres'), '-d', db, '-c', "LOAD 'pg_sift'",
                            '-c', "SELECT name FROM pg_settings WHERE name LIKE 'sift.%'"], env=env, capture_output=True, text=True)
        if r.returncode:
            sys.exit(f'fmt: cannot probe the library: {r.stderr.strip()}')
        self.name = name
        self.gucs = set(r.stdout.split())
        self.testing = TESTING_PROBE in self.gucs
        if name == 'f1' and not self.testing:
            sys.exit('--format f1 needs the test library (SIFT_TESTING=1)')
        if name != 'release' and not self.testing:
            sys.exit(f'--format {name} needs the test library (SIFT_TESTING=1); use --format release on the release library')
        if name == 'release' and self.testing:
            sys.exit('--format release needs the release library (SIFT_TESTING=)')
        self.format_major = 1 if name == 'f1' else 2
        self.v1_hooks = self.testing and self.format_major == 1

    @property
    def pgoptions(self):
        return f'-c {TESTING_PROBE}={"on" if self.format_major == 1 else "off"}' if self.testing else ''

    def env(self, extra_options=''):
        return libpq_env({'PGOPTIONS': f'{self.pgoptions} {extra_options}'.strip()})

    def has(self, guc):
        return not guc.startswith('sift.') or guc in self.gucs

    def sets(self, settings):
        """the 'name=value' settings this library accepts"""
        return [s for s in settings if self.has(s.split('=', 1)[0])]

    def set_sql(self, settings, local=False):
        return ''.join(f"SET {'LOCAL ' if local else ''}{s.split('=', 1)[0]} = '{s.split('=', 1)[1]}'; " for s in self.sets(settings))

    def hook_sql(self, sql):
        """sql when the test library is loaded, else an empty statement"""
        return sql if self.testing else 'SELECT 1'
