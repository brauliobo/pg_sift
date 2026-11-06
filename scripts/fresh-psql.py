#!/usr/bin/env python3
"""psql for pg_regress with a NEW backend for every top-level statement (scripts/pgtest.sh fresh; test/fresh/README.md).

Put first in PATH as `psql` (FRESH_PSQL names the real one): it reads the script pg_regress feeds on stdin and puts a silent `\\connect` before every statement
that is not inside a transaction block, replaying the session's SET / RESET first, so that a statement never meets the caches, the memory contexts or the
shared state an earlier statement of the same backend left. The output is the psql output with the echo of the inserted lines removed: pg_regress compares it
with the expected file as for any run, and a difference is what a cold backend does differently.

From the first statement that binds the session (PREPARE, DECLARE, LISTEN, a temporary table, a session advisory lock, set_config, setseed, \\if, \\i) the script runs in the
backend it has: a new connection would lose what the rest of the file uses. FRESH_STATS names a file that gets one line "new backends<TAB>statements" per run."""
import os, re, subprocess, sys

# two spaces: psql reads it as the same command, and its echo can be told from a "\\set ECHO none" of the test (an included file has one), which stays in the output
CTRL = "\\set  ECHO none"
RESTORE = "\\set ECHO all"
BIND = re.compile(r"^\s*(prepare(?!\s+transaction)|declare|listen|load)\b|\bcreate\s+(global\s+|local\s+)?temp(orary)?\b|\bpg_(try_)?advisory_lock(_shared)?\s*\(|\b(set_config|setseed)\s*\(|\bpg_temp\b", re.I)
G_META = re.compile(r"\\(g\w*|crosstabview|watch)\b")
RUNS_QUERY = re.compile(r"\\(d\w*|z|l\+?|sf\+?|sv\+?|dp)\b")
COPY_STDIN = re.compile(r"^\s*copy\b.*\bfrom\s+stdin", re.I | re.S)


def bare(t):
    return re.sub(r"--[^\n]*|/\*.*?\*/", " ", t, flags=re.S).strip()


SETTING = re.compile(r"(?:set|reset)\s+(?:session\s+)?(session\s+authorization|time\s+zone|[\w.]+)", re.I)


def setting(t):
    m = SETTING.match(t)
    k = re.sub(r"\s+", " ", m.group(1).lower()) if m else t
    return "timezone" if k == "time zone" else k


def remember(state, t):
    """The SETs of the session since the last RESET of the setting, in order: the last one may be a SET that failed (a role that may not change the setting) and the value is the one before it"""
    k = setting(t)
    if k == "session authorization":
        forget(state, "reset role")
    # a SET that failed in the test fails again at every connection: replayed in a block that keeps its error to itself (also a SET ROLE of a role dropped since)
    state.append((k, "DO $sift_fresh$ BEGIN EXECUTE $sift_stmt$%s$sift_stmt$; EXCEPTION WHEN OTHERS THEN NULL; END $sift_fresh$;\n" % t))


def forget(state, t):
    k = setting(t)
    state[:] = [e for e in state if e[0] != k]


