#!/usr/bin/env bash
# A file that uses a PostgreSQL symbol includes the header that declares it, itself: a header that another one pulled in is not a promise of the server, and PostgreSQL 19 dropped one
# (utils/timestamp.h: GetCurrentTimestamp in col/maint/net.c was an implicit declaration, an error of the hosted PostgreSQL 19 and package jobs, 2026-11-03). The table is scripts/pg-includes.tsv:
# a regular expression of the symbols, TAB, the headers that declare them (any of the list satisfies the rule), one row per family; a symbol a compile against a newer server found missing is a row.
# A header another one pulled in can also differ by flavour (the files compiled only with SIFT_TESTING or SIFT_VULKAN are empty in the release one): where the server's headers are installed,
# each .c file is also compiled without code generation (-fsyntax-only, 4 at a time, warnings kept out of the answer, an implicit declaration an error as in the Makefile) in the release
# flavour and, when it names the macro or includes a file of src/ that does, with -DSIFT_TESTING and with -DSIFT_VULKAN. The Vulkan declarations are in the tree: it needs no SDK header.
#   scripts/check-pg-includes.sh            the files of src/ (a header counts for itself, not for the files that include it), then the compile of the flavours
#   scripts/check-pg-includes.sh --selftest
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -I - "$here" "${1:-}" <<'PY'
import os, re, shutil, subprocess, sys, tempfile
from concurrent.futures import ThreadPoolExecutor

root, arg = sys.argv[1], sys.argv[2]


def rows(path):
    out = []
    for n, l in enumerate(open(path), 1):
        if l.strip() and not l.startswith('#'):
            sym, _, hdrs = l.rstrip('\n').partition('\t')
            if not hdrs:
                sys.exit(f'{path}:{n}: a row is SYMBOLS TAB HEADERS')
            out.append((re.compile(r'\b(?:' + sym + r')\b'), hdrs.split()))
    return out


def bad(files, table):
    hits = []
    for f, text in files:
        code = re.sub(r'/\*.*?\*/|//[^\n]*', '', text, flags=re.S)
        have = set(re.findall(r'#\s*include\s+[<"]([^>"]+)[>"]', text))
        for rx, hdrs in table:
            m = rx.search(code)
            if m and not have & set(hdrs):
                hits.append((f, m.group(0), hdrs[0]))
    return hits


FLAVOURS = (('release', None), ('testing', 'SIFT_TESTING'), ('vulkan', 'SIFT_VULKAN'))


def users(files, macro):
    """The .c files that see the macro: those that name it and those that include, by the path from src/, a file that sees it."""
    by = {}
    for f, text in files:
        for i in re.findall(r'#\s*include\s+"([^"]+)"', text):
            by.setdefault('src/' + i, []).append(f)
    todo = [f for f, text in files if re.search(r'\b%s\b' % macro, text)]
    seen = set(todo)
    while todo:
        for u in by.get(todo.pop(), []):
            if u not in seen:
                seen.add(u)
                todo.append(u)
    return sorted(f for f in seen if f.endswith('.c'))


def jobs(files):
    """(file, flavour, defines) of every compile: the release flavour of all files, the others of the files that see their macro."""
    return [(f, name, [] if macro is None else ['-D' + macro])
            for name, macro in FLAVOURS for f in (users(files, macro) if macro else [f for f, _ in files if f.endswith('.c')])]


def server_flags():
    """The compiler flags of the server's headers, or None where they are not installed (the lint job has no PostgreSQL)."""
    if not shutil.which('pg_config'):
        return None
    pg = lambda *a: subprocess.run(['pg_config', *a], capture_output=True, text=True, check=True).stdout.split()
    inc = pg('--includedir-server')[0]
    return [*pg('--cppflags'), '-I' + inc] if os.path.exists(os.path.join(inc, 'postgres.h')) else None


def compile_failures(root, jobs, flags):
    """(file, flavour, first error line) of every source that does not compile in a flavour."""
    with tempfile.TemporaryDirectory() as gen:
        os.mkdir(os.path.join(gen, 'core'))  # the Makefile writes core/build_id.h; a fresh checkout has none
        open(os.path.join(gen, 'core', 'build_id.h'), 'w').write('#define SIFT_BUILD_ID "check"\n')
        cmd = [os.environ.get('CC', 'cc'), '-fsyntax-only', '-Werror=implicit-function-declaration', '-D_GNU_SOURCE',
               '-I' + os.path.join(root, 'src'), '-I' + gen, *flags]

        def run(job):
            f, name, defs = job
            r = subprocess.run([*cmd, *defs, f], cwd=root, capture_output=True, text=True)
            return None if r.returncode == 0 else (f, name, next((l for l in r.stderr.splitlines() if 'error' in l), r.stderr.strip()))

        with ThreadPoolExecutor(4) as pool:
            return [r for r in pool.map(run, jobs) if r]


if arg == '--selftest':
    with tempfile.NamedTemporaryFile('w', suffix='.tsv') as t:
        t.write('GetCurrentTimestamp|TimestampTz\tutils/timestamp.h datatype/timestamp.h\n')
        t.flush()
        table = rows(t.name)
    use = '#include "postgres.h"\nvoid f(void) { TimestampTz t = GetCurrentTimestamp(); }\n'
    assert bad([('a.c', use)], table) == [('a.c', 'TimestampTz', 'utils/timestamp.h')], bad([('a.c', use)], table)
    assert not bad([('a.c', '#include "utils/timestamp.h"\n' + use)], table)
    assert not bad([('a.c', '#include "datatype/timestamp.h"\n' + use)], table)
    assert not bad([('a.c', '/* GetCurrentTimestamp */ int x; // TimestampTz\n')], table)
    flags = server_flags()
    if flags is not None:
        with tempfile.TemporaryDirectory() as d:
            os.mkdir(os.path.join(d, 'src'))
            for name, text in (('a.c', '#include "postgres.h"\nint a(void) { return 1; }\n'),
                               ('b.c', '#include "postgres.h"\n#ifdef SIFT_TESTING\nint b(void) { return no_such_function(); }\n#endif\n')):
                open(os.path.join(d, 'src', name), 'w').write(text)
            files = [(f, open(os.path.join(d, f)).read()) for f in ('src/a.c', 'src/b.c')]
            got = [(f, n) for f, n, _ in compile_failures(d, jobs(files), flags)]
            assert got == [('src/b.c', 'testing')], got
    print('check-pg-includes selftest: ok')
    sys.exit(0)

table = rows(os.path.join(root, 'scripts', 'pg-includes.tsv'))
names = subprocess.run(['git', '-C', root, 'ls-files', 'src/*.c', 'src/*.h'], capture_output=True, text=True, check=True).stdout.split()
files = [(f, open(os.path.join(root, f), errors='replace').read()) for f in names]
hits = bad(files, table)
for f, sym, h in hits:
    print(f'{f}: uses {sym} and does not include "{h}"')
print(f'check-pg-includes: {len(hits)} file(s) without the header of a symbol they use, {len(files)} files, {len(table)} rows')
flags = server_flags()
if flags is None:
    print('check-pg-includes: the flavours were not compiled: no PostgreSQL server headers (pg_config --includedir-server)')
else:
    todo = jobs(files)
    failed = compile_failures(root, todo, flags)
    for f, name, err in failed:
        print(f'{f}: does not compile in the {name} flavour: {err}')
    print(f'check-pg-includes: {len(todo)} compiles (release, SIFT_TESTING, SIFT_VULKAN), {len(failed)} failure(s)')
    hits += failed
sys.exit(1 if hits else 0)
PY
