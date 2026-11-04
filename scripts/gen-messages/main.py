# The entry of scripts/gen-messages.sh: the emitters of src/ and sql/ (emitters.py), the rows of docs/messages.tsv, the checks that they agree, and docs/MESSAGES.md.
# The parts are exec'd, not imported, as in scripts/gen-options/main.py.
import functools
import os
import re
import subprocess
import sys
sys.dont_write_bytecode = True
sys.path.insert(0, 'scripts')
from genlib import call_end, esc, norm, split_args, strip_comments, testing_lines

mode = sys.argv[1]
ROOT = os.getcwd()
COLS = ['id', 'group', 'level', 'source', 'message', 'detail', 'hint', 'fires', 'when', 'title', 'decision', 'default', 'gain', 'cost', 'benefit', 'tradeoff', 'switch', 'option', 'evidence', 'doc', 'kind', 'choice', 'why', 'off']
CHOICE_KINDS = ('auto', 'advice', 'limit', 'action')    # a row of the summary of a build, derived from the index by src/col/build/choices.c (kind empty: a message raised where the source says)
DERIVED_KINDS = ('auto', 'advice', 'limit')             # no emitter of its own: the summary NOTICE and sift.index_choices() are the places it appears
CHOICES_C = 'src/col/build/choices.c'
CHOICES_H = 'src/col/build/choices_gen.h'
LINE_MAX = 100
GROUPS = {
    'create': ('CREATE INDEX and REINDEX: choices and advice', 'What the build of a `sift_col` index chose by itself, and what it advises without changing anything. Each choice is made from the statistics and a sample of the table, is written into the options of the index (`auto: ...`), and is kept by a later `REINDEX`. The build says them all in one NOTICE (row `index-choices-summary`); the rows of kind CHOICE, ADVICE and LIMIT are the ones `sift.index_choices` returns, with their value on the index.'),
    'limits': ('Limits reached by a build: units without what was asked', 'Where a cap of an option left units without a summary, a list or a dictionary, so a query scans them. The build changed no answer; it says how much of the index is affected.'),
    'maint': ('Maintenance, VACUUM and background workers', 'What the merges, the vacuums and the background workers did or failed to do. A WARNING says a step failed and is retried; `VACUUM VERBOSE` adds the INFO lines.'),
    'planning': ('Planning and execution', 'Why a query kept the normal plan, what the aggregate scan does when a resource it asked for is missing, and the optional GPU path.'),
    'upgrade': ('Upgrade and format', 'Printed by `ALTER EXTENSION pg_sift UPDATE` or by a function of a released script; those scripts are frozen, so these texts do not change.'),
}
LEVELS_OK = ('NOTICE', 'WARNING', 'INFO')
REQUIRED = ['id', 'group', 'level', 'source', 'message', 'fires', 'when', 'decision', 'benefit', 'tradeoff', 'switch', 'doc']
NONE = 'no measured number yet'


def read(path):
    with open(os.path.join(ROOT, path), encoding='utf-8') as f:
        return f.read()


def git_files(prefix):
    return subprocess.run(['git', 'ls-files', '-co', '--exclude-standard', '--', prefix], capture_output=True, text=True, check=True).stdout.split('\n')[:-1]


VARIABLE_LEVELS = {'elevel': 'INFO', 'GPU_LEVEL': 'WARNING'}    # a level that is a name: the level its row has
path = os.path.join(os.path.dirname(os.path.abspath(__file__)), 'emitters.py')
with open(path, encoding='utf-8') as f:
    exec(compile(f.read(), path, 'exec'), globals())


# ---- the rows ----
def read_rows(errors):
    rows = []
    for n, line in enumerate(read('docs/messages.tsv').split('\n'), 1):
        if not line.strip() or line.startswith('#'):
            continue
        f = line.split('\t')
        f += [''] * (len(COLS) - len(f))
        if len(f) != len(COLS):
            errors.append('docs/messages.tsv:%d: %d columns, not %d (%s)' % (n, len(f), len(COLS), ', '.join(COLS)))
            continue
        rows.append(dict(zip(COLS, [x.strip() for x in f]), line=n))
    return rows


def key(level, msg):
    return (level, norm(msg))


def numbers(text):
    """the numbers of a text, thousands separators undone; a lone digit is not a measurement"""
    out = set()
    for t in re.findall(r'(?<![\w.])\d[\d,]*(?:\.\d+)?', text):
        t = t.replace(',', '')
        if not re.fullmatch(r'\d', t):
            out.add(t)
    return out


