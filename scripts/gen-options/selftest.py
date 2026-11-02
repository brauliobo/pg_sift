# --selftest: proves in memory that a new option without a row, a row without an option and a changed default are caught.
# Run into the namespace of main.py (exec), not imported; not a script of its own.
def selftest():
    srcs = sources()
    errs = []
    opts, notes, released = load(errs)
    if errs:
        return errs
    text = read('docs/options.tsv')
    victim = next(o for o in opts if o['kind'] == 'guc' and not o['testing'] and not notes[o['key']]['group'])
    victim_row = next(l for l in text.split('\n') if l.startswith(victim['key'] + '\t'))
    # an option the last release already had, taken out of its snapshot: it is new for the check, whatever release the snapshot is of
    rows = {l.split('\t')[0]: l for l in text.split('\n')}
    fresh = next(o['key'] for o in opts if not o['testing'] and o['key'] in released and o['key'] in rows)
    snapshot = read('scripts/options-released.tsv')
    without_fresh = ''.join(l for l in snapshot.splitlines(True) if not l.startswith(fresh + '\t'))
    fresh_row = rows[fresh]
    rows_vis = rows['sift.vis_page_cache']      # its bench is a results table

    def set_cell(row, col, v):
        f = row.split('\t')
        f += [''] * (len(NOTE_COLS) - len(f))
        f[NOTE_COLS.index(col)] = v
        return '\t'.join(f)

    cases = [
        ('a row is missing', dict(notes_text=text.replace(victim_row + '\n', '')), victim['key'] + ' has no row'),
        ('a row names no option', dict(notes_text=text + 'sift.no_such_setting\tall\t\t\t\tx\ty\t\n'), 'sift.no_such_setting is not an option'),
        ('a new setting', dict(srcs={**srcs, 'core/handler.c': srcs['core/handler.c'] + '\nvoid x(void){ DefineCustomBoolVariable("sift.zz_new", "s", NULL, &v, true, PGC_USERSET, 0, NULL, NULL, NULL); }'}),
         'sift.zz_new has no row'),
        ('a new reloption', dict(srcs={**srcs, 'col/am/options.c': srcs['col/am/options.c'] + '\nvoid x(void){ add_bool_reloption(col_relopt_kind, "zz_new", "d", true, ShareUpdateExclusiveLock); }'}),
         'sift_col(zz_new) has no row'),
        ('a reloption that is not parsed', dict(srcs={**srcs, 'col/am/options.c': srcs['col/am/options.c'].replace('{"superunit", RELOPT_TYPE_INT, offsetof(SiftColOptions, superunit)},', '')}),
         'superunit of sift_col is registered but not parsed'),
        ('a class that does not exist', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'class', 'bogus'))), 'class `bogus`'),
        ('an opt-in that passes the formula', dict(notes_text=text.replace(victim_row, set_cell(set_cell(set_cell(set_cell(set_cell(set_cell(set_cell(victim_row, 'class', 'opt-in'), 'why', 'risk'), 's', '2'), 'save', '500'), 'g', '1'), 'w', '0'), 'r', '1'))), 'passes the default formula'),
        ('an opt-in whose why misses the term it fails', dict(notes_text=text.replace(victim_row, set_cell(set_cell(set_cell(set_cell(set_cell(set_cell(set_cell(victim_row, 'class', 'opt-in'), 'why', 'risk'), 's', '1.01'), 'save', '10'), 'g', '1'), 'w', '0'), 'r', '1'))), 'why must name'),
        ('a default that fails the formula', dict(notes_text=text.replace(victim_row, set_cell(set_cell(set_cell(set_cell(set_cell(set_cell(set_cell(victim_row, 'class', 'default'), 'default_why', 'x'), 's', '1.0'), 'save', '0'), 'g', '10'), 'w', '0'), 'r', '1'))), 'a default that fails the formula'),
        ('a formula cell that is no number', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 's', 'fast'))), 's is a number'),
        ('an opt-in with no why', dict(notes_text=text.replace(victim_row, set_cell(set_cell(victim_row, 'class', 'opt-in'), 'why', ''))), 'an opt-in needs why'),
        ('a benchmark that is no case', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'bench', 'no_such_case'))), 'bench no_such_case'),
        ('an n/a with no reason', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'gain', 'n/a'))), 'says n/a without the reason'),
        ('an empty when', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'when', ''))), 'no when'),
        ('a tbd default_why', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'default_why', 'tbd'))), 'no default_why'),
        ('an off that names no setting', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'off', 'SET sift.no_such_setting = off'))), 'sift.no_such_setting'),
        ('an empty gain', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'gain', ''))), 'no gain'),
        ('a test that does not exist', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'bench', 'test:no_such_test'))), 'no_such_test is no regress test'),
        ('a new option with no upgrade line', dict(notes_text=text.replace(fresh_row, set_cell(fresh_row, 'upgrade', '')), released_text=without_fresh), fresh + ': is new since the last release'),
        ('a column format minor with no upgrade_check entry', dict(srcs={**srcs, 'col/col.h': srcs['col/col.h'] + '\n#define SIFT_COL_FORMAT_ZZ 99\n'}), 'no entry for column format minor 99'),
        ('a figure that no row of the results table holds', dict(notes_text=text.replace(rows_vis, set_cell(rows_vis, 'gain', 'a key-cut range query 987.65 ms off'))), 'figures that no row of docs/results/options-evidence.tsv holds (987.65)'),
        ('a link to nowhere', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'link', 'NOWHERE.md#x'))), 'NOWHERE.md#x is not a file'),
    ]
    if not os.path.exists(os.path.join(ROOT, 'docs/results/options-evidence.tsv')):     # the stored results are not published: a checkout without them skips the case and says so
        print('options: selftest: the case "a figure that no row of the results table holds" is skipped, docs/results/options-evidence.tsv is not in this checkout')
        cases = [c for c in cases if not c[2].startswith('figures that no row of docs/results/')]
    for what, kw, want in cases:
        e = []
        load(e, **kw)
        if not any(want in x for x in e):
            errs.append('selftest: %s was not caught (wanted "%s", got %s)' % (what, want, e[:2]))
    # a changed default reaches the tables and is flagged against the release
    lead = lead_rows(notes)
    flip = next(o for o in opts if o['kind'] == 'guc' and o['type'] == 'bool' and not o['testing'] and o['key'] in released
                and not notes[o['key']]['upgrade']      # the first release after its snapshot has upgrade lines for the options that were new
                and (notes[o['key']]['class'] or lead[notes[o['key']]['group'] or o['key']]['class']) not in ('default', 'opt-in'))      # the class of those two follows the default
    changed = srcs[flip['where'].split(':')[0]]
    pat = re.compile(r'(DefineCustomBoolVariable\(\s*"%s".*?&\w+,\s*)(true|false)' % re.escape(flip['name']), re.S)
    new = pat.sub(lambda m: m.group(1) + ('false' if m.group(2) == 'true' else 'true'), changed, count=1)
    e = []
    o2, n2, r2 = load(e, srcs={**srcs, flip['where'].split(':')[0]: new})
    a, b = render(opts, notes, released), render(o2, n2, r2)
    if not any('has another default' in x for x in e):
        errs.append('selftest: a changed default of %s needs an upgrade line and the check did not say so' % flip['name'])
    e = [x for x in e if 'has another default' not in x]
    if e or a == b or ('was ' + fmt(flip, flip['default'])) not in b[1]:
        errs.append('selftest: a changed default of %s is not in the tables or not marked "was" (%s)' % (flip['name'], e[:1]))
    return errs
