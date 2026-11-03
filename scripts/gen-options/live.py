# Splicing a block into a file, loading the sources and the record, rendering, and --live (a new database against the sources).
# Run into the namespace of main.py (exec), not imported; not a script of its own.
def splice(text, tag, block, errors, path):
    m = re.search(r'<!-- %s:start -->.*?<!-- %s:end -->' % (tag, tag), text, re.S)
    if not m:
        errors.append('%s has no <!-- %s:start --> ... <!-- %s:end --> block' % (path, tag, tag))
        return text
    return text[:m.start()] + block + text[m.end():]


def sources():
    out = {}
    for d, _, fs in os.walk(os.path.join(ROOT, 'src')):
        for f in fs:
            if f.endswith(('.c', '.h')):
                out[os.path.relpath(os.path.join(d, f), os.path.join(ROOT, 'src'))] = open(os.path.join(d, f), encoding='utf-8').read()
    return out


def load(errors, srcs=None, notes_text=None, released_text=None):
    opts, errs = collect(srcs or sources())
    errors += errs
    notes = read_tsv(notes_text if notes_text is not None else read('docs/options.tsv'), NOTE_COLS, 'docs/options.tsv', errors)
    rel = read_tsv(released_text if released_text is not None else read('scripts/options-released.tsv'), ['key', 'default'], 'scripts/options-released.tsv', errors)
    released = {k: r['default'] for k, r in rel.items()}
    check(opts, notes, errors, released, srcs)
    return opts, notes, released


def render(opts, notes, released):
    return generate(opts, notes, released, release_label(), released_label())


# ---- --live ----
def live(opts):
    errors = []
    db = 'sift_options_%d' % os.getpid()
    psql = lambda d, q: subprocess.run(['psql', '-X', '-q', '--csv', '-v', 'ON_ERROR_STOP=1', '-d', d], input=q, capture_output=True, text=True)
    r = psql('postgres', 'CREATE DATABASE ' + db)
    if r.returncode:
        return ['live: cannot create a database: ' + r.stderr.strip()]
    try:
        # one session: the settings of the library exist once it is loaded
        r = psql(db, """CREATE EXTENSION pg_sift; CREATE EXTENSION pg_sift_test; SELECT sift.version() \\g /dev/null
            SELECT 'guc', name, vartype, boot_val, coalesce(min_val, ''), coalesce(max_val, ''), context, coalesce(unit, ''), short_desc, coalesce(extra_desc, ''), coalesce(array_to_string(enumvals, ','), '')
              FROM pg_settings WHERE name LIKE 'sift.%';
            SELECT 'rel', am, name, type, dflt, '', '', '', '', '', '' FROM sift._test_reloptions()""")
        if r.returncode:
            return ['live: ' + r.stderr.strip() + ' (a SIFT_TESTING build is needed: scripts/pgtest.sh install)']
        out = [x for x in csv.reader(r.stdout.splitlines(True)) if len(x) == 11 and x[0] in ('guc', 'rel')]
        have = {}
        for x in out:
            if x[0] == 'guc':
                have[x[1]] = x
            else:
                have['%s(%s)' % (x[1], x[2])] = x
        mine = {o['key']: o for o in opts}
        ctx = {'user': 'session', 'superuser': 'superuser', 'sighup': 'reload', 'postmaster': 'restart'}
        for k, x in sorted(have.items()):
            o = mine.get(k)
            if o is None:
                errors.append('live: %s is in the library but not in src/ as scripts/gen-options.sh reads it (a call it does not parse)' % k)
                continue
            if o['kind'] == 'guc':
                _, _, vt, boot, lo, hi, cx, unit, short, extra, enums = x
                same = {'bool': lambda: (boot == 'on') == o['default'], 'integer': lambda: int(boot) == o['default'] and int(lo) == o['lo'] and int(hi) == o['hi'],
                        'real': lambda: float(boot) == o['default'] and float(lo) == o['lo'] and float(hi) == o['hi'], 'string': lambda: boot == o['default'],
                        # sift.simd starts at the top level of the architecture (src/core/cpu.h): neon on AArch64, which hides the levels of x86-64 (avx2, avx512) that src/ lists
                        'enum': lambda: (boot == o['default'] or (k == 'sift.simd' and boot == 'neon' and os.uname().machine in ('aarch64', 'arm64')))
                                        and set(o['members']) - ({'avx2', 'avx512'} if k == 'sift.simd' and os.uname().machine in ('aarch64', 'arm64') else set()) <= set(enums.split(','))}[vt]
                if not same():
                    errors.append('live: %s: the library has default %s (%s%s) but src/ says %s' % (k, boot, lo, '..' + hi if hi else '', fmt(o, o['default'])))
                if ctx.get(cx) != o['ctx']:
                    errors.append('live: %s: the context is %s in the library, %s in src/' % (k, cx, o['ctx']))
                if norm(short) != o['short'] or norm(extra) != o['long']:
                    errors.append('live: %s: the description of the library differs from src/' % k)
            else:
                typ, dflt = x[3], x[4]
                if typ != o['type']:
                    errors.append('live: %s: type %s in the library, %s in src/' % (k, typ, o['type']))
                elif typ != 'enum' and {'bool': lambda: (dflt == 'true') == o['default'], 'int': lambda: int(dflt) == o['default'],
                                        'real': lambda: float(dflt) == o['default'], 'string': lambda: dflt == o['default']}[typ]() is False:
                    errors.append('live: %s: the library has default %s but src/ says %s' % (k, dflt, fmt(o, o['default'])))
        for k, o in mine.items():
            if k not in have and not o['testing']:
                errors.append('live: %s is in src/ but not in the library' % k)
    finally:
        psql('postgres', 'DROP DATABASE IF EXISTS %s WITH (FORCE)' % db)
    return errors


# ---- --selftest: the checks fail on what they are for ----
