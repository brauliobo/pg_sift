# The options as the C sources define them: the DefineCustom*Variable() and add_*_reloption() calls, read into one row per option.
# Also the imports and constants every later part reads. Run into the namespace of main.py (exec), not imported; not a script of its own.
import csv
import functools
import glob
import json
import math
import os
import re
import subprocess
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, 'scripts')
from genlib import call_end, esc, norm, split_args, strip_comments, testing_lines

mode = sys.argv[1]
ROOT = os.getcwd()
CONST = {'INT_MAX': 2147483647, 'INT_MIN': -2147483648, 'MAX_KILOBYTES': 2147483647, 'true': 1, 'false': 0}
KIND_AM = {'sift_relopt_kind': 'sift', 'col_relopt_kind': 'sift_col', 'stub_relopt_kind': None}     # None: the test access method
APPLIES = ['all', 'BM25', 'sift_col', 'sift_table']
CTX = {'PGC_USERSET': 'session', 'PGC_SUSET': 'superuser', 'PGC_SIGHUP': 'reload', 'PGC_POSTMASTER': 'restart'}
ARGS = {'Bool': ['name', 'short', 'long', 'var', 'default', 'ctx', 'flags'],
        'Int': ['name', 'short', 'long', 'var', 'default', 'lo', 'hi', 'ctx', 'flags'],
        'Real': ['name', 'short', 'long', 'var', 'default', 'lo', 'hi', 'ctx', 'flags'],
        'String': ['name', 'short', 'long', 'var', 'default', 'ctx', 'flags'],
        'Enum': ['name', 'short', 'long', 'var', 'default', 'members', 'ctx', 'flags']}
RELARGS = {'int': ['kind', 'name', 'short', 'default', 'lo', 'hi', 'lock'], 'real': ['kind', 'name', 'short', 'default', 'lo', 'hi', 'lock'],
           'bool': ['kind', 'name', 'short', 'default', 'lock'], 'string': ['kind', 'name', 'short', 'default', 'check', 'lock'],
           'enum': ['kind', 'name', 'short', 'members', 'default', 'detail', 'lock']}


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8') as f:
        return f.read()


def string(a):
    if a == 'NULL':
        return ''
    return ''.join(re.sub(r'\\(.)', r'\1', s) for s in re.findall(r'"((?:\\.|[^"\\])*)"', a))


