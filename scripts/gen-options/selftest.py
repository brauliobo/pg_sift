# --selftest: proves missing rows, unknown rows, new C options, bad links, and missing or wrong replacements are caught.
def selftest():
    srcs = sources()
    collected = collect(srcs)   # parsed once: a case that edits only docs/options.tsv reuses it, a case that edits a source parses its own
    errs = []
    opts, notes, released = load(errs, collected=collected)
    if errs:
        return errs
    text = read('docs/options.tsv')
    victim = next(o for o in opts if o['kind'] == 'guc' and not o['testing'] and o['key'] in notes)
    victim_row = next(l for l in text.split('\n') if l.startswith(victim['key'] + '\t'))

    old_row = next(l for l in text.split('\n') if l.startswith('sift_col(key_cut)\t'))

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
        ('a new sift_table option', dict(srcs={**srcs, 'table/f2/options.c': srcs['table/f2/options.c'].replace('"skip_index",\n};', '"skip_index", "zz_opt",\n};', 1).replace('OPT_TEXT, OPT_TEXT,\n};', 'OPT_TEXT, OPT_TEXT, OPT_TEXT,\n};', 1)}),
         'sift_table(zz_opt) has no row'),
        ('a deprecated option without its replacement', dict(notes_text=text.replace(old_row, set_cell(old_row, 'replacement', ''))), 'replacement must be set'),
        ('a replacement the description does not say', dict(notes_text=text.replace(old_row, set_cell(old_row, 'replacement', 'preagg'))), 'replacement is preagg but src/ says key_order'),
        ('a replacement on an option that is not deprecated', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'replacement', 'x'))), 'replacement must be set'),
        ('an empty purpose', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'purpose', ''))), 'purpose is empty'),
        ('a link to nowhere', dict(notes_text=text.replace(victim_row, set_cell(victim_row, 'link', 'NOWHERE.md#x'))), 'NOWHERE.md#x is not a valid'),
    ]
    for what, kw, want in cases:
        e = []
        load(e, **kw) if 'srcs' in kw else load(e, collected=collected, **kw)
        if not any(want in x for x in e):
            errs.append('selftest: %s was not caught (wanted "%s", got %s)' % (what, want, e[:2]))
    return errs
