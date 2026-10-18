#!/usr/bin/env python3
# cpu_time.py OUT CMD...: runs CMD and writes to OUT the CPU seconds (user + system) of CMD and of every process it left behind: it is a
# subreaper, so a postmaster that pg_ctl daemonized is reaped here and counted with the backends it reaped. Exits with the status of CMD.
# scripts/ci.sh uses it for the per-job CPU column.
import ctypes, os, resource, sys

ctypes.CDLL(None).prctl(36, 1, 0, 0, 0)   # PR_SET_CHILD_SUBREAPER
pid = os.fork()
if pid == 0:
    os.execvp(sys.argv[2], sys.argv[2:])
status = None
while status is None:
    try:
        p, st = os.waitpid(-1, 0)
    except ChildProcessError:
        break
    if p == pid:
        status = st
while True:
    try:
        if os.waitpid(-1, os.WNOHANG)[0] == 0:
            break
    except ChildProcessError:
        break
r = resource.getrusage(resource.RUSAGE_CHILDREN)
open(sys.argv[1], 'w').write('%.2f\n' % (r.ru_utime + r.ru_stime))
sys.exit(os.waitstatus_to_exitcode(status) if status is not None else 1)
