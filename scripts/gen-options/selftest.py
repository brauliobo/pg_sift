# --selftest: proves missing rows, unknown rows, new C options, and bad links are caught.
def selftest():
    srcs = sources()
    errs = []
    opts, notes, released = load(errs)
    if errs:
        return errs
    text = read('docs/options.tsv')
    victim = next(o for o in opts if o['kind'] == 'guc' and not o['testing'] and o['key'] in notes)
    victim_row = next(l for l in text.split('\n') if l.startswith(victim['key'] + '\t'))

    def set_cell(row, col, v):
        f = row.split('\t')
        f += [''] * (len(NOTE_COLS) - len(f))
        f[NOTE_COLS.index(col)] = v
        return '\t'.join(f)

    cases = [
        ('a row is missing', dict(notes_text=text.replace(victim_row + '\n', '')), victim['key'] + ' has no row'),
        ('a row names no option', dict(notes_text=text + 'sift.no_such_setting\tall\t\tx\t\t6.0\n'), 'sift.no_such_setting is not an option'),
        ('a new setting', dict(srcs={**srcs, 'core/handler.c': srcs['core/handler.c'] + '\nvoid x(void){ DefineCustomBoolVariable("sift.zz_new", "s", NULL, &v, true, PGC_USERSET, 0, NULL, NULL, NULL); }'}),
         'sift.zz_new has no row'),
        ('a new reloption', dict(srcs={**srcs, 'col/am/options.c': srcs['col/am/options.c'] + '\nvoid x(void){ add_bool_reloption(col_relopt_kind, "zz_new", "d", true, ShareUpdateExclusiveLock); }'}),
         'sift_col(zz_new) has no row'),
        ('an isset member in a relopt_parse_elt', dict(srcs={**srcs, 'core/handler.c': srcs['core/handler.c'].replace('offsetof(SiftOptions, accent_folding)}', 'offsetof(SiftOptions, accent_folding), offsetof(SiftOptions, accent_folding_set)}')}),
         'has a member after the offset'),
        ('an empty purpose', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'purpose', ''))), 'purpose is empty'),
        ('a link to nowhere', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'link', 'NOWHERE.md#x'))), 'NOWHERE.md#x is not a valid'),
    ]
    for what, kw, want in cases:
        e = []
        load(e, **kw)
        if not any(want in x for x in e):
            errs.append('selftest: %s was not caught (wanted "%s", got %s)' % (what, want, e[:2]))
    return errs