def transform(text, restore=RESTORE, load=""):
    out, state = [], []
    fresh = stmts = 0
    in_txn = bound = copy_data = False
    mode, tag, depth, paren = None, "", 0, 0
    sig, cur = False, ""

    def end_stmt():
        nonlocal in_txn, bound, copy_data, sig, cur, stmts
        t = bare(cur).rstrip(";").rstrip()
        sig, cur = False, ""
        stmts += 1
        w = re.match(r"\w+", t)
        kw = w.group(0).lower() if w else ""
        if kw in ("begin", "start"):
            in_txn = True
        elif kw in ("commit", "end", "rollback", "abort"):
            in_txn = bool(re.match(r"rollback\s+to\b", t, re.I))
        elif re.match(r"prepare\s+transaction\b", t, re.I):
            in_txn = False
        elif kw == "set" and not re.match(r"set\s+(local|transaction|constraints)\b", t, re.I):
            remember(state, t)
        elif kw == "reset" or kw == "discard":
            state.clear() if re.match(r"(reset\s+all|discard)\b", t, re.I) else forget(state, t)
        if BIND.search(t):
            bound = True
        copy_data = bool(COPY_STDIN.match(t))

    for line in text.splitlines(keepends=True):
        if copy_data:
            out.append(line)
            copy_data = line.strip() != "\\."
            continue
        s = line.lstrip()
        if mode is None and not sig and s and not s.startswith(("--", "/*")) and (not s.startswith("\\") or RUNS_QUERY.match(s)) and not in_txn and not bound:
            out.append(CTRL + "\n\\connect\n" + load + "".join(e[1] for e in state) + restore + "\n")
            fresh += 1
        out.append(line)
        i, n = 0, len(line)
        while i < n:
            i0, c = i, line[i]
            if mode == "sq" or mode == "esq":
                if mode == "esq" and c == "\\":
                    i += 1
                elif c == "'":
                    mode = None
            elif mode == "dq":
                if c == '"':
                    mode = None
            elif mode == "dollar":
                if line.startswith(tag, i):
                    i += len(tag) - 1
                    mode = None
            elif mode == "block":
                if line.startswith("*/", i):
                    i, depth = i + 1, depth - 1
                    mode = "block" if depth else None
                elif line.startswith("/*", i):
                    i, depth = i + 1, depth + 1
            elif line.startswith("--", i):
                cur += line[i:]
                break
            elif line.startswith("/*", i):
                mode, depth, i = "block", 1, i + 1
            elif c == "\\":
                m = line[i:]
                if line.startswith("\\;", i):
                    i += 1
                elif sig:
                    if G_META.match(m):
                        end_stmt()
                    break
                else:
                    echo_set = re.match(r"\\set\s+ECHO\s+(\w+)", m)
                    if echo_set:
                        restore = "\\set ECHO " + echo_set.group(1)
                    if re.match(r"\\(c|connect)\b", m):
                        state.clear()
                    elif re.match(r"\\(if|i|ir|include\w*)\b", m):
                        bound = True
                    elif re.match(r"\\copy\b", m) and re.search(r"\bfrom\s+(p?stdin)", m, re.I):
                        copy_data = True
                    break
            elif c in "'\"" or (c == "$" and (i == 0 or not re.match(r"\w", line[i - 1]))):
                if c == "'":
                    mode = "esq" if i and line[i - 1] in "eE" and (i == 1 or not re.match(r"\w", line[i - 2])) else "sq"
                elif c == '"':
                    mode = "dq"
                else:
                    d = re.match(r"\$([A-Za-z_\x80-\U0010ffff][\w\x80-\U0010ffff]*)?\$", line[i:])
                    if d:
                        mode, tag = "dollar", d.group(0)
                        i += len(tag) - 1
                sig = True
            elif not c.isspace():
                sig = True
                if c == "(":
                    paren += 1
                elif c == ")":
                    paren = max(0, paren - 1)
                elif c == ";" and not paren and not (re.match(r"create\s+(or\s+replace\s+)?(function|procedure)\b", bare(cur), re.I)
                                                     and re.search(r"\bbegin\s+atomic\b", cur, re.I) and not re.search(r"\bend\s*$", cur, re.I)):
                    cur += c
                    end_stmt()
                    i += 1
                    continue
            i += 1
            cur += line[i0:i]
    return "".join(out), fresh, stmts


def main(argv):
    real = os.environ.get("FRESH_PSQL", "")
    # a shim that runs itself (FRESH_PSQL unset, or found in PATH after the shim) would start itself without end: refuse
    if not real or os.path.realpath(real) == os.path.realpath(__file__) or os.environ.get("FRESH_ACTIVE"):
        sys.exit("fresh-psql: FRESH_PSQL must name the real psql (scripts/pgtest.sh fresh sets it), and the shim does not run inside itself")
    # pg_regress runs psql with a command of its own (CREATE DATABASE) too, and with the script on stdin only for a test: only the latter is made fresh
    if any(a in ("-c", "-f", "--command", "--file") or a.startswith(("--command=", "--file=")) for a in argv):
        os.execv(real, [real] + argv)
    # the echo the caller asked for (pg_regress: -a) comes back after the silent connection
    echo = "all" if any(a in ("-a", "--echo-all") or re.fullmatch(r"-[XqAtnbxa]*a[XqAtnbxa]*", a) for a in argv) else "queries" if "-e" in argv else "none"
    # FRESH_LOAD=LIBRARY: the library is loaded first in every new backend, as shared_preload_libraries / session_preload_libraries would (a setting of the extension is
    # unknown before the library is loaded: a test that sets or shows one differs in the default mode, by design)
    # a role that is not a superuser cannot LOAD: it meets the library as it does without the option
    load = "DO $sift_fresh$ BEGIN EXECUTE 'LOAD ''%s'''; EXCEPTION WHEN OTHERS THEN NULL; END $sift_fresh$;\n" % os.environ["FRESH_LOAD"] if os.environ.get("FRESH_LOAD") else ""
    script, fresh, stmts = transform(sys.stdin.read(), "\\set ECHO " + echo, load)
    p = subprocess.run([real] + argv, input=script.encode(), stdout=subprocess.PIPE, stderr=subprocess.STDOUT,
                       env=dict(os.environ, FRESH_ACTIVE="1"))
    sys.stdout.buffer.write(b"".join(l for l in p.stdout.splitlines(keepends=True) if l.rstrip(b"\r\n") != CTRL.encode()))
    sys.stdout.flush()
    if os.environ.get("FRESH_STATS"):
        with open(os.environ["FRESH_STATS"], "a") as f:
            f.write("%d\t%d\n" % (fresh, stmts))
    return p.returncode


if __name__ == "__main__":
    sys.exit(main(sys.argv[1:]))
