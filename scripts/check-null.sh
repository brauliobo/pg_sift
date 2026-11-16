#!/usr/bin/env bash
# A result that can be NULL is tested before it is used (the crash of 5.0.0: the row estimate of `simple_rel_array[x]` read for the relid of an outer join, which has no RelOptInfo). Per function of
# src/ (the code of a test build, *_test.c, is not read), this fails on:
#   planner-array   `simple_rel_array[..]`, `simple_rte_array[..]`, `find_base_rel(`, `find_join_rel(` whose result is not compared with NULL in the function. A relid of a bitmapset (ppi_req_outer,
#                   relids, all_query_rels) names a base relation, an outer join (no RelOptInfo) or a join; a relid of a FROM item names a base relation. Loop over a set: skip what has no entry
#   list-first      linitial/lsecond/llast/list_nth.. of a list that the planner or the parser fills with a variable length (indexlist, pathlist, subroots, rtable, sortClause, custom_private, ..) when the
#                   function never tests that list (list_length, NIL, a condition, a loop over it)
#   null-result     a variable assigned from a call that returns NULL (SearchSysCache*, try_relation_open, RelationIdGetRelation, get_rel_name, get_func_name, get_opname, get_namespace_name,
#                   malloc, strdup, fopen, fgets, getenv, strchr, strstr, ..) that the function never compares with NULL (or HeapTupleIsValid); a name used only as an argument of a message is not a use
#   null-wrapped    such a name call as the argument of strcmp, strlen, pstrdup, strcpy, .. on the same line: the wrapper dereferences NULL
# A test of an Assert does not count: a release build has none. A site that cannot be NULL says why in scripts/null.allow: FILE TAB FUNCTION TAB RULE TAB REASON (one line); a line whose function no
# longer has a hit fails too, so the list stays the list of the sites that were read.
#   scripts/check-null.sh            the tracked sources of src/
#   scripts/check-null.sh --selftest
set -euo pipefail
here=$(cd "$(dirname "$0")/.." && pwd)
exec python3 -I - "$here" "${1:-}" <<'PY'
import os, re, subprocess, sys

root, arg = sys.argv[1], sys.argv[2]
FUNC = re.compile(r'^([A-Za-z_]\w*)\(')
TYPE_LINE = re.compile(r'^[A-Za-z_][\w \t\*]*$')

ARRAYS = re.compile(r'\b(simple_rel_array|simple_rte_array)\[|\b(find_base_rel|find_join_rel|find_base_rel_ignore_join)\(')
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


def hits(path, text):
    lines = text.split('\n')
    cl = clean(lines)
    found = []
    for name, a, b in functions(lines):
        body = cl[a:b + 1]
        for k, l in enumerate(body):
            n = a + k + 1
            for m in ARRAYS.finditer(l):
                if not relation_entry_tested(m, l, body):
                    found.append((path, n, name, 'planner-array', 'a relation entry that no code of the function compares with NULL: an outer join relid has none; skip it as the neighbouring loops do'))
            for m in LIST_ACCESS.finditer(l):
                arg = m.group(2).strip()
                if VARIABLE_LISTS.search(arg) and not guarded_list(arg, body):
                    found.append((path, n, name, 'list-first', '%s of %s with no test of the list in the function (list_length or NIL)' % (m.group(1), arg)))
            for rule, call in PRODUCERS:
                for m in ASSIGN(call).finditer(l):
                    v = m.group(1)
                    if (rule == 'name' and only_messages(v, body, k)) or re.search(call + r'[^;]*\)\s*(\?|[!=]=\s*NULL)', l):
                        continue
                    if not compared(v, body):
                        found.append((path, n, name, 'null-result', '%s is assigned a result that can be NULL and the function never tests it' % v))
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
    }
    for rule, srcs in cases.items():
        for s in srcs:
            assert rule in rules(s), 'missed %s: %s -> %s' % (rule, s, rules(s))
    clean_src = [
        'RelOptInfo *r = root->simple_rel_array[x];\nif (r == NULL)\n\treturn 0;\nreturn r->rows;',
        'if (root->simple_rel_array[x] != NULL)\n\treturn root->simple_rel_array[x]->rows;',
        'RelOptInfo *r = find_join_rel(root, s);\nreturn r ? r->rows : 0;',
        'if (rel->pathlist == NIL)\n\treturn 0;\nreturn linitial(rel->pathlist);',
        'if (list_length(root->glob->subroots) < x)\n\treturn 0;\nreturn list_nth(root->glob->subroots, x - 1);',
        'HeapTuple tp = SearchSysCache1(RELOID, 0);\nif (!HeapTupleIsValid(tp))\n\telog(ERROR, "cache lookup failed");\nreturn 0;',
        'char *n = get_rel_name(x);\nelog(ERROR, "no %s", n);',
        'char *s = getenv("X");\nreturn s != NULL && s[0] == 1;',
        '/* root->simple_rel_array[x]->rows */\nreturn linitial(o->args);',
        'Assert(0);',
        'return strncmp(n, "int", 3) != 0 || strstr(n, "div") == NULL;',
        'return strcmp(sift_func_name(x), "sum");',
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
names = [f for f in names if f.endswith(('.c', '.h')) and not f.endswith('_test.c')]
found = []
for f in names:
    found += hits(f, open(os.path.join(root, f), errors='replace').read())
bad, stale = report(found, allow)
for f, n, fn, rule, msg in bad:
    print('%s:%d: %s: %s (%s; or a row in scripts/null.allow with the reason it cannot be NULL)' % (f, n, rule, msg, fn))
for k in stale:
    print('scripts/null.allow: %s %s %s has no hit any more: delete the row' % k)
print('check-null: %d unread site(s), %d stale row(s), %d allowed, %d files' % (len(bad), len(stale), len(allow), len(names)))
sys.exit(1 if bad or stale else 0)
PY
