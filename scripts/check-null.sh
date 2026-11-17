#!/usr/bin/env bash
# A result that can be NULL is tested before it is used (the crash of 5.0.0: the row estimate of `simple_rel_array[x]` read for the relid of an outer join, which has no RelOptInfo). Per function of
# src/ (the code of a test build, *_test.c, is not read), this fails on:
#   planner-array   `simple_rel_array[..]`, `simple_rte_array[..]`, `find_join_rel(` whose result is not compared with NULL in the function. A relid of a bitmapset (ppi_req_outer,
#                   relids, all_query_rels) names a base relation, an outer join (no RelOptInfo) or a join; a relid of a FROM item names a base relation. Loop over a set: skip what has no entry
#   list-first      linitial/lsecond/llast/list_nth.. of a list that the planner or the parser fills with a variable length (indexlist, pathlist, subroots, rtable, sortClause, custom_private, ..) when the
#                   function never tests that list (list_length, NIL, a condition, a loop over it); list_nth(.., id - 1) is read when the function tests id against 0 or 1
#   null-result     a variable assigned from a call that returns NULL (SearchSysCache*, try_relation_open, RelationIdGetRelation, get_rel_name, get_func_name, get_opname, get_namespace_name,
#                   malloc, strdup, fopen, fgets, getenv, strchr, strstr, ..) that the function never compares with NULL (or HeapTupleIsValid); a name used only as an argument of a message is not a use
#   null-wrapped    such a name call as the argument of strcmp, strlen, pstrdup, strcpy, .. on the same line: the wrapper dereferences NULL
#   sql-oid         relation_open/index_open/table_open/RelationIdGetRelation of an OID that a SQL argument supplies (PG_GETARG_OID, or a variable assigned from one) without sift_rel_must_exist
#                   (core/inspect.h): a dropped regclass or 0 is the error 42P01 of the user, not the XX000 of the open; the code of the test build (*_test.c, src/*/test/) is read for this rule
#                   only: its functions are called by the surface sweep too
# A store to an entry (`simple_rte_array[i] = e`) and find_base_rel (it raises for a relid without an entry) are no reads of a NULL. A test of an Assert does not count: a release build has none. A site that cannot be NULL says why in scripts/null.allow: FILE TAB FUNCTION TAB RULE TAB REASON (one line); a line whose function no
# longer has a hit fails too, so the list stays the list of the sites that were read.
#   sites           scripts/check-sites.py (run here): a catalog or relcache call that SQL can reach and whose result is not tested needs a row in scripts/sites.verified (read: guarded, fixed or unreachable)
#   scripts/check-null.sh            the tracked sources of src/
#   scripts/check-null.sh --selftest
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -I - "$here" "${1:-}" <<'PY'
import os, re, subprocess, sys

root, arg = sys.argv[1], sys.argv[2]
FUNC = re.compile(r'^([A-Za-z_]\w*)\(')
TYPE_LINE = re.compile(r'^[A-Za-z_][\w \t\*]*$')

ARRAYS = re.compile(r'\b(simple_rel_array|simple_rte_array)\[|\b(find_join_rel|find_base_rel_ignore_join)\(')
LIST_ACCESS = re.compile(r'\b(linitial|linitial_node|linitial_oid|linitial_int|lsecond|lsecond_node|lthird|lthird_node|llast|llast_node|llast_int|llast_oid|list_nth|list_second)\(\s*(?:[A-Za-z_]\w*\s*,\s*)?((?:[^(),]|\([^()]*\))*)')
VARIABLE_LISTS = re.compile(r'\b(indexlist|pathlist|partial_pathlist|fromlist|targetList|rtable|groupClause|subroots|subplans|baserestrictinfo|join_info_list|eq_classes|ec_members|sortClause|'
                            r'distinctClause|windowClause|rowMarks|returningList|indexprs|indpred|append_rel_list|functions|colnames|custom_private|custom_exprs|partexprs|attexprs|jointree|'
                            r'partitioned_rels|part_rels|lateral_vars|joininfo|pathkeys|query_pathkeys|group_pathkeys|processed_tlist|indexquals|indexorderbys|indrestrictinfo)\b')