def heading_anchors(path):
    fence, out = False, set()
    for line in read(path).split('\n'):
        fence ^= line.startswith('```')
        m = None if fence else re.match(r'#{1,6}\s+(.*?)\s*#*$', line)
        if m:
            out.add(re.sub(r'\s', '-', re.sub(r'[^\w\s-]', '', m.group(1).replace('`', '').lower())))
    return out


def option_text(keys, errors, where):
    """the text of the rows of docs/options.tsv named by keys (the measurements the numbers of a message may quote)"""
    text = ''
    for line in read('docs/options.tsv').split('\n'):
        if line and not line.startswith('#'):
            k = line.split('\t')[0]
            if k in keys:
                text += line + '\n'
                keys = [x for x in keys if x != k]
    for k in keys:
        errors.append('%s: option %s has no row in docs/options.tsv' % (where, k))
    return text


def derive_ids():
    """the choices src/col/build/choices.c derives: the first field of each line of its table, {"id", function}"""
    return set(re.findall(r'^\t\{"([a-z0-9-]+)", ', read(CHOICES_C), re.M))


def length_cells(r):
    """the cells a person reads in the terminal, with their limit: a first line of LINE_MAX characters, a DETAIL of two lines, a HINT of one"""
    out = [('message', r['message'].split('\n')[0], LINE_MAX), ('detail', r['detail'], 2 * LINE_MAX), ('hint', r['hint'], LINE_MAX)]
    if r['kind']:
        out += [('why', r['why'], LINE_MAX), ('off', r['off'], 2 * LINE_MAX)]
    return out


def length_allow():
    """docs/messages-length.allow: id and cell of the older rows over the limit, until their source is reworded (one per line, tab separated)"""
    path_ = os.path.join(ROOT, 'docs/messages-length.allow')
    if not os.path.exists(path_):
        return set()
    return {tuple(x.split('\t')[:2]) for x in read('docs/messages-length.allow').split('\n') if x and not x.startswith('#')}


