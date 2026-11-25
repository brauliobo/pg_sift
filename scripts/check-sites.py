#!/usr/bin/env python3
"""The catalog and relcache calls of src/ that a SQL call can reach (a PG_FUNCTION_ARGS function that takes an OID, or its callees): relation_open, index_open, table_open,
get_rel_name, get_attnum, get_attname, get_namespace_name, lookup_type_cache, SearchSysCache* and the like, with the origin of the OID and whether the result is tested.
A call whose result is not tested (N) is verified by reading: scripts/sites.verified has a row FILE TAB FUNCTION TAB VERDICT TAB REASON for it (VERDICT: guarded, fixed, unreachable;
the reason says where, or why). A call with no row, or a row with no call any more, fails (scripts/check-null.sh runs this).
  scripts/check-sites.py ROOT            fail on unverified sites and stale rows
  scripts/check-sites.py ROOT --table    the whole table (file, line, function, api, argument, origin, handled, reach)"""
import os, re, subprocess, sys

root = sys.argv[1]
TABLE = '--table' in sys.argv
API = (r'relation_open|try_relation_open|index_open|try_index_open|table_open|try_table_open|relation_openrv\w*|table_openrv\w*|RelationIdGetRelation|'
       r'SearchSysCache[0-9A-Za-z]*|GetSysCache\w*|get_rel_name|get_namespace_name|get_attname|get_attnum|get_atttype|get_rel_relkind|get_rel_namespace|'
       r'get_func_name|get_opname|lookup_type_cache|get_opfamily_member|get_opfamily_proc|get_opclass_\w+|get_index_column_opclass|get_relname_relid|'
       r'get_typ_typrelid|get_rel_type_id|get_rel_tablespace')
CALL = re.compile(r'\b(' + API + r')\(')
OPEN = {'relation_open', 'index_open', 'table_open'}
TRY = {'try_relation_open', 'try_index_open', 'try_table_open', 'RelationIdGetRelation'}
FUNC = re.compile(r'^([A-Za-z_]\w*)\(')
TYPE_LINE = re.compile(r'^[A-Za-z_][\w \t\*]*$')
GUARDS = ('sift_rel_must_exist', 'sift_relation_get', 'sift_rel_name', 'sift_func_name', 'sift_namespace_name', 'sift_inspect_expect')


def clean(lines):
    res, inside = [], False
    for l in lines:
        if not inside and '/' not in l:
            res.append(l)
            continue
        if inside:
            if '*/' not in l:
                res.append('')
                continue
            l, inside = l.split('*/', 1)[1], False
        l = re.sub(r'/\*.*?\*/', '', l)
        if '/*' in l:
            l, inside = l.split('/*', 1)[0], True
        res.append(re.sub(r'//.*', '', l))
    return res


def functions(lines):
    out, cur = [], None
    for i, l in enumerate(lines):
        if cur is None:
            m = FUNC.match(l)
            if m and i > 0 and TYPE_LINE.match(lines[i - 1]) and not lines[i - 1].startswith(('#', 'typedef', 'else', 'return', 'goto')):
                cur = (m.group(1), i)
        elif l.startswith('}'):
            out.append((cur[0], cur[1], i))
            cur = None
    return out


def first_arg(line, pos):
    depth, i, start = 0, pos, pos
    while i < len(line):
        c = line[i]
        if c == '(':
            depth += 1
        elif c == ')':
            if depth == 0:
                return line[start:i].strip()
            depth -= 1
        elif c == ',' and depth == 0:
            return line[start:i].strip()
        i += 1
    return line[start:].strip()


def compared(v, body):
    v = r'(?:[\w\]\[]+(?:->|\.))*' + re.escape(v)
    tests = [r'%s\s*[!=]=\s*(NULL|NIL|0|InvalidOid)\b' % v, r'\b(NULL|NIL)\s*[!=]=\s*%s\b' % v, r'!\s*%s\b' % v, r'%s\s*\?' % v,
             r'(HeapTupleIsValid|RelationIsValid|OidIsValid)\(\s*%s\s*\)' % v, r'\bif \(\s*%s\s*\)' % v, r'&&\s*%s\b(?![\w\s]*[-.\[=(])' % v,
             r'\|\|\s*%s\b(?![\w\s]*[-.\[=(])' % v, r'%s\s*(&&|\|\|)' % v, r'\(\s*%s\s*\)\s*(&&|\|\||\?)' % v, r'\bwhile \(\s*%s\s*\)' % v]
    return any(re.search(t, l) for l in body if not re.match(r'\s*(Assert\w*|Trap)\(', l) for t in tests)