NAME_CALLS = r'(?:get_rel_name|get_func_name|get_opname|get_namespace_name|get_database_name|get_collation_name|get_constraint_name|get_language_name|get_am_name|get_extension_name)'
PRODUCERS = [
    ('syscache', r'\b(?:SearchSysCache[1-4]?|SearchSysCacheCopy[1-4]?|GetSysCacheCopy|SearchSysCacheAttName|SearchSysCacheAttNum)\('),
    ('relation', r'\b(?:try_relation_open|try_index_open|try_table_open|RelationIdGetRelation|relation_openrv_extended|table_openrv_extended)\('),
    ('libc', r'\b(?:malloc|calloc|realloc|strdup|fopen|fdopen|fgets|getenv|dlsym|dlopen|opendir|readdir|strchr|strrchr|strstr|strpbrk|popen)\('),
    ('name', r'\b' + NAME_CALLS + r'\('),
]
ASSIGN = lambda call: re.compile(r'(?:\*\s*)?\b([A-Za-z_]\w*)\s*=\s*(?:\([A-Za-z_][\w \*]*\)\s*)?' + call)
MESSAGE_ARG = re.compile(r'\b(errmsg\w*|errdetail\w*|errhint\w*|errcontext|elog|ereport|psprintf|appendStringInfo\w*|snprintf|agg_refuse\w*|ExplainProperty\w*|SIFT_INVARIANT|sift_\w*_refuse\w*)\(')
WRAPPERS = r'(?:strcmp|strncmp|strlen|pstrdup|strcpy|strcasecmp|pg_strcasecmp|pg_strncasecmp|atoi|atol|strtol|strtoul|strtod|makeString|cstring_to_text|CStringGetTextDatum|namestrcpy|quote_identifier|memcpy|strncpy)'
OPEN_CALL = re.compile(r'\b(?:relation_open|index_open|table_open|RelationIdGetRelation)\(\s*((?:[^(),]|\([^()]*\))*)')
SQL_OID = re.compile(r'PG_GETARG_(?:OID|DATUM)\(')
WRAPPED = re.compile(r'\b' + WRAPPERS + r'\(\s*(?:[^(),;]*,\s*)*(?:' + NAME_CALLS + r'|strchr|strrchr|strstr|getenv)\(')


def clean(lines):
    res, inside = [], False
    for l in lines:
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


def compared(v, body):
    v = r'(?:[\w\]\[]+(?:->|\.))*' + re.escape(v)
    tests = [r'%s\s*[!=]=\s*(NULL|NIL|0|InvalidOid)\b' % v, r'\b(NULL|NIL)\s*[!=]=\s*%s\b' % v, r'!\s*%s\b' % v, r'%s\s*\?' % v, r'(HeapTupleIsValid|RelationIsValid)\(\s*%s\s*\)' % v,
             r'\bif \(\s*%s\s*\)' % v, r'&&\s*%s\b(?![\w\s]*[-.\[=(])' % v, r'\|\|\s*%s\b(?![\w\s]*[-.\[=(])' % v, r'%s\s*(&&|\|\|)' % v,
             r'\(\s*%s\s*\)\s*(&&|\|\||\?)' % v, r'\bwhile \(\s*%s\s*\)' % v, r'\b%s\s*=\s*[^;]*\)\s*[!=]=\s*NULL' % v, r'%s\s*=\s*[^;]*\)\s*(&&|\|\|)' % v]
    return any(re.search(t, l) for l in body if not re.match(r'\s*(Assert\w*|Trap)\(', l) for t in tests)


