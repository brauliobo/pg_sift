#!/usr/bin/env python3
"""Cancel and timeout torture of the cases of test/scaling/par_sorts.tsv (Spec 95 WS-95-3b).

Every case runs ITER times (20) at twice its rows in ONE backend: the even iterations under a statement_timeout of a few milliseconds, the odd ones cancelled by pg_cancel_backend from a second
session after a few milliseconds. After each iteration the backend, back at its prompt, holds no temporary file (pg_ls_tmpdir()), has raised no leak warning, and is alive; at the end its anonymous
memory is at most 2x what it was after the third iteration (a statement that is cancelled leaves nothing the next one grows on).

  scripts/par-sorts-torture.py [--only RE] [--iter N]      one TORTURE line a case; exit 1 on any failure

Reads PGHOST, PGPORT, PGUSER, PGDATABASE like psql.
"""
import importlib.util, os, random, re, sys, threading, time

HERE = os.path.dirname(os.path.abspath(__file__))
spec = importlib.util.spec_from_file_location("gate", os.path.join(HERE, "par-sorts-gate.py"))
g = importlib.util.module_from_spec(spec)
spec.loader.exec_module(g)
ms = g.ms

ITER = int(os.environ.get("PAR_SORTS_TORTURE_ITER", "20"))
WARM = 3
GROWTH_MAX = 2.0
TIMEOUT_MS = (2, 40)


def anon_kb(pid):
    with open(f"/proc/{pid}/status") as f:
        return int(next(l for l in f if l.startswith("RssAnon:")).split()[1])


def cancel_later(pid, delay_ms):
    time.sleep(delay_ms / 1000.0)
    ms.script(f"select pg_cancel_backend({pid});")


def iteration(p, c, n, i, pid):
    by_timeout = i % 2 == 0
    delay = random.randint(*TIMEOUT_MS)
    canceller = None
    if not by_timeout:
        canceller = threading.Thread(target=cancel_later, args=(pid, delay))
        canceller.start()
    out, err = ms.send(p, (f"set statement_timeout = '{delay}ms';\n" if by_timeout else "") + g.stmt(c, n) + "\nreset statement_timeout;", "OPDONE")
    if canceller:
        canceller.join()
    post, _ = ms.send(p, "select 'TMP', count(*) from pg_ls_tmpdir();", "TMPDONE")
    left = ms.num_of(post, "TMP")
    leaks = sum(1 for l in out + post if g.LEAK.search(l) and ("WARNING" in l or "ERROR" in l))
    ended = "timeout" if err and "statement timeout" in err else "cancel" if err and "canceling statement" in err else "done" if not err else "error: " + err
    return ended, left, leaks


def torture(c):
    n = c.rows * 2 if c.kind == "mem" else c.rows
    g.build(c, n)
    p = ms.psql()
    out, _ = ms.send(p, ms.SETTINGS + "select 'PID', pg_backend_pid();", "PIDDONE")
    pid = ms.num_of(out, "PID")
    ends, tmp_left, leaks, anon = {}, 0, 0, []
    try:
        for i in range(ITER):
            ended, left, lk = iteration(p, c, n, i, pid)
            ends[ended] = ends.get(ended, 0) + 1
            tmp_left += left
            leaks += lk
            anon.append(anon_kb(pid))
    except (StopIteration, BrokenPipeError, FileNotFoundError):
        return f"TORTURE {c.name} | FAIL the backend died at iteration {len(anon)}", False
    p.stdin.write("\\q\n")
    p.wait()
    growth = anon[-1] / max(anon[WARM - 1], 1)
    errors = [k for k in ends if k.startswith("error")]
    fails = [k for k, bad in (("tmp_files", tmp_left > 0), ("leaks", leaks > 0), ("growth", growth > GROWTH_MAX), ("errors", bool(errors))) if bad]
    return (f"TORTURE {c.name} | iters {ITER} {' '.join(f'{k} {v}' for k, v in sorted(ends.items()))} | tmp_left {tmp_left} leaks {leaks} | anon_kb {anon[WARM - 1]} -> {anon[-1]} (x{growth:.2f}) | " +
            ("PASS" if not fails else "FAIL " + ",".join(fails))), not fails


def main():
    global ITER
    argv = sys.argv[1:]
    only = argv[argv.index("--only") + 1] if "--only" in argv else None
    if "--iter" in argv:
        ITER = int(argv[argv.index("--iter") + 1])
    ok = True
    for c in g.read_cases(only):
        line, good = torture(c)
        print(line, flush=True)
        ok &= good
        if not good:
            g.wait_ready()
    sys.exit(0 if ok else 1)


if __name__ == "__main__":
    main()