# ---- the sources ----
def collect(srcs):
    """srcs: {path: text} -> (options, errors). An option: dict(key, kind, am, name, type, default, lo, hi, unit, ctx, short, long, members, testing, where)"""
    errors, defs, texts = [], {}, {}
    for path, raw in sorted(srcs.items()):
        texts[path] = strip_comments(raw)
        for m in re.finditer(r'^[ \t]*#[ \t]*define[ \t]+(\w+)[ \t]+(.*(?:\\\n.*)*)$', texts[path], re.M):
            defs.setdefault(m.group(1), []).append((path, m.group(2).replace('\\\n', ' ').strip()))

    def ev(expr, path, depth=0):
        e = re.sub(r'\bUINT64CONST\s*\(', '(', expr)
        e = re.sub(r'\(\s*(?:Size|uint32|uint64|int64|int|unsigned|long|double)\s*\)', '', e)
        e = re.sub(r'\b(0[xX][0-9a-fA-F]+|\d+)[uUlL]+\b', r'\1', e)
        if '.' not in e:
            e = e.replace('/', '//')

        def sub(m):
            name = m.group(0)
            if name in CONST:
                return str(CONST[name])
            pick = [d for d in defs.get(name, []) if d[0] == path] or defs.get(name, [])
            if depth > 10 or not pick or len({d[1] for d in pick}) != 1:
                raise ValueError('cannot evaluate %s (add it to CONST of scripts/gen-options.sh)' % name)
            return '(%s)' % ev(pick[0][1], pick[0][0], depth + 1)
        return eval(re.sub(r'\b[A-Za-z_]\w*\b', sub, e), {'__builtins__': {}}, {})

    def enum_labels(var, path):
        """the labels of a config_enum_entry or relopt_enum_elt_def array: [(label, constant text, hidden)]"""
        m = re.search(r'\b%s\[\]\s*=\s*\{(.*?)\n\s*\};' % re.escape(var), texts[path], re.S)
        return [(g[0], g[1].strip(), g[2].strip() == 'true') for g in re.findall(r'\{\s*"([^"]*)"\s*,\s*([^,}]+?)\s*(?:,\s*(\w+)\s*)?\}', m.group(1))] if m else None

    opts = []
    for path, text in texts.items():
        tl = testing_lines(text)
        calls = [(m.start(), m.end() - 1, 'guc', m.group(1)) for m in re.finditer(r'\bDefineCustom(Bool|Int|Real|String|Enum)Variable\(', text)]
        calls += [(m.start(), m.end() - 1, 'guc', 'Enum') for m in re.finditer(r'\bsift_host_enum_option\(', text) if not text[:m.start()].rstrip().endswith('define')]
        calls += [(m.start(), m.end() - 1, 'rel', m.group(1)) for m in re.finditer(r'\badd_(int|real|bool|string|enum)_reloption\(', text)]
        for start, op, what, t in calls:
            line = text.count('\n', 0, start) + 1
            where = '%s:%d' % (path, line)
            a = split_args(text[op + 1:call_end(text, op)])
            if what == 'guc' and not a[0].startswith('"'):      # the body of the sift_host_enum_option macro
                continue
            try:
                if what == 'guc' and text.startswith('sift_host_enum_option', start):
                    a = dict(zip(['name', 'short', 'long', 'var', 'default', 'members'], a))      # sift_host_enum_option(name, short, long, var, boot, options, assign)
                    a['ctx'], a['flags'] = 'PGC_USERSET', ''
                elif what == 'guc':
                    a = dict(zip(ARGS[t], a))
                else:
                    a = dict(zip(RELARGS[t], a))
                o = dict(kind=what, type=t.lower(), where=where, testing=line in tl or string(a['name']).startswith('sift.test_'), name=string(a['name']), short=norm(string(a['short'])),
                         long=norm(string(a.get('long', 'NULL'))), unit='', ctx=CTX.get(a.get('ctx', ''), ''), lo=None, hi=None, am=None, members=None)
                if what == 'rel':
                    if a['kind'] not in KIND_AM:
                        errors.append('%s: add_%s_reloption(%s, ...): the reloption kind is none of %s (add it to KIND_AM of scripts/gen-options.sh)' % (where, t, a['kind'], ', '.join(KIND_AM)))
                        continue
                    o['am'] = KIND_AM[a['kind']]
                    if o['am'] is None:
                        continue
                    o['key'] = '%s(%s)' % (o['am'], o['name'])
                else:
                    o['key'] = o['name']
                    um = re.search(r'GUC_UNIT_(\w+)', a['flags'])
                    o['unit'] = {'KB': 'kB', 'MB': 'MB', 'MS': 'ms'}.get(um.group(1), um.group(1).lower()) if um else ''
                if o['type'] == 'enum':
                    labels = enum_labels(a['members'], path)
                    if labels is None:
                        errors.append('%s: the labels %s of %s are not an array of this file' % (where, a['members'], o['name']))
                        continue
                    o['members'] = [l for l, _, hidden in labels if not hidden]
                    o['default'] = next((l for l, c, _ in labels if c == a['default']), None)
                    if o['default'] is None:
                        errors.append('%s: the default %s of %s is none of the labels' % (where, a['default'], o['name']))
                elif o['type'] == 'string':
                    o['default'] = string(a['default'])
                elif o['type'] == 'bool':
                    o['default'] = bool(ev(a['default'], path))
                else:
                    o['default'] = ev(a['default'], path)
                    o['lo'], o['hi'] = ev(a['lo'], path), ev(a['hi'], path)
                opts.append(o)
            except (ValueError, KeyError, TypeError, SyntaxError, NameError) as ex:
                errors.append('%s: %s' % (where, ex))
    for p, text in texts.items():       # the table of build_reloptions() of every access method names the options it parses
        for m in re.finditer(r'relopt_parse_elt\s+\w*tab\[\]\s*=\s*\{(.*?)\};', text, re.S):
            am = {'col/am/options.c': 'sift_col', 'core/handler.c': 'sift'}.get(p)
            parsed = set(re.findall(r'\{\s*"(\w+)"\s*,\s*RELOPT_TYPE', m.group(1)))
            for n in sorted(parsed ^ {o['name'] for o in opts if o['kind'] == 'rel' and o['am'] == am}) if am else ():
                errors.append('src/%s: the reloption %s of %s is %s' % (p, n, am, 'parsed but not registered' if n in parsed else 'registered but not parsed (relopt_parse_elt)'))
    seen = set()
    for o in opts:
        if o['key'] in seen:
            errors.append('%s: %s is defined twice' % (o['where'], o['key']))
        seen.add(o['key'])
    return opts, errors