def only_messages(v, body, own):
    uses = [l for k, l in enumerate(body) if k != own and re.search(r'\b%s\b' % re.escape(v), l)]
    return bool(uses) and all(MESSAGE_ARG.search(l) for l in uses)


def guarded_list(arg, body):
    a = re.escape(arg)
    tests = [r'list_length\(\s*%s' % a, r'%s\s*[!=]=\s*(NIL|NULL)' % a, r'!\s*%s\b' % a, r'\bif \(\s*%s\s*\)' % a, r'\?\s*%s' % a, r'&&\s*%s\b' % a, r'\b%s\s*&&' % a,
             r'foreach\w*\([^;]*%s' % a, r'%s\s*=\s*(list_make|lappend|lcons|list_concat)' % a]
    return any(re.search(t, l) for l in body for t in tests)


def call_args(l, start):
    args, cur, depth = [], '', 0
    for ch in l[l.index('(', start):]:
        if ch == '(':
            depth += 1
            if depth == 1:
                continue
        elif ch == ')':
            depth -= 1
            if depth == 0:
                break
        if ch == ',' and depth == 1:
            args.append(cur.strip())
            cur = ''
        else:
            cur += ch
    return args + [cur.strip()]


def positive_id(m, l, body):
    """list_nth(root->glob->subroots, id - 1) where the function tests id against 0 or 1: a plan id counts from 1, 0 or -1 is a CTE without a plan"""
    args = call_args(l, m.start())
    idx = re.fullmatch(r'([\w>.-]+)\s*-\s*1', args[1]) if m.group(1) == 'list_nth' and len(args) > 1 else None
    return bool(idx) and any(re.search(r'\b%s\b[^;{}]{0,60}?(?:<=?|>=?)\s*[01]\b' % re.escape(idx.group(1)), x) for x in body)


def relation_entry_tested(m, line, body):
    if m.group(1):
        index = re.match(r'[^\]]*', line[m.end():]).group(0)
        text = r'%s\[%s\]' % (m.group(1), re.escape(index))
    else:
        text = r'%s\([^;]*?\)' % m.group(2)
    if any(re.search(text + r'\s*[!=]=\s*NULL|(?<![\w>])!\s*' + text, l) for l in body):
        return True
    assigned = re.search(r'(?:\*\s*)?\b([A-Za-z_]\w*)\s*=\s*[^=;]*' + (r'%s\[' % m.group(1) if m.group(1) else r'%s\(' % m.group(2)), line)
    return bool(assigned) and compared(assigned.group(1), body)


def sql_oid_unchecked(arg, body, upto):
    arg = arg.strip()
    if 'sift_rel_must_exist(' in arg:
        return False
    if SQL_OID.search(arg):
        return True
    if not re.fullmatch(r'[A-Za-z_]\w*', arg):
        return False
    assigned = [l for l in body[:upto] if re.search(r'\b%s\s*=[^=]' % re.escape(arg), l)]
    if not any(SQL_OID.search(l) for l in assigned):
        return False
    return not any(re.search(r'sift_rel_must_exist\(\s*%s\s*\)' % re.escape(arg), l) or re.search(r'\b%s\s*=\s*sift_rel_must_exist\(' % re.escape(arg), l) for l in body[:upto + 1])


SQL_OID_MSG = 'the OID is a SQL argument opened with no sift_rel_must_exist: a dropped regclass or 0 is the user error 42P01, not an XX000 of the open'


def array_store(m, l):
    """`simple_rte_array[i] = e;` writes the entry, it does not read one"""
    if not m.group(0).endswith('['):
        return False
    depth, i = 1, m.end()
    while i < len(l) and depth:
        depth += (l[i] == '[') - (l[i] == ']')
        i += 1
    return re.match(r'\s*=(?!=)', l[i:]) is not None