def check(rows, emitters, errors):
    seen, ids = {}, set()
    for r in rows:
        where = 'docs/messages.tsv:%d: %s' % (r['line'], r['id'])
        if not re.fullmatch(r'[a-z][a-z0-9-]*', r['id']) or r['id'] in ids:
            errors.append('%s: the id is not a lowercase slug or is twice' % where)
        ids.add(r['id'])
        for c in REQUIRED:
            if not r[c]:
                errors.append('%s: the cell %s is empty' % (where, c))
        if r['group'] not in GROUPS:
            errors.append('%s: group %s is none of %s' % (where, r['group'], ', '.join(GROUPS)))
        if r['level'] not in LEVELS_OK:
            errors.append('%s: level %s is none of %s' % (where, r['level'], ', '.join(LEVELS_OK)))
        if r['title'] and not (r['default'] and r['gain'] and r['cost']):
            errors.append('%s: a row with a title (decisions at a glance) needs default, gain and cost' % where)
        derived = r['kind'] in DERIVED_KINDS
        if r['kind'] and r['kind'] not in CHOICE_KINDS:
            errors.append('%s: kind %s is none of %s' % (where, r['kind'], ', '.join(CHOICE_KINDS)))
        if r['kind']:
            for c in ('choice', 'why', 'off'):
                if not r[c]:
                    errors.append('%s: a row of kind %s needs the cell %s (sift.index_choices shows it)' % (where, r['kind'], c))
            if derived and r['source'] != CHOICES_C:
                errors.append('%s: a row of kind %s is derived in %s, not in %s' % (where, r['kind'], CHOICES_C, r['source']))
        elif r['choice'] or r['why'] or r['off']:
            errors.append('%s: choice, why and off belong to a row with a kind' % where)
        k = key(r['level'], r['message'])
        if not derived:
            if k in seen:
                errors.append('%s: the message is also the one of %s' % (where, seen[k]['id']))
            seen[k] = r
        e = None if derived else emitters.get(k)
        if derived:
            pass
        elif e is None:
            errors.append('%s: no %s with this message in src/ or sql/ (stale row; the text of the source: scripts/gen-messages.sh --skeleton)' % (where, r['level']))
        else:
            if r['source'] not in e['files']:
                errors.append('%s: the message is raised in %s, not in %s' % (where, ', '.join(sorted(e['files'])), r['source']))
            for c in ('detail', 'hint'):
                if e[c] not in ('?', '') and norm(e[c]) != norm(r[c]):
                    errors.append('%s: %s is "%s" in the source, "%s" in the row' % (where, c, e[c][:60], r[c][:60]))
                if e[c] == '' and r[c]:
                    errors.append('%s: the source has no %s' % (where, c))
        for c in ('doc',):
            m = re.fullmatch(r'([A-Za-z0-9_.-]+\.md)(?:#(\S+))?', r[c])
            if not m or not os.path.exists(os.path.join(ROOT, 'docs', m.group(1))) or (m.group(2) and m.group(2) not in heading_anchors('docs/' + m.group(1))):
                errors.append('%s: doc %s is not a file and heading of docs/' % (where, r[c]))
        evid = option_text([x.strip() for x in r['option'].split(',') if x.strip()], errors, where)
        for p in [x.strip() for x in r['evidence'].split(',') if x.strip()]:
            if os.path.exists(os.path.join(ROOT, p)):
                evid += read(p)
            else:
                errors.append('%s: evidence %s is not a file' % (where, p))
        have = numbers(evid)
        for c in ('gain', 'cost', 'benefit', 'tradeoff', 'why'):
            if not r[c]:
                continue
            if c == 'why':
                for n in sorted(numbers(r[c]) - have):
                    errors.append('%s: the number %s of why is in no row of docs/options.tsv named in option and in no file of evidence' % (where, n))
                continue
            for n in sorted(numbers(r[c]) - have):
                errors.append('%s: the number %s of %s is in no row of docs/options.tsv named in option and in no file of evidence' % (where, n, c))
            if not numbers(r[c]) and NONE not in r[c] and not r[c].startswith('n/a:'):
                errors.append('%s: %s has no number: give one from the evidence, say "%s", or "n/a: reason"' % (where, c, NONE))
    code = derive_ids()
    want = {r['id'] for r in rows if r['kind']}
    for i in sorted(code - want):
        errors.append('%s derives "%s", which has no row of a kind in docs/messages.tsv' % (CHOICES_C, i))
    for i in sorted(want - code):
        errors.append('docs/messages.tsv: %s has a kind, and %s derives no such choice' % (i, CHOICES_C))
    allow = length_allow()
    for r in rows:
        for what, text, limit in length_cells(r):
            if len(text) > limit and (r['id'], what) not in allow:
                errors.append('docs/messages.tsv: %s: %s is %d characters, the limit is %d (plain words, one short line; docs/messages-length.allow lists the old ones)' % (r['id'], what, len(text), limit))
        for what in ('message', 'detail', 'hint', 'why', 'off', 'decision'):
            if re.search(r'(^|[\s(])\.\.($|[\s,;)])', r[what]) and (r['id'], what + ' ..') not in allow:
                errors.append('docs/messages.tsv: %s: %s says ".." for "..."-style shorthand; write the words out' % (r['id'], what))
    for k, e in emitters.items():
        if k not in seen:
            errors.append('no row for the %s of %s: %s (add one to docs/messages.tsv; scripts/gen-messages.sh --skeleton prints it)' % (k[0], ', '.join(sorted(e['files'])), k[1][:100]))


def load_emitters():
    out = {}
    for e in c_emitters():
        k = key(e['level'], e['msg'])
        out.setdefault(k, dict(e, files=set()))['files'].add(e['file'])
    for e in sql_emitters():
        out.setdefault(key(e['level'], e['msg']), e)
    return out


def unmeasured(rows):
    return [(r['id'], [c for c in ('benefit', 'tradeoff') if NONE in r[c]]) for r in rows if any(NONE in r[c] for c in ('benefit', 'tradeoff'))]


# ---- the document ----
def msg_cell(r):
    label = {'auto': 'CHOICE', 'advice': 'ADVICE', 'limit': 'LIMIT', 'action': r['level']}.get(r['kind'], r['level'])
    return '<a id="%s"></a>**%s** `%s`' % (r['id'], label, esc(r['message'].replace('%%', '%')))


def doc_link(r):
    return '[%s](%s)' % (r['doc'].replace('.md', ''), r['doc'])


