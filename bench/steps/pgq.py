"""One psql session that runs some statements and times the last one: the helper of the benchmarks that drive a local cluster with `psql -d DB` (steps/sets.py, steps/scaling.py). Standard library only."""
import os
import re
import subprocess

TIME = re.compile(r'Time: ([0-9.]+) ms( \([0-9:.]+\))?\n?')


def timed(db, sqls, setup=(), timed=True, timeout=600):
    """(ms of the last statement as psql timed it, stdout without the timing lines) of SETUP and SQLS in one session; a statement ending in \\g or \\gexec is sent as it is"""
    last = sqls[-1]
    script = ''.join(s + ';\n' for s in [*setup, *sqls[:-1]]) + ('\\timing on\n' if timed else '') + last + ('\n' if last.rstrip().endswith(('\\g', '\\gexec')) else ';\n')
    p = subprocess.run(['psql', '-X', '-qAt', '-v', 'ON_ERROR_STOP=1', '-d', db], input=script, capture_output=True, text=True, timeout=timeout, env=dict(os.environ, LC_ALL='C'))
    if p.returncode:
        raise RuntimeError(p.stderr.strip()[:300] + ' :: ' + last[:120])
    t = TIME.findall(p.stdout)
    return (float(re.findall(r'Time: ([0-9.]+) ms', p.stdout)[-1]) if t else None), TIME.sub('', p.stdout)


class Backend:
    """one psql backend kept open: the output of a statement ends at a marker line (steps/store_engine.py, steps/gap.py). A statement runs with its output discarded (\\o /dev/null)
    unless `capture`; STRICT raises on an ERROR or FATAL line of the output, else they are part of the lines returned; STDERR is merged into the output (None: left to the terminal)"""

    def __init__(self, argv, env=None, setup=(), strict=True, stderr=subprocess.STDOUT):
        self.strict = strict
        self.p = subprocess.Popen(argv, stdin=subprocess.PIPE, stdout=subprocess.PIPE, stderr=stderr, text=True, bufsize=1, env=env)
        try:
            for sql in [*setup, '\\o /dev/null']:
                self.run(sql)
        except BaseException:   # no backend (the server is starting): reap this psql
            self.p.kill()
            self.p.communicate()
            raise

    def run(self, sql, capture=False):
        w = self.p.stdin.write
        if capture:
            w('\\o\n')
        w(sql + ('' if sql.startswith('\\') else ';') + '\n')
        if capture:
            w('\\o /dev/null\n')
        w('\\echo __DONE__\n')
        self.p.stdin.flush()
        out = []
        while True:
            line = self.p.stdout.readline()
            if line == '':
                raise RuntimeError('psql ended: ' + ' '.join(out[-3:]))
            if line.strip() == '__DONE__':
                break
            out.append(line.rstrip('\n'))
        bad = [l for l in out if l.startswith(('ERROR', 'FATAL'))]
        if self.strict and bad:
            raise RuntimeError(bad[0][:300])
        return out

    def close(self):
        self.p.stdin.close()
        self.p.wait()