def hits(path, text):
    test_code = path.endswith('_test.c') or '/test/' in path
    lines = text.split('\n')
    cl = clean(lines)
    found = []
    for name, a, b in functions(lines):
        body = cl[a:b + 1]
        for k, l in enumerate(body):
            n = a + k + 1
            if test_code:
                for m in OPEN_CALL.finditer(l):
                    if sql_oid_unchecked(m.group(1), body, k):
                        found.append((path, n, name, 'sql-oid', SQL_OID_MSG))
                continue
            for m in ARRAYS.finditer(l):
                if not array_store(m, l) and not relation_entry_tested(m, l, body):
                    found.append((path, n, name, 'planner-array', 'a relation entry that no code of the function compares with NULL: an outer join relid has none; skip it as the neighbouring loops do'))
            for m in LIST_ACCESS.finditer(l):
                arg = m.group(2).strip()
                if VARIABLE_LISTS.search(arg) and not guarded_list(arg, body) and not positive_id(m, l, body):
                    found.append((path, n, name, 'list-first', '%s of %s with no test of the list in the function (list_length or NIL)' % (m.group(1), arg)))
            for rule, call in PRODUCERS:
                for m in ASSIGN(call).finditer(l):
                    v = m.group(1)
                    if (rule == 'name' and only_messages(v, body, k)) or re.search(call + r'[^;]*\)\s*(\?|[!=]=\s*NULL)', l):
                        continue
                    if not compared(v, body):
                        found.append((path, n, name, 'null-result', '%s is assigned a result that can be NULL and the function never tests it' % v))
            for m in OPEN_CALL.finditer(l):
                if sql_oid_unchecked(m.group(1), body, k):
                    found.append((path, n, name, 'sql-oid', SQL_OID_MSG))
            if WRAPPED.search(l):
                found.append((path, n, name, 'null-wrapped', 'a result that can be NULL is the argument of a function that dereferences it'))
    seen, out = set(), []
    for h in found:
        if h[:4] not in seen:
            seen.add(h[:4])
            out.append(h)
    return out


def load_allow(path):
    allow = {}
    for l in open(path):
        if l.strip() and not l.startswith('#'):
            f = l.rstrip('\n').split('\t')
            if len(f) != 4 or not f[3].strip():
                sys.exit('%s: a row is FILE TAB FUNCTION TAB RULE TAB REASON: %s' % (path, l.strip()))
            allow[tuple(f[:3])] = f[3]
    return allow


def report(found, allow):
    bad = [h for h in found if (h[0], h[2], h[3]) not in allow]
    used = {(h[0], h[2], h[3]) for h in found}
    stale = sorted(k for k in allow if k not in used)
    return bad, stale