def origin(arg, body, k, params, entry_oid):
    a = arg
    m = re.match(r'(?:ObjectIdGetDatum|PointerGetDatum|Int32GetDatum|CStringGetDatum)\((.*)\)$', a)
    if m:
        a = m.group(1).strip()
    if 'PG_GETARG' in a:
        return 'sql-arg'
    if re.match(r'^(RelationGetRelid|IndexRelationGetRelid)\(', a) or re.search(r'->rd_id\b|->rd_index->indexrelid|->rd_rel->reltype', a):
        return 'internal'
    for pre in (r'(?:rte|RangeTblEntry\w*)->relid', r'->relid\b', r'->indexoid\b', r'->indexrelid\b', r'->indrelid\b', r'->conrelid\b', r'->attrelid\b',
                r'->rd_rel->relam\b', r'->relam\b', r'->relnamespace\b', r'->opfamily\b', r'->funcid\b', r'->opno\b', r'->vartype\b', r'->consttype\b'):
        if re.search(pre, a):
            return 'planner/catalog'
    ident = re.match(r'^[A-Za-z_]\w*$', a)
    if ident:
        v = a
        for l in reversed(body[:k]):
            m = re.search(r'\b' + v + r'\s*=\s*([^;]*)', l)
            if m and not re.search(r'[=!<>]=', l[m.start():m.start() + len(v) + 3]):
                rhs = m.group(1)
                if 'PG_GETARG' in rhs:
                    return 'sql-arg'
                sub = origin(rhs.strip().rstrip(','), [], 0, params, entry_oid) if rhs.strip() != a else 'param'
                if sub != 'param':
                    return sub
                break
        if v in params:
            return 'param:' + v
        return 'local'
    if re.search(r'Anum_|ObjectIdAttributeNumber|fcinfo|typid|->atttypid|->typid|\.atttypid|exprType|F_', a):
        return 'planner/catalog'
    if re.search(r'\b(namespace|nsp|relid|reloid|oid)\w*\b', a):
        return 'local'
    return 'local'


