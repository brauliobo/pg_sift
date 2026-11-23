#!/usr/bin/env python3
"""A call that fills a local through a nested call and reads the same local in a sibling argument: f(g(&n), n). C leaves the order of evaluation of the arguments unspecified, so the
read may see the local before g stored it (an uninitialized count, a stale pointer), and the result changes with the compiler, the optimization level and the stack. The fix is
the root cause, not a reorder that happens to work: call g first, keep its result in a local, and pass the local.
  scripts/check-arg-order.py          check every src/*.c and src/*.h of the tree
  scripts/check-arg-order.py --selftest"""
import re, subprocess, sys

SKIP_CALLS = {'if', 'while', 'for', 'switch', 'sizeof', 'return', 'offsetof', 'typeof', 'alignof'}
OUT = re.compile(r'(?<![&\w)\]])&\s*([A-Za-z_]\w*)\b(?!\s*[.\[]|\s*->)')


def strip(src):
    src = re.sub(r'/\*.*?\*/', lambda m: re.sub(r'[^\n]', ' ', m.group(0)), src, flags=re.S)
    src = re.sub(r'//[^\n]*', '', src)
    src = re.sub(r"'(\\.|[^'\\])'", "'x'", src)
    return re.sub(r'"(\\.|[^"\\])*"', '""', src)


def split_args(body):
    args, depth, cur = [], 0, ''
    for ch in body:
        depth += ch in '([{'
        depth -= ch in ')]}'
        if ch == ',' and depth == 0:
            args.append(cur)
            cur = ''
        else:
            cur += ch
    return args + [cur]


def calls(src):
    stack = []
    for i, ch in enumerate(src):
        if ch == '(':
            stack.append(i)
        elif ch == ')' and stack:
            o = stack.pop()
            m = re.search(r'([A-Za-z_]\w*)\s*$', src[max(0, o - 80):o])
            if m and not m.group(1).isupper() and m.group(1) not in SKIP_CALLS:
                yield m.group(1), o, src[o + 1:i]


def filled_by_nested_call(arg, m):
    depth = 0
    for k in range(m.start() - 1, -1, -1):
        depth += arg[k] == ')'
        if arg[k] == '(':
            if depth == 0:
                return re.search(r'[A-Za-z_]\w*\s*$', arg[:k]) is not None
            depth -= 1
    return False


def reads(arg, name):
    arg = re.sub(r'sizeof\s*\([^()]*\)', '', arg)
    arg = re.sub(r'&\s*' + name + r'\b', '', arg)
    return re.search(r'(?<![\w.>&])' + name + r'\b(?!\s*[.\[(]|\s*->)', arg) is not None


def findings(src):
    src = strip(src)
    for name, o, body in calls(src):
        args = split_args(body)
        for i, a in enumerate(args):
            for m in OUT.finditer(a):
                if filled_by_nested_call(a, m) and any(reads(b, m.group(1)) for j, b in enumerate(args) if j != i):
                    yield src[:o].count('\n') + 1, name, m.group(1)


def selftest():
    bad = ['void f(void) { int n; use(read(&n), n); }', 'void f(void) { int n; use(n, read(x, &n)); }', 'void f(void) { use(a(b(&n)), 1, n + 1); }']
    good = ['void f(void) { int n; p = read(&n); use(p, n); }', 'void f(void) { use(read(&n), sizeof(n)); }', 'void f(void) { use(read(&d), d.len); }',
            'void f(void) { use((char *) &a, a); }', 'void f(void) { CHECK(read(&n), n); }', 'void f(void) { use(read(&n), &n); }', 'void f(void) { use(ok && n, n); }']
    for s in bad:
        assert list(findings(s)), s
    for s in good:
        assert not list(findings(s)), s
    print('check-arg-order: selftest ok')


def main():
    if '--selftest' in sys.argv:
        return selftest()
    files = subprocess.run(['git', 'ls-files', 'src/*.c', 'src/*.h'], capture_output=True, text=True, check=True).stdout.split()
    bad = [(f, line, call, var) for f in files for line, call, var in findings(open(f, errors='replace').read())]
    for f, line, call, var in bad:
        print('%s:%d: %s(): %s is filled by a nested call and read by a sibling argument (unspecified order)' % (f, line, call, var))
    if bad:
        sys.exit('check-arg-order: call the filling function first, keep its result in a local and pass the local; the order of the arguments is not the fix')
    print('check-arg-order: ok (%d files)' % len(files))


main()
