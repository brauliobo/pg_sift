"""Workload cases of the SQLite port of the DRY performance gate: the portable core (BM25 analyzer, query language, segments, merge, top-k) on the real hosts that compile it.

SQLite: the extension (sift.so) loaded into python's sqlite3, one fresh process per run; the window (perf stat --control) opens after the corpus and the queries are built and the
        connection is open, and closes after the statements and the commit, so it holds the extension and the SQLite it calls, not the interpreter's start.

Cases are measured like the PostgreSQL cases (instructions:u and cycles:u of the process tree, elapsed ms, md5 of the printed rows); `wal` holds the bytes of the database file after the run. The corpus is synthetic and deterministic (a generator of its own, no external data).
Run as a script this file is the driver of one SQLite run:  python3 -I ports.py sqlite MODE SO DB
"""
import hashlib
import os
import re
import shutil
import signal
import subprocess
import sys
import time
from pathlib import Path

N_DOCS = 20000
N_BATCH = 2000
MODES = {
    'sqlite': ('build', 'insert', 'update_delete', 'optimize', 'integrity', 'top10_and', 'top10_or', 'top10_phrase', 'count_and', 'count_or', 'count_phrase', 'prefix', 'fuzzy',
               'regex', 'highlight'),
}
SYL = ('ka', 'lo', 'mi', 'ren', 'tu', 'sa', 'vel', 'dor', 'an', 'pi', 'shu', 'ber', 'gan', 'ow', 'el', 'nes', 'tia', 'quo', 'zen', 'fi')
SUFFIX = {7: 'ing', 11: 's', 13: 'ed'}


# --- the corpus -------------------------------------------------------------------------------------------------------------------------------------------------
def vocabulary(n=1500):
    out = []
    for i in range(n):
        x, w = i + 1, ''
        while x:
            w += SYL[x % len(SYL)]
            x //= len(SYL)
        out.append(w + next((s for k, s in SUFFIX.items() if i % k == 0), ''))
    return out


VOCAB = vocabulary()


def doc_words(i):
    """The words of document i: a skewed draw from the vocabulary, by a generator of its own so the corpus does not depend on the python version."""
    x = (i * 2654435761 + 12345) & (2 ** 64 - 1)
    out = []
    for _ in range(30 + i * 7 % 41):
        x = (x * 6364136223846793005 + 1442695040888963407) & (2 ** 64 - 1)
        out.append(VOCAB[int(len(VOCAB) * ((x >> 11) / 2 ** 53) ** 3)])
    return out


def doc(i):
    w = doc_words(i)
    return ' '.join(w[:3]), ' '.join(w)


def queries(kind, n=60):
    """n deterministic queries of a kind from words of the corpus: runs of 2 to 7 consecutive words of a document (and / or / phrase), or one-word wildcard forms."""
    out = []
    for j in range(n):
        w = doc_words(1 + j * 7919 % N_DOCS)
        k = 2 + j % 6
        s = j * 5 % (len(w) - k)
        t = w[s:s + k]
        a = t[0]
        out.append({'and': ' AND '.join(t), 'or': ' OR '.join(t), 'phrase': '"' + ' '.join(t) + '"', 'prefix': a[:3] + '*', 'fuzzy': a + '~1',
                    'regex': '/^' + a[:2] + '[a-z]*' + a[-2:] + '$/'}[kind])
    return out


# --- SQLite driver (one run) ----------------------------------------------------------------------------------------------------------------------------------
class Window:
    """The measured window: `enable` / `disable` to the control fifo of perf stat and wait for its acknowledgement."""

    def __init__(self):
        self.ctl, self.ack, self.t0, self.ms = os.environ.get('GATE_CTL'), os.environ.get('GATE_ACK'), 0, 0

    def send(self, cmd):
        if self.ctl:
            with open(self.ctl, 'w') as f:
                f.write(cmd + '\n')
            with open(self.ack) as f:
                f.readline()

    def __enter__(self):
        self.send('enable')
        self.t0 = time.perf_counter()

    def __exit__(self, *a):
        self.ms += (time.perf_counter() - self.t0) * 1000
        self.send('disable')