def main():
    names = subprocess.run(['git', '-C', root, 'ls-files', 'src'], capture_output=True, text=True, check=True).stdout.split()
    names = [f for f in names if f.endswith(('.c', '.h')) and not f.endswith(('_test.c', '/test.c')) and '/test/' not in f]
    funcs = {}
    for f in names:
        text = open(os.path.join(root, f), errors='replace').read()
        raw = text.split('\n')
        cl = clean(raw)
        for name, a, b in functions(raw):
            sig = ' '.join(raw[a - 1:a + 4]).split(')')[0]
            body = cl[a:b + 1]
            funcs.setdefault(name, []).append((f, a, b, raw, cl, body, sig))
    entries = {n for n, v in funcs.items() if any('PG_FUNCTION_ARGS' in x[6] for x in v)}
    entry_oid = {n for n in entries if any(any('PG_GETARG_OID' in l or 'PG_GETARG_NAME' in l for l in x[5]) for x in funcs[n])}
    callers = {}
    for n, vs in funcs.items():
        for x in vs:
            body = ' '.join(x[5])
            for c in set(re.findall(r'\b([A-Za-z_]\w*)\s*\(', body)):
                if c in funcs and c != n:
                    callers.setdefault(c, set()).add(n)
    reach_cache = {}

    def reach(fn):
        if fn in reach_cache:
            return reach_cache[fn]
        seen, todo = set(), [fn]
        while todo:
            x = todo.pop()
            if x in seen:
                continue
            seen.add(x)
            todo += list(callers.get(x, ()))
        reach_cache[fn] = (bool(seen & entries), bool(seen & entry_oid))
        return reach_cache[fn]

    rows = []
    for name, vs in funcs.items():
        for (f, a, b, raw, cl, body, sig) in vs:
            params = set(re.findall(r'\b([A-Za-z_]\w*)\s*(?:,|$|\[)', sig.split('(', 1)[-1]))
            for k, l in enumerate(body):
                for m in CALL.finditer(l):
                    api = m.group(1)
                    arg = first_arg(l, m.end())
                    org = origin(arg, body, k, params, entry_oid)
                    sql, sqloid = reach(name)
                    line = a + k + 1
                    if api in OPEN:
                        guarded = any(g in l for g in GUARDS) or any(g in x for x in body[max(0, k - 12):k] for g in GUARDS)
                        handled = 'Y' if guarded else 'N'
                        why = 'open raises XX000 for a gone oid'
                    else:
                        mv = re.search(r'(?:\*\s*)?\b([A-Za-z_]\w*)\s*=\s*(?:\([A-Za-z_][\w \*]*\)\s*)?' + re.escape(api) + r'\(', l)
                        inline = re.search(re.escape(api) + r'\([^;]*\)\s*(\?|[!=]=\s*NULL|\|\||&&)', l) or re.search(r'(if|while|!)\s*\(\s*!?\s*' + re.escape(api) + r'\(', l)
                        msg = re.search(r'\b(errmsg\w*|errdetail\w*|errhint\w*|elog|appendStringInfo\w*|psprintf)\(', l) or \
                            (k > 0 and re.search(r'\b(errmsg\w*|errdetail\w*|errhint\w*|elog)\(', body[k - 1]) and not l.rstrip().endswith(';'))
                        if api.startswith('SearchSysCacheExists') or api == 'SearchSysCacheExists1':
                            handled = 'Y'
                        elif mv:
                            handled = 'Y' if compared(mv.group(1), body) else 'N'
                        elif inline:
                            handled = 'Y'
                        elif msg and api.startswith('get_'):
                            handled = 'msg'
                        elif api in ('get_attnum', 'get_rel_relkind', 'get_rel_namespace', 'get_relname_relid', 'get_typ_typrelid', 'get_rel_type_id',
                                     'get_rel_tablespace', 'get_atttype', 'get_opfamily_member', 'get_opfamily_proc', 'get_opclass_family', 'get_opclass_input_type'):
                            handled = 'invalid-oid'
                        else:
                            handled = 'N'
                        why = ''
                    rows.append((f, line, name, api, arg[:60], org, handled, 'SQL' if sqloid else ('entry' if sql else 'internal')))
    rows.sort(key=lambda r: (r[0], r[1]))
    if TABLE:
        print('file\tline\tfunction\tapi\targ\torigin\tnull_or_invalid_handled\tsql_reach')
        for r in rows:
            print('\t'.join(str(x) for x in r))
        return
    verified = {}
    path = os.path.join(root, 'scripts', 'sites.verified')
    for l in open(path):
        if l.strip() and not l.startswith('#'):
            f = l.rstrip('\n').split('\t')
            if len(f) != 4 or f[2] not in ('guarded', 'fixed', 'unreachable') or not f[3].strip():
                sys.exit('%s: a row is FILE TAB FUNCTION TAB guarded|fixed|unreachable TAB REASON: %s' % (path, l.strip()))
            verified[(f[0], f[1])] = f
    need = {}
    for r in rows:
        if r[6] == 'N' and r[7] in ('SQL', 'entry') and '/test/' not in r[0] and not r[0].endswith(('_test.c', '/test.c')):
            need.setdefault((r[0], r[2]), []).append(r)
    bad = [k for k in need if k not in verified]
    stale = [k for k in verified if k not in need]
    for k in bad:
        r = need[k][0]
        print('%s:%d: %s: %s(%s) can be reached by SQL and its result is not tested: make it fail cleanly (sift_rel_must_exist, core/catname.h), or read it and add a row to scripts/sites.verified' % (r[0], r[1], k[1], r[3], r[4]))
    for k in stale:
        print('scripts/sites.verified: %s %s has no such site any more: delete the row' % k)
    print('check-sites: %d unverified site function(s), %d stale row(s), %d verified' % (len(bad), len(stale), len(verified)))
    sys.exit(1 if bad or stale else 0)


main()
