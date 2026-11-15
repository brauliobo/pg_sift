# The entry of scripts/gen-options.sh: runs the parts of this directory in one namespace, then the mode asked for.
# The parts are exec'd, not imported: they share the globals (mode, ROOT, errors) the single script had.
import os
import sys

PARTS = ['sources', 'notes', 'tables', 'live', 'selftest']
for part in PARTS:
    path = os.path.join(os.path.dirname(os.path.abspath(__file__)), part + '.py')
    with open(path, encoding='utf-8') as f:
        exec(compile(f.read(), path, 'exec'), globals())

errors = []
if mode == '--selftest':
    errors = selftest()
    if not errors:
        print('options: selftest ok')
elif mode == '--snapshot':
    opts, errs = collect(sources())
    errors += errs
    if not errors:
        print('# released: %s, the defaults of the last release (a release replaces this file with scripts/gen-options.sh --snapshot)\n# column format read: %d, the newest minor that release reads\n%s' % (release_label(), col_max_read(), snapshot(opts)))
else:
    opts, notes, released = load(errors)
    if mode == '--missing':
        m = missing(notes)
        for k, cells in m:
            print('%s\t%s' % (k, ' '.join(cells)))
        print('options: %d of %d rows have an unmeasured cell' % (len(m), len(notes)), file=sys.stderr)
    elif mode == '--live':
        errors += live(opts)
        if not errors:
            print('options: live ok, the library agrees with src/ on every setting and reloption')
    elif not errors:
        readme, settings, reloptions, cases, upgrade, diagnostics = render(opts, notes, released)
        files = {'README.md': [('sift-options', readme)], 'docs/USAGE.md': [('sift-settings', settings), ('sift-reloptions', reloptions)], 'docs/OPTIONS.md': [('sift-cases', cases), ('sift-upgrade', upgrade)], 'docs/TUNING.md': [('sift-diagnostics', diagnostics)]}
        for path, blocks in files.items():
            old = new = read(path)
            for tag, block in blocks:
                new = splice(new, tag, block, errors, path)
            if mode == '--check' and new != old:
                errors.append('%s is not what the sources generate: run scripts/gen-options.sh (make options)' % path)
            elif mode == 'write' and not errors:
                with open(os.path.join(ROOT, path), 'w', encoding='utf-8') as f:
                    f.write(new)
        if not errors:
            n = [o for o in opts if not o['testing']]
            print('options: ok, %d settings and %d reloptions listed' % (sum(o['kind'] == 'guc' for o in n), sum(o['kind'] == 'rel' for o in n)) +
                  ('; the figures of %d rows were not compared with their stored results (%s are not in this checkout)' % (len(SKIPPED), ', '.join(sorted(set(SKIPPED)))) if SKIPPED else ''))
if errors:
    for e in errors:
        print('options: ' + e, file=sys.stderr)
    sys.exit(1)