def render(rows):
    by = {g: [r for r in rows if r['group'] == g] for g in GROUPS}
    levels = {l: sum(r['level'] == l for r in rows) for l in LEVELS_OK}
    out = ['# Messages: every NOTICE, WARNING and INFO of pg_sift', '',
           'Generated by `scripts/gen-messages.sh` from `docs/messages.tsv`; do not edit by hand. `scripts/gen-messages.sh --check` (lint) fails when a message of `src/` or `sql/` has no row, when a row names a message that no longer exists, when the DETAIL or HINT of a row differs from the source, and when a number of a row is in none of the measurements it cites.',
           '',
           'pg_sift tells what it chose on its own and what it advises, in the message levels the client shows by default: **%d NOTICE**, **%d WARNING** and **%d INFO** (the lines `VACUUM VERBOSE` adds). Every message has one row below: the exact text (`%%s`, `%%d`, `%%u`, `%%.0f` are the values it fills in), where and when it appears, the decision pg_sift took, the benefit and the trade-off of that decision with a measured number, and how to change it or switch it off. A cell that says *%s* is a measurement nobody made yet.' % (levels['NOTICE'], levels['WARNING'], levels['INFO'], NONE),
           '',
           '- `SET client_min_messages = warning` hides the NOTICEs and the INFO lines for the session; the choice they report is made either way. `ALTER DATABASE .. SET client_min_messages = warning` or `ALTER ROLE` keeps it.',
           '- `CREATE INDEX` and `REINDEX` of a `sift_col` index print ONE NOTICE for all the choices they made, the advice and the limits they reached: the count, the values on two lines, and a HINT that names `SELECT * FROM sift.index_choices(\'index\')`. That function returns one row of this page for each choice (the **CHOICE**, **ADVICE** and **LIMIT** rows below): its value on the index, the cost measured on it, the reason and the command that turns it off. `SELECT * FROM sift.help(\'slug\')` returns any row of this page by its anchor, and `sift.help()` all of them.',
           '- The DETAIL and HINT of a message are listed in full [at the end](#detail-and-hint-in-full). An ERROR with its HINT is not in this table: the refusals of the aggregate scan are in [USAGE](USAGE.md#why-a-query-kept-the-normal-plan) and the limits in [LIMITS](LIMITS.md). LOG and DEBUG messages go to the server log, not to the client, and are not listed.',
           '- Numbers are in the units of their source: instructions of one backend, milliseconds, bytes, percent of the index; the conditions (rows, columns, version) are in the cited option case of [OPTIONS](OPTIONS.md).',
           '', '## Decisions at a glance', '',
           '| Decision | Default | Gain | Cost | Change or switch off | Row |', '|---|---|---|---|---|---|']
    for r in rows:
        if r['title']:
            out.append('| %s | %s | %s | %s | %s | [%s](#%s) |' % (esc(r['title']), esc(r['default']), esc(r['gain']), esc(r['cost']), esc(re.split(r'(?<!\.)\.\s', r['switch'])[0]), r['id'], r['id']))
    for g, (title, intro) in GROUPS.items():
        out += ['', '## ' + title, '', intro, '', '| Message | Fires at, and when | Decision | Benefit | Trade-off | Change or switch off | Docs |', '|---|---|---|---|---|---|---|']
        for r in by[g]:
            out.append('| %s | %s. %s | %s | %s | %s | %s | %s |' % (msg_cell(r), esc(r['fires'].rstrip('.')), esc(r['when']), esc(r['decision']), esc(r['benefit']), esc(r['tradeoff']), esc(r['switch']), doc_link(r)))
    out += ['', '## DETAIL and HINT in full', '', 'The text after the message line, as `psql` prints it (`%s`, `%d` fill in values). A DETAIL or HINT built from pieces at run time shows its fixed sentences.', '']
    for r in rows:
        if r['detail'] or r['hint']:
            out.append('- **%s** (%s)' % (r['id'], r['level']))
            for lab, c in (('DETAIL', 'detail'), ('HINT', 'hint')):
                if r[c]:
                    out.append('  - %s: %s' % (lab, esc(r[c].replace('%%', '%'))))
    return '\n'.join(out) + '\n'


def cstr(t):
    return '"' + t.replace('\\', '\\\\').replace('"', '\\"').replace('\n', '\\n') + '"'


def render_header(rows):
    """src/col/build/choices_gen.h: the rows as the table sift.help() and sift.index_choices() read, so that the text of a message is written once (docs/messages.tsv)"""
    out = ['/* Generated by scripts/gen-messages.sh from docs/messages.tsv; do not edit. */', '',
           'typedef struct SiftMsgRow', '{']
    out += ['\tconst char *%s;' % c for c in ('id', 'kind', 'choice', 'level', 'message', 'detail', 'hint', 'decision', 'why', 'off', 'typical', 'doc')]
    out += ['} SiftMsgRow;', '', 'static const SiftMsgRow sift_msg_rows[] = {']
    for r in rows:
        typical = r['cost'] if r['title'] and not r['cost'].startswith('n/a') and NONE not in r['cost'] else ''
        cells = [r['id'], r['kind'], r['choice'], r['level'], r['message'], r['detail'], r['hint'], r['decision'], r['why'], r['off'], typical, r['doc']]
        out.append('\t{' + ', '.join(cstr(c) for c in cells) + '},')
    out += ['};', '']
    return '\n'.join(out)


