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
