# The emitters: every NOTICE, WARNING and INFO that src/ and sql/ can send to the client, read from the sources. Run into the namespace of main.py (exec).
SKIP_C = re.compile(r'(^|/)(test|[^/]*_test[^/]*)(\.[ch]|/)')    # test-only files and directories (the build with SIFT_TESTING)
SKIP_SQL = re.compile(r'pg_sift_(helpers|test)--')                # the test helper extensions are never installed by a normal make install
LEVELS = ('NOTICE', 'WARNING', 'INFO')
C_CALL = re.compile(r'\b(ereport|elog)\(\s*([A-Za-z_][A-Za-z_0-9]*)')
SQL_RAISE = re.compile(r'\bRAISE\s+(NOTICE|WARNING|INFO)\b', re.I)
ESC = {'n': '\n', 't': '\t', '"': '"', '\\': '\\', "'": "'"}


def c_text(arg):
    """the text of a C argument made only of string literals (adjacent literals join); None when it holds anything else"""
    arg = arg.strip()
    if not re.fullmatch(r'(?:"(?:\\.|[^"\\])*"\s*)+', arg):
        return None
    return ''.join(re.sub(r'\\(.)', lambda m: ESC.get(m.group(1), m.group(0)), s) for s in re.findall(r'"((?:\\.|[^"\\])*)"', arg))


def part(body, fn):
    """the text of the first argument of fn(...) in an ereport body, '' when the call has none, '?' when that argument is not one literal (built at run time)"""
    m = re.search(r'\b%s\s*\(' % fn, body)
    if not m:
        return ''
    i = body.index('(', m.start())
    t = c_text(split_args(body[i + 1:call_end(body, i)])[0])
    return '?' if t is None or t == '%s' else t


def c_emitters():
    out = []
    for path in sorted(git_files('src')):
        if not path.endswith(('.c', '.h')) or SKIP_C.search(path):
            continue
        text = read(path)
        t = strip_comments(text)
        skip = testing_lines(text)
        for m in C_CALL.finditer(t):
            line = t.count('\n', 0, m.start()) + 1
            level = m.group(2)
            if line in skip or not (level in LEVELS or level in VARIABLE_LEVELS):
                continue
            i = t.index('(', m.start())
            body = t[i + 1:call_end(t, i)]
            if m.group(1) == 'elog':
                msg = c_text(split_args(body)[1]) or '?'
                detail = hint = ''
            else:
                msg = part(body, 'errmsg')
                detail = part(body, 'errdetail') or part(body, 'errdetail_internal')
                hint = part(body, 'errhint')
            out.append(dict(kind='c', file=path, line=line, level=VARIABLE_LEVELS.get(level, level), msg=msg, detail=detail, hint=hint))
    return out


def sql_text(s):
    """the value of a run of SQL string literals, '' doubled quotes undone"""
    return ''.join(x.replace("''", "'") for x in re.findall(r"'((?:''|[^'])*)'", s))


def sql_emitters():
    """one emitter for each RAISE of the scripts; the same text in several scripts (the full install scripts repeat a function) is one emitter with all the files. A RAISE '%' format(msg[i], ..) takes the texts of the ARRAY literals msg and hint of its block"""
    found = {}
    for path in sorted(git_files('sql')):
        if not path.endswith('.sql') or SKIP_SQL.search(path):
            continue
        text = read(path)
        for m in SQL_RAISE.finditer(text):
            end = text.index(';', m.end())
            stmt = text[m.end():end]
            lit = re.match(r"\s*('(?:''|[^'])*'(?:\s*\n\s*'(?:''|[^'])*')*)", stmt)
            msg = sql_text(lit.group(1)) if lit else '?'
            hint = re.search(r"\bHINT\s*=\s*('(?:''|[^'])*'(?:\s*\n\s*'(?:''|[^'])*')*)", stmt)
            detail = re.search(r"\bDETAIL\s*=\s*('(?:''|[^'])*')", stmt)
            if msg == '%':
                block = text[text.rindex('DECLARE', 0, m.start()):m.start()]
                arrays = {a: [sql_text(x) for x in re.findall(r"'(?:''|[^'])*'", body)] for a, body in re.findall(r'\b(msg|hint)\s+text\[\]\s*:=\s*ARRAY\[(.*?)\];', block, re.S)}
                for k, mm in enumerate(arrays.get('msg', [])):
                    found.setdefault(('WARNING', mm), dict(kind='sql', files=set(), level=m.group(1).upper(), msg=mm, detail='?', hint=(arrays.get('hint') or [''] * 9)[k]))['files'].add(path)
                continue
            e = found.setdefault((m.group(1).upper(), msg), dict(kind='sql', files=set(), level=m.group(1).upper(), msg=msg, detail=sql_text(detail.group(1)) if detail else '', hint=sql_text(hint.group(1)) if hint else ''))
            e['files'].add(path)
    return list(found.values())