# ---- the selftest ----
def selftest():
    errors = []
    emitters = load_emitters()
    rows = read_rows(errors)
    check(rows, emitters, errors)
    if errors:
        return ['selftest: the tree is not clean: ' + errors[0]]
    cases = [
        ('a message without a row', lambda r: r[1:], 'no row for the'),
        ('a row of a message that is gone', lambda r: r + [dict(r[0], id='gone', message='a message that no source raises')], 'stale row'),
        ('a reworded message', lambda r: [dict(r[0], message=r[0]['message'] + ' (reworded)')] + r[1:], 'stale row'),
        ('another level', lambda r: [dict(r[0], level='WARNING' if r[0]['level'] != 'WARNING' else 'NOTICE')] + r[1:], 'stale row'),
        ('a changed hint', lambda r: [dict(next(x for x in r if x['hint']), hint='another hint')] + [x for x in r if not x['hint']], 'in the source'),
        ('an invented number', lambda r: [dict(r[0], benefit=r[0]['benefit'] + ' and 987654.321 times')] + r[1:], 'is in no row'),
        ('an unknown option', lambda r: [dict(r[0], option='sift.no_such_setting')] + r[1:], 'has no row in docs/options.tsv'),
        ('a derived choice without a row', lambda r: [x for x in r if x['kind'] != 'auto'], 'derives'),
        ('a row with a kind that nothing derives', lambda r: r + [dict(r[0], id='nothing-derives', kind='auto', source=CHOICES_C, choice='x', why='x', off='x', message='nothing derives')], 'derives no such choice'),
        ('a first line over the limit', lambda r: [dict(r[0], message='x' * 130)] + r[1:], 'characters'),
        ('a shorthand with two dots', lambda r: [dict(r[0], decision='run ALTER INDEX .. SET (x)')] + r[1:], 'shorthand'),
    ]
    for name, mutate, want in cases:
        errs = []
        check(mutate([dict(r) for r in rows]), emitters, errs)
        if not any(want in e for e in errs):
            errors.append('selftest: %s was not caught (wanted "%s")' % (name, want))
    return errors


# ---- main ----
errors = []
if mode == '--selftest':
    errors = selftest()
    if not errors:
        print('messages: selftest ok')
elif mode == '--skeleton':
    emitters = load_emitters()
    have = {key(r['level'], r['message']) for r in read_rows(errors)}
    for k, e in emitters.items():
        if k not in have:
            src = sorted(e['files'])[0]
            ident = re.sub(r'[^a-z0-9]+', '-', re.sub(r'%[-+ 0#]*[0-9*]*(?:\.[0-9*]+)?(?:l|ll|z)?[a-zA-Z]', '', k[1].lower())).strip('-')[:48]
            cells = dict(dict.fromkeys(COLS, ''), id=ident, level=k[0], source=src, message=k[1], detail=e['detail'] if e['detail'] != '?' else '', hint=e['hint'] if e['hint'] != '?' else '')
            print('\t'.join(cells[c] for c in COLS))
else:
    emitters = load_emitters()
    rows = read_rows(errors)
    check(rows, emitters, errors)
    if mode == '--missing':
        for i, cs in unmeasured(rows):
            print('%s\t%s' % (i, ' '.join(cs)))
        print('messages: %d of %d rows have a cell without a measured number' % (len(unmeasured(rows)), len(rows)), file=sys.stderr)
    elif not errors:
        outputs = {'docs/MESSAGES.md': render(rows), CHOICES_H: render_header(rows)}
        for name, text in outputs.items():
            if mode == '--check' and read(name) != text:
                errors.append('%s is not what docs/messages.tsv generates: run scripts/gen-messages.sh' % name)
            elif mode == 'write':
                with open(os.path.join(ROOT, name), 'w', encoding='utf-8') as f:
                    f.write(text)
        if not errors:
            by = {l: sum(r['level'] == l for r in rows) for l in LEVELS_OK}
            print('messages: ok, %d messages (%s)' % (len(rows), ', '.join('%d %s' % (by[l], l) for l in LEVELS_OK)))
if errors:
    for e in errors:
        print('messages: ' + e, file=sys.stderr)
    sys.exit(1)