def sqlite_read(db, win, mode):
    kind = mode.split('_', 1)[-1]
    if mode.startswith('top10'):
        sql, rows = 'SELECT rowid FROM docs WHERE docs MATCH ? ORDER BY rank LIMIT 10', lambda q: [r[0] for r in db.execute(sql, (q,))]
    elif mode.startswith('count') or mode in ('prefix', 'fuzzy', 'regex'):
        sql, rows = "SELECT sift_count('docs', ?)", lambda q: [db.execute(sql, (q,)).fetchone()[0]]
    else:
        sql, rows = "SELECT sift_highlight(docs, 1, '[', ']') FROM docs WHERE docs MATCH ? ORDER BY rank LIMIT 3", lambda q: [r[0] for r in db.execute(sql, (q,))]
        kind = 'or'
    qs = queries(kind, 20 if mode in ('fuzzy', 'regex', 'highlight') else 60)
    for q in qs:                      # warm: the same statements once before the window
        rows(q)
    out = []
    with win:
        for q in qs:
            out.append(rows(q))
    return out


def sqlite_write(db, win, mode, path):
    ins = 'INSERT INTO docs(rowid, title, body) VALUES (?, ?, ?)'
    if mode == 'build':
        rows = [(i, *doc(i)) for i in range(1, N_DOCS + 1)]
        with win:
            db.execute('CREATE VIRTUAL TABLE docs USING sift(title, body)')
            db.executemany(ins, rows)
            db.commit()
    elif mode == 'insert':
        rows = [(i, *doc(i)) for i in range(N_DOCS + 1, N_DOCS + N_BATCH + 1)]
        with win:
            db.executemany(ins, rows)
            db.commit()
    elif mode == 'update_delete':
        upd = [(*doc(i + 100000)[1:], i) for i in range(1, 501)]
        with win:
            db.executemany('UPDATE docs SET body = ? WHERE rowid = ?', upd)
            db.execute('DELETE FROM docs WHERE rowid % 40 = 3')
            db.commit()
    elif mode == 'optimize':
        for b in range(3):            # three more segments to merge, outside the window
            db.executemany(ins, [(i, *doc(i)) for i in range(N_DOCS + 1 + b * 500, N_DOCS + 501 + b * 500)])
            db.commit()
            db.execute('BEGIN')
        with win:
            db.execute("INSERT INTO docs(docs) VALUES('optimize')")
            db.commit()
    else:
        with win:
            db.execute("INSERT INTO docs(docs, rank) VALUES('integrity-check', 1)")
    db.commit()
    return [db.execute("SELECT count(*) FROM docs").fetchone()[0], db.execute("SELECT sift_count('docs', 'ka')").fetchone()[0]]


def sqlite_main(mode, so, path):
    import sqlite3
    db = sqlite3.connect(path, isolation_level=None)
    db.enable_load_extension(True)
    db.load_extension(so)
    db.execute('PRAGMA synchronous = OFF')
    db.execute('PRAGMA cache_size = -200000')
    win = Window()
    if mode == 'template':
        db.execute('BEGIN')
        db.execute('CREATE VIRTUAL TABLE docs USING sift(title, body)')
        db.executemany('INSERT INTO docs(rowid, title, body) VALUES (?, ?, ?)', [(i, *doc(i)) for i in range(1, N_DOCS + 1)])
        db.execute('COMMIT')
        return
    db.execute('BEGIN')
    out = sqlite_write(db, win, mode, path) if mode in MODES['sqlite'][:5] else sqlite_read(db, win, mode)
    print('\n'.join(map(str, out)))
    print(f'@@MS {win.ms:.3f}')


# --- the sides of the gate ------------------------------------------------------------------------------------------------------------------------------------
def dir_bytes(p):
    p = Path(p)
    return sum(f.stat().st_size for f in ([p] if p.is_file() else p.rglob('*')) if f.is_file())


