#!/usr/bin/env python3
"""Kernel coverage: which slots of the kernel tables (agg/kern.h, col/kern.h, bm25/kernels.h) a SIMD level binds to the scalar function, and is every slot run by the
equality test of its family. A slot that a level leaves scalar needs a line in scripts/kernels.allow (family, level, slot, reason: a measurement or a property of the
operation), a line for a slot that is no longer scalar fails too, so the list is the gap list and cannot go stale.

  scripts/check-kernels.py           exit 1 on a scalar slot without a line, a line without a scalar slot or a reason, a slot no test runs
  scripts/check-kernels.py --list    the matrix (family, level, slot, vector or scalar)
"""
import re, sys, os, collections

R = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
FAMILIES = {
    'col': dict(header='src/col/kern.h', struct='SiftColKernels', test='src/col/kern_test.c',
                tables={'avx2': ('src/col/kern_avx2.c', 'sift_col_kernels_avx2'), 'avx512': ('src/col/kern_avx512.c', 'sift_col_kernels_avx512'),
                        'neon': ('src/col/kern.c', 'sift_col_kernels_neon')}),
    'agg': dict(header='src/agg/kern.h', struct='SiftAggKernels', test='src/agg/kern_cases.h',
                tables={'avx2': ('src/agg/kern_avx2.c', 'sift_aggkern_avx2'), 'avx512': ('src/agg/kern_avx512.c', 'sift_aggkern_avx512'),
                        'neon': ('src/agg/kern_neon.c', 'sift_aggkern_neon')}),
    'bm25': dict(header='src/bm25/kernels.h', struct='SiftKernels', test='src/bm25/kernels_test.c',
                 tables={'avx2': ('src/bm25/kernels_avx2.c', 'sift_kernels_avx2'), 'avx512': ('src/bm25/kernels_avx512.c', 'sift_kernels_avx512'),
                         'neon': ('src/bm25/kernels_neon.c', 'sift_kernels_neon')}),
    'shred': dict(header='src/col/shred.h', struct='SiftShredKernels', test='src/col/shred_test.c',
                  tables={'avx2': ('src/col/shred_kern_avx2.c', 'sift_shred_kernels_avx2'), 'avx512': ('src/col/shred_kern_avx512.c', 'sift_shred_kernels_avx512'),
                          'neon': ('src/col/shred_kern.c', 'sift_shred_kernels_neon')}),
}


def read(p):
    return open(os.path.join(R, p)).read()


def members(fam):
    src = read(fam['header'])
    m = re.search(r'typedef struct[^{]*\{(.*?)\}\s*' + fam['struct'] + r'\s*;', src, re.S)
    out = []
    for line in m.group(1).split('\n'):
        d = re.match(r'\s*[\w\s]+\(\*\s*(\w+)\s*(?:\[\s*\w+\s*\])?\)', line)
        if d:
            out.append(d.group(1))
    return out


def block(src, name):
    """the text of `const T name = ... ;` (braces balanced; a macro call on the right is expanded by the caller)"""
    m = re.search(r'\b' + name + r'\s*=\s*', src)
    if not m:
        return None
    i, depth = m.end(), 0
    j = i
    while j < len(src):
        c = src[j]
        if c in '{(':
            depth += 1
        elif c in '})':
            depth -= 1
        elif c == ';' and depth == 0:
            break
        j += 1
    return src[i:j]


def expand_macro(src, text):
    """AVX2_TABLE(a, b): the body of the #define with its parameters replaced"""
    m = re.match(r'(\w+)\(([^)]*)\)\s*$', text.strip())
    if not m:
        return text
    d = re.search(r'#define\s+' + m.group(1) + r'\(([^)]*)\)\s*((?:[^\n]*\\\n)*[^\n]*)', src)
    if not d:
        return text
    body = d.group(2).replace('\\\n', ' ')
    for p, a in zip([x.strip() for x in d.group(1).split(',')], [x.strip() for x in m.group(2).split(',')]):
        body = re.sub(r'\b' + p + r'\b', a, body)
    return body


def level_status(fam, level, mem):
    f, name = fam['tables'][level]
    src = read(f)
    text = expand_macro(src, block(src, name))
    st = {}
    for slot, val in re.findall(r'\.(\w+)\s*=\s*(\{[^}]*\}|[^,}\n]+)', text):
        parts = [v.strip() for v in val.strip('{} ').split(',')] if val.startswith('{') else [val.strip()]
        st[slot] = ['scalar' if 'scalar' in v else 'vector' for v in parts]
    for slot in re.findall(r'AGK_INIT\d?\((\w+)\)', text):
        st[slot] = ['vector']
    if 'AGK_SLOTS(AGK_INIT_NEON)' in text:
        for slot in mem:
            a = re.search(r'#define\s+agk_' + slot + r'_neon\s+(\S+)', src)
            st[slot] = ['scalar' if a and 'scalar' in a.group(1) else 'vector']
    return {slot: st.get(slot, ['scalar']) for slot in mem}


def matrix():
    out = []
    for fn, fam in FAMILIES.items():
        mem = members(fam)
        test = read(fam['test'])
        for level in fam['tables']:
            for slot, parts in level_status(fam, level, mem).items():
                for k, s in enumerate(parts):
                    out.append((fn, level, slot + ('[%d]' % k if len(parts) > 1 else ''), s, bool(re.search(r'\b' + slot + r'\b', test))))
    return out


def main():
    rows = matrix()
    if '--list' in sys.argv:
        for fn, level, slot, s, t in rows:
            print(fn, level, slot, s, '' if t else 'UNTESTED')
        return 0
    allow = collections.OrderedDict()
    for n, l in enumerate(read('scripts/kernels.allow').split('\n'), 1):
        if l.strip() and not l.startswith('#'):
            f = l.split('\t')
            if len(f) < 4 or not f[3].strip():
                print('kernels.allow:%d: a line needs family, level, slot and a reason' % n)
                return 1
            allow[tuple(f[:3])] = f[3]
    bad, seen = [], set()
    for fn, level, slot, s, tested in rows:
        key = (fn, level, re.sub(r'\[\d+\]', '[]', slot))
        if s == 'scalar':
            seen.add(key)
            if key not in allow:
                bad.append('%s %s %s is scalar: vectorize it or add a line with the reason to scripts/kernels.allow' % key)
        if not tested:
            bad.append('%s %s %s: no equality test runs the slot' % (fn, level, slot))
    for key in allow:
        if key not in seen:
            bad.append('kernels.allow: %s %s %s is not scalar (or does not exist): remove the line' % key)
    for b in sorted(set(bad)):
        print(b)
    if not bad:
        print('kernels: ok, %d slots over %d tables, %d scalar with a reason' % (len({(a, c) for a, b, c, d, e in rows}), len(FAMILIES) * 3, len(allow)))
    return 1 if bad else 0


sys.exit(main())