if arg == '--selftest':
    def rules(src):
        return sorted(h[3] for h in hits('a.c', 'static int\nf(PlannerInfo *root, int x)\n{\n' + src + '\n}\n'))
    cases = {
        'planner-array': ['double r = root->simple_rel_array[x]->rows;', 'RelOptInfo *r = find_join_rel(root, s);\nreturn r->rows;'],
        'list-first': ['Path *p = linitial(rel->pathlist);', 'return list_nth(root->glob->subroots, x - 1);'],
        'null-result': ['HeapTuple tp = SearchSysCache1(RELOID, 0);\nreturn GETSTRUCT(tp);', 'char *s = getenv("X");\nreturn s[0];', 'char *n = get_rel_name(x);\nreturn n[0];',
                        'Relation r = try_relation_open(x, NoLock);\nreturn r->rd_id;'],
        'null-wrapped': ['return strcmp(get_func_name(x), "sum");', 'return strlen(getenv("HOME"));', 'return pstrdup(get_rel_name(x));'],
        'sql-oid': ['Relation r = table_open(PG_GETARG_OID(0), NoLock);\nreturn r->rd_id;', 'Oid o = PG_GETARG_OID(0);\nRelation r = index_open(o, NoLock);\nreturn r->rd_id;'],
    }
    for rule, srcs in cases.items():
        for s in srcs:
            assert rule in rules(s), 'missed %s: %s -> %s' % (rule, s, rules(s))
    clean_src = [
        'RelOptInfo *r = root->simple_rel_array[x];\nif (r == NULL)\n\treturn 0;\nreturn r->rows;',
        'if (root->simple_rel_array[x] != NULL)\n\treturn root->simple_rel_array[x]->rows;',
        'RelOptInfo *r = find_join_rel(root, s);\nreturn r ? r->rows : 0;',
        'root->simple_rte_array[x] = e;',
        'return find_base_rel(root, x)->rows;',
        'if (rel->pathlist == NIL)\n\treturn 0;\nreturn linitial(rel->pathlist);',
        'if (list_length(root->glob->subroots) < x)\n\treturn 0;\nreturn list_nth(root->glob->subroots, x - 1);',
        'if (x <= 0)\n\treturn 0;\nreturn list_nth(root->glob->subroots, x - 1);',
        'HeapTuple tp = SearchSysCache1(RELOID, 0);\nif (!HeapTupleIsValid(tp))\n\telog(ERROR, "cache lookup failed");\nreturn 0;',
        'char *n = get_rel_name(x);\nelog(ERROR, "no %s", n);',
        'char *s = getenv("X");\nreturn s != NULL && s[0] == 1;',
        '/* root->simple_rel_array[x]->rows */\nreturn linitial(o->args);',
        'Assert(0);',
        'return strncmp(n, "int", 3) != 0 || strstr(n, "div") == NULL;',
        'return strcmp(sift_func_name(x), "sum");',
        'Relation r = table_open(sift_rel_must_exist(PG_GETARG_OID(0)), NoLock);\nreturn r->rd_id;',
        'Oid o = sift_rel_must_exist(PG_GETARG_OID(0));\nRelation r = index_open(o, NoLock);\nreturn r->rd_id;',
        'Oid o = PG_GETARG_OID(0);\nsift_rel_must_exist(o);\nRelation r = index_open(o, NoLock);\nreturn r->rd_id;',
        'Relation r = table_open(root->relid, NoLock);\nreturn r->rd_id;',
    ]
    for s in clean_src:
        assert not rules(s), 'reported a clean site: %s -> %s' % (s, rules(s))
    assert rules('HeapTuple tp = SearchSysCache1(RELOID, 0);\nAssert(HeapTupleIsValid(tp));\nreturn GETSTRUCT(tp);') == ['null-result'], 'an Assert counted as a test'
    bad, stale = report(hits('a.c', 'static int\nf(PlannerInfo *root, int x)\n{\n\treturn root->simple_rel_array[x]->rows;\n}\n'), {('a.c', 'f', 'planner-array'): 'why', ('a.c', 'g', 'list-first'): 'why'})
    assert not bad and stale == [('a.c', 'g', 'list-first')], 'the allow list did not clear a hit or did not report a stale row'
    print('check-null selftest: ok')
    sys.exit(0)

allow = load_allow(os.path.join(root, 'scripts', 'null.allow'))
names = subprocess.run(['git', '-C', root, 'ls-files', 'src'], capture_output=True, text=True, check=True).stdout.split()
names = [f for f in names if f.endswith(('.c', '.h'))]
found = []
for f in names:
    found += hits(f, open(os.path.join(root, f), errors='replace').read())
bad, stale = report(found, allow)
for f, n, fn, rule, msg in bad:
    print('%s:%d: %s: %s (%s; or a row in scripts/null.allow with the reason it cannot be NULL)' % (f, n, rule, msg, fn))
for k in stale:
    print('scripts/null.allow: %s %s %s has no hit any more: delete the row' % k)
print('check-null: %d unread site(s), %d stale row(s), %d allowed, %d files' % (len(bad), len(stale), len(allow), len(names)))
sites = subprocess.run([sys.executable, '-I', os.path.join(root, 'scripts', 'check-sites.py'), root]).returncode
sys.exit(1 if bad or stale or sites else 0)
PY