def under_perf(cmd, env, rundir, window):
    """cmd under `perf stat`; with a window the counters stay off until the command enables them through the control fifo. Returns (stdout, stderr, returncode, {event: count})."""
    out = rundir / 'perf.out'
    out.unlink(missing_ok=True)
    pc = ['perf', 'stat', '-e', 'instructions:u,cycles:u', '-x,', '-o', str(out)]
    if window:
        ctl, ack = rundir / 'ctl', rundir / 'ack'
        for f in (ctl, ack):
            f.unlink(missing_ok=True)
            os.mkfifo(f)
        pc += ['--control', f'fifo:{ctl},{ack}', '--delay=-1']
        env = {**env, 'GATE_CTL': str(ctl), 'GATE_ACK': str(ack)}
    p = subprocess.Popen(pc + ['--'] + cmd, env=env, stdout=subprocess.PIPE, stderr=subprocess.PIPE, text=True)
    try:
        so, se = p.communicate(timeout=600)
    except subprocess.TimeoutExpired:
        p.send_signal(signal.SIGKILL)
        raise
    vals = {}
    for line in out.read_text().splitlines():
        f = line.split(',')
        if len(f) > 2 and f[2].startswith(('instructions', 'cycles')):
            vals[f[2].split(':')[0]] = int(f[0]) if f[0].isdigit() else None
    return so, se, p.returncode, vals


class PortSide:
    """One build of a port (the directory a PortableCheck made) and the template data both sides copy; run() is what run_once calls for a port case."""

    def __init__(self, name, build, template):
        self.name, self.build, self.template = name, Path(build), Path(template)

    def run(self, case, rundir, tag):
        mode = case.sql
        work = rundir / f'work-{self.name}'
        shutil.rmtree(work, ignore_errors=True)
        work.mkdir(parents=True)
        env = {**os.environ, 'LC_ALL': 'C', 'SIFT_TMP': str(work)}
        db = work / 'gate.db'
        if mode != 'build':
            shutil.copy(self.template / 'gate.db', db)
        cmd, window, size = [sys.executable, '-I', str(Path(__file__).resolve()), 'sqlite', mode, str(self.build / 'sqlite' / 'sift.so'), str(db)], True, db
        t0 = time.perf_counter()
        so, se, rc, vals = under_perf(cmd, env, rundir, window)
        wall = (time.perf_counter() - t0) * 1000
        r = dict(tag=tag, ok=rc == 0)
        if rc or vals.get('instructions') is None:
            r.update(ok=False, error=(se.strip().splitlines() or [f'exit {rc}'])[-1][:200])
            return r
        ms = re.search(r'^@@MS ([0-9.]+)$', so, re.M)
        rows = sorted(l for l in so.splitlines() if not l.startswith('@@MS'))
        r.update(instr=vals['instructions'], cycles=vals['cycles'], ms=float(ms.group(1)) if ms else wall, hits=0, reads=0, writes=0, extends=0, wal=dir_bytes(size),
                 rows=hashlib.md5('\n'.join(rows).encode()).hexdigest()[:12], nrows=len(rows))
        return r


def object_hashes(root):
    """sha256 of every object file under root without its debug sections: equal for two builds of sources that compile to the same code."""
    import concurrent.futures
    root = Path(root)

    def one(f):
        tmp = f.with_suffix('.strip')
        subprocess.run(['objcopy', '--strip-debug', str(f), str(tmp)], check=True)
        h = hashlib.sha256(tmp.read_bytes()).hexdigest()[:16]
        tmp.unlink()
        return str(f.relative_to(root)), h
    with concurrent.futures.ThreadPoolExecutor(4) as ex:
        return dict(ex.map(one, sorted(root.rglob('*.o'))))


def make_template(base_build, out, hosts):
    """The data both sides read, made by the BASE build: the SQLite database and the MariaDB index directory of the corpus."""
    out.mkdir(parents=True, exist_ok=True)
    env = {**os.environ, 'LC_ALL': 'C', 'SIFT_TMP': str(out)}
    if 'sqlite' in hosts:
        (out / 'gate.db').unlink(missing_ok=True)
        subprocess.run([sys.executable, '-I', str(Path(__file__).resolve()), 'sqlite', 'template', str(Path(base_build) / 'sqlite' / 'sift.so'), str(out / 'gate.db')], env=env, check=True)

if __name__ == '__main__' and len(sys.argv) == 5 and sys.argv[1] == 'sqlite':
    sqlite_main(*sys.argv[2:])
